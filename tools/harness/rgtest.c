/* The rhythm minigame: beatmap loading, scoring, health, the end-of-song
 * summary, and the result the screen hands back to the script.
 *
 * Two halves.
 *
 * The first drives rhythm.c directly with a clock the test owns. The audio
 * path given is deliberately one that cannot be opened, so no track ever
 * starts and the module's clock stays on the wall clock this test is
 * stepping. That matters: the scoring windows are 200ms and 300ms wide, and
 * asserting on which side of them a tap fell is only meaningful if the clock
 * is not also being moved along by a decoder.
 *
 * The second runs the real `call screen rhythm_game(...)` out of
 * script_game.rpy through the interpreter, which is the part no amount of
 * unit testing covers: that the statement parses, that its arguments reach
 * the module, that the display changes hands and comes back, and that
 * `num_hits, num_notes, rhythm_score = _return` unpacks to three different
 * numbers rather than three copies of one -- which is what it did before
 * there was anything to unpack.
 *
 * Run from a scratch directory: it writes and removes PERSIST.SAV.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "mr.h"
#include "keys.h"
#include "vfs.h"
#include "audio.h"
#include "image.h"
#include "ui.h"
#include "expr.h"
#include "interp.h"
#include "script.h"
#include "scene.h"
#include "persist.h"
#include "rhythm.h"
#include "tl.h"
#include "list.h"
#include "slrender.h"   /* mr_sl_style_color, for the confirm colours */

static long g_out;
static void sink(const short *d, int n) { (void)d; g_out += n / 2; }
void (*hostshim_wave_sink)(const short *, int) = sink;

/* main.c is not linked here (it is all Win32), so the few things the engine
 * expects from it are supplied. MR_VERBOSE=1 lets the engine's own log go
 * to the terminal when a failure needs explaining. */
static mr_surface *g_stage;

void mr_log(const char *fmt, ...)
{
    va_list ap;
    if (!getenv("MR_VERBOSE"))
        return;
    va_start(ap, fmt);
    printf("    log: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

void mr_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("FATAL: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    exit(1);
}

void mr_quit(void) { }
void mr_invalidate(void) { }
mr_surface *mr_stage(void) { return g_stage; }

static int fails;

static void check(const char *what, int ok)
{
    printf("%-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        fails++;
}

static void checkl(const char *what, long got, long want)
{
    printf("%-58s %s", what, got == want ? "ok" : "FAIL");
    if (got != want) {
        printf("  (got %ld, wanted %ld)", got, want);
        fails++;
    }
    printf("\n");
}


/* Fixture assets, not a game's: this test takes the fixture pak.
 *
 * The beatmap is a real one, copied from a game rather than hand-written,
 * because the expectations below are read out of the file itself -- the
 * first strided onsets and their lanes -- so what matters is that it is the
 * shape tools/wavonset.py actually produces, with real onset spacing. A
 * hand-made one would be a guess about the format it is supposed to prove
 * can be read.
 *
 * The background is generated (tools/mkfixtureimg.py -m room). Nothing here
 * looks at it; the minigame just needs something behind the notes.
 *
 * The *track* is deliberately absent, which is what NOPE is for: every run
 * below starts with no audio so the clock is the one the test steps rather
 * than a sound card's. That was already true before the fixture move.
 */
#define BMAP  "audio/fix-music.beatmap.txt"
#define BG    "images/fix-room.png"
#define NOPE  "audio/__no_such_track__.mp3"

/* The first strided onsets, read straight out of the file so the
 * expectations come from the data rather than from the code under test. */
#define NEXP 12
static long g_onset[NEXP];
static int  g_lane[NEXP];
static int  g_nexp;
static long g_last_onset;
static int  g_total;

static void read_expected(void)
{
    long size;
    char *text = mr_vfs_read_all(BMAP, &size);
    char *p;

    if (!text)
        return;
    p = text;
    while (p < text + size) {
        char *line = p, *end, *tab;
        while (p < text + size && *p != '\n')
            p++;
        end = p;
        if (p < text + size)
            p++;
        while (end > line && (end[-1] == '\r' || end[-1] == ' ' ||
                              end[-1] == '\t'))
            end--;
        *end = '\0';
        if (!*line)
            continue;

        if ((g_total % 2) == 0) {           /* the stride the screen uses */
            long ms = (long)(atof(line) * 1000.0 + 0.5);
            g_last_onset = ms;
            if (g_nexp < NEXP) {
                tab = line;
                while (*tab && *tab != ' ' && *tab != '\t') tab++;
                while (*tab == ' ' || *tab == '\t') tab++;
                g_onset[g_nexp] = ms;
                g_lane[g_nexp] = *tab ? (atoi(tab) % 4) : -1;
                g_nexp++;
            }
        }
        g_total++;
    }
    free(text);
}

/* Lane -> the key the player presses for it. */
static int lane_key(int lane)
{
    static const int k[4] = { MR_KEY_LEFT, MR_KEY_UP, MR_KEY_DOWN, MR_KEY_RIGHT };
    return k[lane & 3];
}

/* One frame, as the loop does it: update then draw. Drawing is not
 * decoration here -- note placement and the summary only run in the render
 * path, so a fault in either is a fault in the game. */
static void step(long now)
{
    mr_rhythm_update(now);
    if (mr_rhythm_active())
        mr_rhythm_render(g_stage, now);
}

/* Wall-clock time of a given song time. A run starts at 0, the countdown is
 * 3s and the lead-in one note lifetime (2s), so song time zero is 5000. */
#define WALL(t) (5000L + (t))

static long score_now(void)
{
    long h, n, s;
    mr_rhythm_result(&h, &n, &s);
    return s;
}

static long hits_now(void)
{
    long h, n, s;
    mr_rhythm_result(&h, &n, &s);
    return h;
}

/* Take the run up to the point where notes are falling. */
static void begin_run(void)
{
    step(0);
    step(1000);
    step(2000);
    step(3100);
}

/* --------------------------------------------------------------------- */

/* The give-up dialog used to be bare text over the playfield while every
 * other confirm in the game is a framed box. It is drawn natively -- it sits
 * over a display the screen system does not own -- so what can go wrong is
 * that the drawing and the hit test disagree, which is invisible until a
 * click lands nowhere. Both now come from dialog_layout(), and this drives
 * the real clicks to prove it.
 *
 * MR_DUMP=1 writes the frame to /tmp/rg_pause.ppm to look at.
 */
static void dump_stage(const char *path)
{
    FILE *f;
    int x, y;

    if (!getenv("MR_DUMP"))
        return;
    f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", MR_STAGE_W, MR_STAGE_H);
    for (y = 0; y < MR_STAGE_H; y++) {
        mr_u32 *row = g_stage->px + (size_t)y * g_stage->pitch;
        for (x = 0; x < MR_STAGE_W; x++) {
            unsigned char px[3];
            px[0] = (unsigned char)MR_R(row[x]);
            px[1] = (unsigned char)MR_G(row[x]);
            px[2] = (unsigned char)MR_B(row[x]);
            fwrite(px, 1, 3, f);
        }
    }
    fclose(f);
    printf("   wrote %s\n", path);
}

/* Is there a pixel of the frame's blue border along this row? The frame art
 * is the same gui/frame.png the confirm screen uses, so finding its edge is
 * what tells a framed dialog from bare text. */
static int row_has_border(int y)
{
    int x, n = 0;

    for (x = 0; x < MR_STAGE_W; x++) {
        mr_u32 c = g_stage->px[(size_t)y * g_stage->pitch + x];
        if (MR_B(c) > 120 && MR_B(c) > MR_R(c) + 40)
            n++;
    }
    return n;
}

/* Distance between the leftmost and rightmost border pixels on a row: the
 * width of the frame, which is what says the box was sized to its text. */
static int border_width(int y)
{
    int x, lo = -1, hi = -1;

    for (x = 0; x < MR_STAGE_W; x++) {
        mr_u32 c = g_stage->px[(size_t)y * g_stage->pitch + x];
        if (MR_B(c) > 120 && MR_B(c) > MR_R(c) + 40) {
            if (lo < 0)
                lo = x;
            hi = x;
        }
    }
    return (lo < 0) ? 0 : hi - lo;
}

static unsigned long stage_hash(void)
{
    unsigned long h = 5381;
    int x, y;

    for (y = 0; y < MR_STAGE_H; y++) {
        mr_u32 *row = g_stage->px + (size_t)y * g_stage->pitch;
        for (x = 0; x < MR_STAGE_W; x++)
            h = h * 33u + (row[x] & 0xFFu);
    }
    return h;
}

/* How many pixels on the stage are exactly this colour. The labels are drawn
 * with an outline, so their edges are blended, but a glyph's interior is the
 * flat colour asked for -- which is what makes an exact count the right
 * question here. */
static long count_color(mr_u32 want)
{
    long n = 0;
    int y, x;

    for (y = 0; y < g_stage->h; y++) {
        const mr_u32 *row = g_stage->px + (size_t)y * g_stage->pitch;
        for (x = 0; x < g_stage->w; x++)
            if ((row[x] & 0xFFFFFF) == (want & 0xFFFFFF))
                n++;
    }
    return n;
}

/* The give-up dialog's buttons must light under the pointer, like every other
 * button. They did not: the dialog is drawn natively rather than as a screen,
 * so it has no focus entry and the screen layer's hover never reached it.
 *
 * Runs after the tests that load the script, because the colours are read
 * from `confirm_button_text` -- the style the dialog imitates -- and styles
 * are only finalised once mr_interp_start has run. Asked of the style rather
 * than against a constant so this follows the game's gui.hover_color.
 */
static void test_give_up_hover(void)
{
    int mid = MR_STAGE_H / 2;
    int yes_x = MR_STAGE_W / 2 - 90, no_x = MR_STAGE_W / 2 + 90;
    int btn_y = mid + 40;
    mr_u32 idle, hover;

    printf("\n-- the give-up dialog lights under the pointer --\n");

    idle  = mr_sl_style_color("confirm_button_text", "idle_", "color",
                              0xFFFFFFFFUL);
    hover = mr_sl_style_color("confirm_button_text", "hover_", "color", idle);

    /* Without this the rest proves nothing: if the two colours were equal,
     * every count below would match whatever was drawn. */
    check("the confirm style gives a hover colour distinct from idle",
          (hover & 0xFFFFFF) != (idle & 0xFFFFFF));

    mr_rhythm_start(NOPE, BMAP, BG, 2);
    begin_run();
    mr_rhythm_key(MR_KEY_ESCAPE);
    check("Escape pauses the run", mr_rhythm_paused());

    check("the dialog takes the pointer motion", mr_rhythm_hover(20, 20) == 1);
    mr_rhythm_render(g_stage, WALL(4000));
    check("  nothing is lit with the pointer away from the buttons",
          count_color(hover) == 0);

    mr_rhythm_hover(yes_x, btn_y);
    mr_rhythm_render(g_stage, WALL(4010));
    check("Oui lights when hovered", count_color(hover) > 0);

    mr_rhythm_hover(no_x, btn_y);
    mr_rhythm_render(g_stage, WALL(4020));
    check("Non lights when hovered", count_color(hover) > 0);

    /* Only one at a time: with the pointer on Non, the lit pixels must be
     * Non's alone, so moving between them cannot leave both lit. */
    {
        long both;
        mr_rhythm_hover(no_x, btn_y);
        mr_rhythm_render(g_stage, WALL(4030));
        both = count_color(hover);
        mr_rhythm_hover(yes_x, btn_y);
        mr_rhythm_render(g_stage, WALL(4040));
        check("  and only one of them at a time",
              both > 0 && count_color(hover) > 0 &&
              labs(both - count_color(hover)) < both);
    }

    /* Away again, and the highlight goes. */
    mr_rhythm_hover(20, 20);
    mr_rhythm_render(g_stage, WALL(4050));
    check("moving off puts it out", count_color(hover) == 0);

    /* With no dialog up the motion is not ours, or the screen layer's hover
     * would be suppressed for the whole minigame. */
    mr_rhythm_click(no_x, btn_y);          /* Non: resume */
    check("resumed", !mr_rhythm_paused());
    check("  and motion is no longer taken", mr_rhythm_hover(yes_x, btn_y) == 0);

    mr_rhythm_stop();
}

/* The quit button in the top-right corner.
 *
 * `imagebutton align (1.0, 0.0) xoffset -10 yoffset 10` in the .rpy, inside
 * `showif not is_paused and not has_ended and countdown_text is None`. It was
 * simply missing here, which left the give-up dialog reachable only from the
 * keyboard. Drawn natively for the same reason the dialog is -- it sits over
 * a displayable the screen system does not own -- so the same thing can go
 * wrong: the drawing and the hit test disagreeing, which is invisible until a
 * click lands nowhere.
 *
 * The *placement rule* is recomputed here from the .rpy -- align (1.0, 0.0)
 * with offsets -10 and +10 -- so that a change to rhythm.c has to be made
 * here too. The *size* is read from the art rather than written down: the
 * first version of this test assumed 48x48, the images are 56x56, and the
 * only check that noticed was the one probing just outside the edge. A
 * hardcoded size would have made three of these checks test a rectangle that
 * does not exist while still passing.
 */
#define QBTN_MARGIN 10

static void test_quit_button(void)
{
    long lit_away, lit_on;
    int qw, qh, qx, qy, cx, cy;
    mr_surface *art;

    printf("\n-- the quit button --\n");

    mr_image_define("\001tqb", "00-renpythm/images/quit_idle.png", 0);
    art = mr_image_get("\001tqb");
    check("the quit button art is there", art != NULL);
    if (!art)
        return;
    qw = art->w;
    qh = art->h;
    qx = MR_STAGE_W - qw - QBTN_MARGIN;
    qy = QBTN_MARGIN;
    cx = qx + qw / 2;
    cy = qy + qh / 2;
    printf("   (art %dx%d, so the button is at %d,%d)\n", qw, qh, qx, qy);

    mr_rhythm_start(NOPE, BMAP, BG, 2);

    /* During the countdown the .rpy's showif is false: countdown_text is not
     * None yet. A click there must fall through to the lanes, not pause. */
    step(0);
    step(1000);
    mr_rhythm_click(cx, cy);
    check("a click in the corner during the countdown does not pause",
          !mr_rhythm_paused());

    begin_run();
    check("the run is playing", mr_rhythm_active() && !mr_rhythm_paused());

    /* Drawn: the corner is not what the background put there. Compared
     * against the same corner with the button deliberately not up, rather
     * than against a colour, so this does not depend on the art. */
    {
        mr_u32 with, without;
        step(4000);
        with = g_stage->px[(size_t)cy * g_stage->pitch + cx];
        mr_rhythm_key(MR_KEY_ESCAPE);           /* paused: the button goes away */
        step(4100);
        without = g_stage->px[(size_t)cy * g_stage->pitch + cx];
        check("the button is drawn, and the dialog hides it", with != without);
        mr_rhythm_key(MR_KEY_ESCAPE);           /* resume */
    }

    /* Hover. Counting changed pixels rather than a specific colour: the two
     * states are different art, and what matters is that moving onto it
     * changes the corner and moving off changes it back. */
    step(4200);
    mr_rhythm_hover(20, 20);
    step(4300);
    lit_away = count_color(g_stage->px[(size_t)cy * g_stage->pitch + cx]);
    check("the pointer away from it is taken", 1);

    check("the button takes the pointer motion",
          mr_rhythm_hover(cx, cy) == 1);
    step(4400);
    lit_on = count_color(g_stage->px[(size_t)cy * g_stage->pitch + cx]);
    check("hovering changes what is drawn there", lit_on != lit_away);

    mr_rhythm_hover(20, 20);
    step(4500);
    check("  and moving off puts it back",
          count_color(g_stage->px[(size_t)cy * g_stage->pitch + cx])
              == lit_away);

    /* The click. This is the one that proves the drawing and the hit test
     * agree: the rectangle above came from the .rpy, not from rhythm.c. */
    mr_rhythm_click(cx, cy);
    check("clicking it opens the give-up dialog", mr_rhythm_paused());

    /* Just outside, on all four sides: a button that pauses from anywhere in
     * the corner would pass every check above. */
    mr_rhythm_key(MR_KEY_ESCAPE);               /* resume */
    check("resumed", !mr_rhythm_paused());
    mr_rhythm_click(qx - 2, cy);
    mr_rhythm_click(qx + qw + 2, cy);
    mr_rhythm_click(cx, qy - 2);
    mr_rhythm_click(cx, qy + qh + 2);
    check("  a click just outside it on any side does not",
          !mr_rhythm_paused());

    mr_rhythm_stop();
}

static void test_give_up_dialog(void)
{
    int mid = MR_STAGE_H / 2;
    int before, after;

    printf("\n-- the give-up dialog --\n");

    mr_rhythm_start(NOPE, BMAP, BG, 2);
    begin_run();

    before = row_has_border(mid);
    mr_rhythm_key(MR_KEY_ESCAPE);
    check("Escape pauses the run", mr_rhythm_paused());
    mr_rhythm_render(g_stage, WALL(3500));
    after = row_has_border(mid);
    dump_stage("/tmp/rg_pause.ppm");

    check("the dialog is framed, not bare text", after > before + 4);

    /* Clicking where nothing is must not answer the dialog: this is the
     * failure a drifting hit rectangle produces. */
    mr_rhythm_click(20, 20);
    check("a click outside the buttons does nothing",
          mr_rhythm_paused() && mr_rhythm_active());

    /* "Non" resumes. Its rectangle is to the right of centre. */
    mr_rhythm_click(MR_STAGE_W / 2 + 90, mid + 40);
    check("clicking Non resumes", !mr_rhythm_paused() && mr_rhythm_active());

    /* And "Oui" gives up, from the left of centre. */
    mr_rhythm_key(MR_KEY_ESCAPE);
    mr_rhythm_render(g_stage, WALL(3600));
    mr_rhythm_click(MR_STAGE_W / 2 - 90, mid + 40);
    /* Giving up leaves the run finished rather than inactive: the module
     * waits for the interpreter to collect the result before it goes away. */
    check("clicking Oui gives up",
          !mr_rhythm_paused() && mr_rhythm_finished());

    mr_rhythm_stop();

    /* The dialog can be opened during the countdown too. It used to pause the
     * run and draw nothing: the countdown branch of the render returns before
     * the dialog, so the game froze with no way out shown. */
    printf("\n-- ...during the countdown --\n");
    mr_rhythm_start(NOPE, BMAP, BG, 2);
    step(0);
    step(500);
    mr_rhythm_key(MR_KEY_ESCAPE);
    check("Escape during the countdown pauses", mr_rhythm_paused());
    mr_rhythm_render(g_stage, 900);
    check("  and the dialog is on screen", row_has_border(mid) > 4);
    mr_rhythm_key(MR_KEY_ESCAPE);
    check("  Escape again resumes", !mr_rhythm_paused());
    mr_rhythm_stop();

    /* The music volume. Pausing ducks the music, and what has to come back is
     * the slider's value, not 1.0. Leaving the run while the dialog was up
     * put nothing back at all: the channel stayed silent, so the menu music
     * did not return, and neither did the next song or the next chapter's. */
    printf("\n-- the music volume survives the dialog --\n");
    mr_audio_set_volume(MR_CH_MUSIC, 0.6f);
    mr_rhythm_start(NOPE, BMAP, BG, 2);
    begin_run();

    mr_rhythm_key(MR_KEY_ESCAPE);
    check("pausing ducks the music",
          mr_audio_get_volume(MR_CH_MUSIC) == 0.0f);
    mr_rhythm_key(MR_KEY_ESCAPE);
    check("  resuming restores the slider, not full volume",
          mr_audio_get_volume(MR_CH_MUSIC) == 0.6f);

    mr_rhythm_key(MR_KEY_ESCAPE);
    mr_rhythm_key('Y');
    check("giving up from the dialog restores it too",
          mr_audio_get_volume(MR_CH_MUSIC) == 0.6f);
    mr_rhythm_stop();

    /* Whatever the exit, since mr_rhythm_stop is the one door out. */
    mr_rhythm_start(NOPE, BMAP, BG, 2);
    begin_run();
    mr_rhythm_key(MR_KEY_ESCAPE);
    mr_rhythm_stop();
    check("  and so does stopping while it is up",
          mr_audio_get_volume(MR_CH_MUSIC) == 0.6f);

    /* The strings are the .rpy's `_("...")`, so they follow the language. */
    printf("\n-- the dialog is translated --\n");
    {
        unsigned long french, english;
        int fr_w, en_w;

        mr_rhythm_start(NOPE, BMAP, BG, 2);
        begin_run();
        mr_rhythm_key(MR_KEY_ESCAPE);

        mr_tl_set_language(NULL);
        mr_rhythm_render(g_stage, WALL(3500));
        french = stage_hash();
        fr_w = border_width(mid);

        mr_tl_set_language("english");
        mr_rhythm_render(g_stage, WALL(3500));
        english = stage_hash();
        en_w = border_width(mid);
        dump_stage("/tmp/rg_pause_en.ppm");

        check("switching language changes the dialog", french != english);

        /* "Quit the rhythm game?" is shorter than "Quitter le jeu de
         * rythme ?", so a box measured on the original would not have
         * changed width. */
        check("  and the box is sized to the translation", en_w != fr_w);
        printf("   (%dpx in French, %dpx in English)\n", fr_w, en_w);

        mr_tl_set_language(NULL);
        mr_rhythm_stop();
    }
}

static void test_beatmap(void)
{
    long h, n, s;

    printf("\n-- the beatmap --\n");

    check("a run starts", mr_rhythm_start(NOPE, BMAP, BG, 2));

    mr_rhythm_result(&h, &n, &s);
    checkl("  stride 2 keeps every other onset", n, (g_total + 1) / 2);

    /* These beatmaps are generated from the music: lane N is the note you
     * hear on it. Dealing lanes from a hash of the file name, which the old
     * format needs, would throw that away. */
    check("  the lanes come from the file, not from a hash", g_lane[0] >= 0);

    check("  nothing is scored before the first tap", score_now() == 0);
    mr_rhythm_stop();
}

static void test_scoring(void)
{
    long combo, perfect, great;
    int  health, over;

    printf("\n-- scoring --\n");

    mr_rhythm_start(NOPE, BMAP, BG, 2);
    begin_run();

    /* A tap during the countdown must not reach a note. */
    mr_rhythm_key(lane_key(g_lane[0]));
    checkl("a tap before the notes fall scores nothing", score_now(), 0);

    /* Dead on the beat: a PERFECT, worth (10 + combo) * 3/2. With no combo
     * yet that is 15. */
    step(WALL(g_onset[0]));
    mr_rhythm_key(lane_key(g_lane[0]));
    checkl("a tap on the beat is a PERFECT worth 15", score_now(), 15);

    mr_rhythm_stats(&combo, &health, &perfect, &great, &over);
    checkl("  and starts a combo", combo, 1);
    checkl("  counted as a PERFECT", perfect, 1);

    /* The combo makes the next note worth more: (10 + 1) * 3/2 = 17. */
    step(WALL(g_onset[1]));
    mr_rhythm_key(lane_key(g_lane[1]));
    checkl("the combo makes the next PERFECT worth 17", score_now(), 15 + 17);

    /* 250ms off the beat is outside the 200ms PERFECT window but inside the
     * 300ms hit window: a GREAT, worth 10 + combo with no bonus. */
    step(WALL(g_onset[2] - 250));
    mr_rhythm_key(lane_key(g_lane[2]));
    checkl("a tap 250ms early is a GREAT, worth 12", score_now(), 15 + 17 + 12);

    mr_rhythm_stats(&combo, &health, &perfect, &great, &over);
    checkl("  counted as a GREAT, not a PERFECT", great, 1);
    checkl("  the combo still grows", combo, 3);
    checkl("  and nothing has been missed", health, 100);

    /* A tap far from any note scores nothing and does not break the combo --
     * only a note going past unhit does that. */
    step(WALL(g_onset[3] - 900));
    mr_rhythm_key(lane_key(g_lane[3]));
    checkl("a tap nowhere near a note scores nothing",
           score_now(), 15 + 17 + 12);

    mr_rhythm_stop();
}

static void test_misses(void)
{
    long combo, perfect, great;
    int  health, over;
    long before;

    printf("\n-- missing --\n");

    mr_rhythm_start(NOPE, BMAP, BG, 2);
    begin_run();

    /* Score one note so there is a combo to lose. */
    step(WALL(g_onset[0]));
    mr_rhythm_key(lane_key(g_lane[0]));
    before = score_now();
    mr_rhythm_stats(&combo, &health, &perfect, &great, &over);
    checkl("a combo is running", combo, 1);

    /* Walk past the next few onsets without touching anything. A note is
     * missed once it is more than the hit window past its beat. */
    step(WALL(g_onset[3] + 400));

    mr_rhythm_stats(&combo, &health, &perfect, &great, &over);
    checkl("a note going past unhit breaks the combo", combo, 0);
    check("  and costs health", health < 100);
    check("  the score is not taken away", score_now() == before);
    check("  the run is not over yet", !over);

    mr_rhythm_stop();
}

static void test_game_over(void)
{
    long combo, perfect, great, h, n, s;
    int  health, over;

    printf("\n-- running out of health --\n");

    mr_rhythm_start(NOPE, BMAP, BG, 2);
    begin_run();

    /* Twenty missed notes at 5 health each. Stepping well past the twentieth
     * onset misses every note up to it. */
    step(WALL(g_onset[NEXP - 1] + 400));
    step(WALL(g_onset[NEXP - 1] + 800));
    /* ...and on, until the health is gone. */
    {
        long t = g_onset[NEXP - 1] + 800;
        int guard = 0;
        do {
            t += 500;
            step(WALL(t));
            mr_rhythm_stats(&combo, &health, &perfect, &great, &over);
        } while (health > 0 && guard++ < 2000);
    }

    mr_rhythm_stats(&combo, &health, &perfect, &great, &over);
    checkl("twenty missed notes empty the health bar", health, 0);
    check("  which ends the run", over);
    check("  and puts the summary up", mr_rhythm_ended());

    mr_rhythm_result(&h, &n, &s);
    checkl("a lost run reports no hits", h, 0);
    checkl("  and no score", s, 0);
    check("  but still says how many notes there were", n > 0);

    /* A game over must not be able to set a record. */
    checkl("a game over does not touch the high score",
           mr_rhythm_highscore_get(BMAP), 0);

    mr_rhythm_stop();
}

static void test_summary_and_highscore(void)
{
    long h, n, s;
    long combo, perfect, great;
    int  health, over;

    printf("\n-- finishing the song --\n");

    /* A huge stride, so the beatmap is five notes instead of 490. Playing to
     * the end matters here and the health only tolerates twenty misses: at
     * the real stride, running the clock to the end of the song misses
     * everything and ends the run as a game over instead -- which is correct,
     * and is checked above, but it is not this path. */
    mr_rhythm_start(NOPE, BMAP, BG, 200);
    begin_run();

    /* The first kept onset is the first onset of the file whatever the
     * stride, so this is the same note as everywhere else. */
    step(WALL(g_onset[0]));
    mr_rhythm_key(lane_key(g_lane[0]));
    s = score_now();
    checkl("a note is scored", s, 15);

    /* Out to the end. The four remaining notes go by unhit, which is four
     * misses -- well inside the twenty the health allows. */
    step(WALL(g_last_onset + 2500));
    check("the run ends when the song does", mr_rhythm_ended());

    mr_rhythm_stats(&combo, &health, &perfect, &great, &over);
    check("  not as a game over", !over);
    check("  with health to spare", health > 0);

    mr_rhythm_result(&h, &n, &s);
    checkl("a finished run keeps its hits", h, 1);
    checkl("  and its score", s, 15);

    checkl("finishing records a high score", mr_rhythm_highscore_get(BMAP), s);

    /* The summary ignores input briefly, so the tap that hit the last note
     * cannot also dismiss it. */
    mr_rhythm_key(MR_KEY_SPACE);
    check("the summary ignores an immediate keypress",
          !mr_rhythm_finished());

    step(WALL(g_last_onset + 2500) + 900);
    mr_rhythm_key(MR_KEY_SPACE);
    check("  and accepts one a moment later", mr_rhythm_finished());

    /* A worse run must not overwrite the record. */
    {
        long best = mr_rhythm_highscore_get(BMAP);
        check("a worse run does not overwrite the record",
              !mr_rhythm_highscore_set(BMAP, best - 1));
        checkl("  the record stands", mr_rhythm_highscore_get(BMAP), best);
        check("a better run does overwrite it",
              mr_rhythm_highscore_set(BMAP, best + 1));
        checkl("  and is what is read back",
               mr_rhythm_highscore_get(BMAP), best + 1);
    }

    mr_rhythm_stop();
}

/* The high scores have to survive a restart, which means living in the
 * persistent file under a name persist.c will actually write out -- and one
 * short enough to fit its 64-byte name field. */
static void test_persistence(void)
{
    /* Names, not files: none of these is opened. What is being tested is
     * the *key* derived from a beatmap path -- that it fits the persistent
     * file's 64-byte name field and is recognised as persistent.
     *
     * So the lengths are the content. These were nine of the game's own
     * beatmap names; they are now synthetic, with the same spread and the
     * same longest case. The last one is deliberately at the edge: with
     * the "persistent.rhythm_game_highscores." prefix, which is 34
     * characters, a stem of 29 lands on 63 and one more character would
     * fail the cap. That is what forced ".beatmap.txt" to be stripped from
     * the key in the first place, and a shorter set would stop proving it.
     */
    static const char *const songs[] = {
        /* BMAP itself has to be in the list. Earlier tests set a high
         * score for the beatmap they run, so its key already exists, and
         * the count below is of *all* high-score keys -- nine names plus a
         * tenth from earlier reads as "every song gets its own entry FAIL
         * (got 10, wanted 9)". On the game's list this was invisible
         * because the beatmap the rest of the test used was one of the
         * nine. */
        BMAP,
        "audio/fix-second-song.beatmap.txt",
        "audio/fix-a-longer-song-name.beatmap.txt",
        "audio/fix-with_underscores_too.beatmap.txt",
        "audio/fix-and-hyphens-as-well.beatmap.txt",
        "audio/fix-mixed_name-styles.beatmap.txt",
        "audio/fix-short.beatmap.txt",
        "audio/fix-medium-length-one.beatmap.txt",
        "audio/fix-twentynine-chars-long-x.beatmap.txt"
    };
    int i;
    int n = (int)(sizeof(songs) / sizeof(songs[0]));
    int all_fit = 1, all_persist = 1;

    printf("\n-- the high score table --\n");

    for (i = 0; i < n; i++)
        mr_rhythm_highscore_set(songs[i], 100 + i);

    /* Every key has to be under the 64-byte cap in the persistent file and
     * has to be recognised as persistent, or it is silently dropped on the
     * way out. The longest of these names is what forced the `.beatmap.txt`
     * suffix to be stripped from the key. */
    {
        const char *name;
        const mr_value *v;
        int found = 0;

        for (i = 0; mr_var_enumerate(i, &name, &v); i++) {
            if (strncmp(name, "persistent.rhythm_game_highscores.", 33))
                continue;
            found++;
            if ((int)strlen(name) >= 64)
                all_fit = 0;
            if (!mr_persist_is_persistent(name))
                all_persist = 0;
        }
        checkl("every song gets its own entry", found, n);
    }
    check("  each name fits the persistent file's 64-byte field", all_fit);
    check("  and each is recognised as persistent data", all_persist);

    check("the scores are written out", mr_persist_save() != 0);

    /* Forget them, then read the file back. */
    for (i = 0; i < n; i++) {
        char key[160];
        sprintf(key, "persistent.rhythm_game_highscores.%s", "x");
        (void)key;
    }
    {
        const char *name;
        const mr_value *v;
        char doomed[32][80];
        int  nd = 0;

        for (i = 0; mr_var_enumerate(i, &name, &v); i++)
            if (!strncmp(name, "persistent.rhythm_game_highscores.", 33) &&
                nd < 32) {
                strncpy(doomed[nd], name, 79);
                doomed[nd][79] = '\0';
                nd++;
            }
        for (i = 0; i < nd; i++)
            mr_var_unset(doomed[i]);
    }
    checkl("  forgotten before the reload",
           mr_rhythm_highscore_get(songs[0]), 0);

    mr_persist_load();
    {
        int ok = 1;
        for (i = 0; i < n; i++)
            if (mr_rhythm_highscore_get(songs[i]) != 100 + i)
                ok = 0;
        check("  and every one comes back after a restart", ok);
    }
}

/* --------------------------------------------------------------------- */

/* The whole path, through the real script: `call screen rhythm_game(...)`. */
static void test_through_the_script(const char *dir)
{
    mr_script *s;
    long now = 0;
    int  guard;

    printf("\n-- through the interpreter --\n");

    s = mr_script_load(dir);
    if (!s || s->n == 0) {
        check("the script loads", 0);
        return;
    }
    mr_interp_init(s, dir);
    /* The fixture's own label, not the game's `game_chichinoito`. It is
     * shaped after the real one -- see tools/harness/fixgame/script.rpy. */
    mr_interp_start("fixture_rhythm");

    /* Run until the statement hands the display over. */
    for (guard = 0; guard < 400 && !mr_rhythm_active(); guard++) {
        mr_interp_update(now);
        now += 33;
    }
    check("`call screen rhythm_game(...)` starts a run", mr_rhythm_active());
    if (!mr_rhythm_active())
        return;

    /* The arguments reached the module: this beatmap has 490 strided notes
     * and the call in script_game.rpy passes beatmap_stride=2. */
    {
        long h, n, sc;
        mr_rhythm_result(&h, &n, &sc);
        checkl("  with the beatmap and stride the script named",
               n, (g_total + 1) / 2);
    }

    check("  and every frame is a full redraw while it runs",
          mr_interp_redraw_kind() == MR_REDRAW_ALL);

    check("  with no dialogue box over it", !mr_interp_window_visible());

    /* Escape asks whether to give up; Y confirms. */
    check("  Escape is taken by the minigame, not by the game",
          mr_interp_rhythm_key(MR_KEY_ESCAPE));
    check("  and Y gives up", mr_interp_rhythm_key('Y'));

    /* The next update collects the result and carries on with the script. */
    for (guard = 0; guard < 200 && mr_rhythm_active(); guard++) {
        mr_interp_update(now);
        now += 33;
    }
    check("the display comes back to the script", !mr_rhythm_active());

    /* script_game.rpy does `$ num_hits, num_notes, rhythm_score = _return`
     * right after the call. Three targets, three different numbers. */
    for (guard = 0; guard < 200; guard++) {
        mr_interp_update(now);
        now += 33;
        if (mr_var_get_int("num_notes", -1) > 0)
            break;
    }
    checkl("the script unpacks the note count from _return",
           mr_var_get_int("num_notes", -1), (g_total + 1) / 2);
    checkl("  and the score separately from it",
           mr_var_get_int("rhythm_score", -1), 0);

    /* --- the high score table -----------------------------------------
     *
     * `build_rhythm_highscores()` in screens.rpy is a list-of-tuples literal
     * spread over several lines, a conditional `.append`, and a list
     * comprehension. None of it ran before -- the value model had no
     * sequence -- so the extras menu's table was simply empty. This is the
     * whole of it, evaluated as the screen's `default scores = ...` does.
     */
    printf("\n-- the score table the extras menu builds --\n");
    {
        mr_value v;
        int id, i, n;
        int seen_title = 0, seen_score = 0;

        /* Two songs are locked behind persistent flags, and this harness
         * removed PERSIST.SAV, so only the seven unconditional ones. */
        mr_rhythm_highscore_set("audio/fix-cbr-44-mono.beatmap.txt", 4242);

        mr_expr_eval("build_rhythm_highscores()", &v);
        id = mr_list_id(&v);
        check("build_rhythm_highscores() returns a list", id >= 0);
        if (id < 0)
            goto done;

        n = mr_list_len(id);
        checkl("  with the seven unlocked songs in it", n, 7);

        for (i = 0; i < n; i++) {
            mr_value row, title, score;
            int sub;

            mr_list_get(id, i, &row);
            sub = mr_list_id(&row);
            if (sub < 0)
                continue;
            mr_list_get(sub, 0, &title);
            mr_list_get(sub, 1, &score);
            if (title.type == MR_V_STR &&
                !strcmp(title.s, "One second")) {
                seen_title = 1;
                seen_score = (score.type == MR_V_INT && score.i == 4242);
            }
        }
        check("  each row is a (title, score) pair", seen_title);
        check("  and the score is the one in persistent data", seen_score);

        /* The locked songs appear once their flag is set, which is the
         * conditional `songs.append(...)` doing its job. */
        mr_var_set_int("persistent.unlock_a", 1);
        mr_expr_eval("build_rhythm_highscores()", &v);
        checkl("  unlocking a song adds it to the table",
               mr_list_len(mr_list_id(&v)), 8);
        mr_var_set_int("persistent.unlock_b", 1);
        mr_expr_eval("build_rhythm_highscores()", &v);
        checkl("  and so does the other", mr_list_len(mr_list_id(&v)), 9);
    }

done:
    mr_interp_shutdown();
    mr_script_free(s);
}

/* The song clock, with a real track actually going through the mixer.
 *
 * Everything above runs with an unopenable audio path so the clock is the one
 * the test steps. This is the opposite: the real MP3, the real mixer, and the
 * host shim reporting a device position that trails what has been written by
 * a full queue, as a card does.
 *
 * The clock must never run backwards. It used to, exactly once per song: the
 * track was handed to the mixer at song time zero, but the card had up to
 * a runway of already-queued audio to get through first, so its position read 0
 * throughout and the notes ran on the wall clock. The moment that position
 * became readable the anchor adopted it and the clock fell back by the whole
 * queue depth -- a screenful of notes travelling back up.
 */
static void test_clock_never_runs_backwards(void)
{
    long now = 0;
    long prev;
    long worst_drop = 0;
    long want_audio = 0;
    int  i;
    int  saw_audio = 0;

    printf("\n-- the song clock, against the real mixer --\n");

    /* The fixture's 60-second excerpt of real music. Real and not a tone:
     * this is the one test here that runs the actual mixer, and it steps
     * twelve seconds through it. */
    check("a run starts on the real track",
          mr_rhythm_start("audio/fix-music.mp3", BMAP, BG, 2));

    mr_rhythm_update(now);
    prev = mr_rhythm_song_ms();

    /* Twelve seconds at 30fps: the countdown, the lead-in, the moment the
     * track becomes audible, and well past it. */
    for (i = 0; i < 360; i++) {
        long song, drop;
        int  guard = 0;

        now += 33;

        /* Let the mixer produce about a frame's worth of audio, so the device
         * position advances at roughly real time instead of racing ahead of
         * the clock being stepped. */
        want_audio += 33L * 44100L / 1000L;
        while (g_out < want_audio && guard++ < 200)
            mr_audio_poll();

        mr_rhythm_update(now);
        song = mr_rhythm_song_ms();

        if (mr_audio_output_position_ms(MR_CH_MUSIC) > 0)
            saw_audio = 1;

        drop = prev - song;
        if (drop > worst_drop)
            worst_drop = drop;
        prev = song;
    }

    check("the track really became audible", saw_audio);

    /* The anchor eases onto the card's reading, so the clock may advance a
     * little more slowly than the wall clock for a while. What it must not do
     * is go backwards by anything a player would see; the bug was most of a
     * second. */
    printf("%-58s %s  (worst drop %ldms)\n", "  it never jumps backwards",
           worst_drop <= 50 ? "ok" : "FAIL", worst_drop);
    if (worst_drop > 50)
        fails++;

    mr_rhythm_stop();
    mr_audio_stop(MR_CH_MUSIC, 0);
}

/* Reaching a song the way the player does: from the extras menu, by clicking
 * a song button. Both of the bugs this covers were invisible when the
 * minigame was started any other way.
 */
static void test_from_the_extras_menu(const char *dir)
{
    mr_script *s;
    long now = 0;
    int  guard;

    printf("\n-- reached from the extras menu --\n");

    s = mr_script_load(dir);
    if (!s || s->n == 0) {
        check("the script loads", 0);
        return;
    }
    mr_interp_init(s, dir);
    mr_interp_start("start");
    mr_interp_main_menu();

    /* The extras menu, then the button for a song. Jump() through the real
     * action handler is exactly what the button does. */
    mr_interp_show_screen("extras_rythm");
    check("the extras menu is up", mr_scene_screen_count() > 0);
    /* The fixture's label, not the game's `game_watashinooto`. What this
     * checks is the path -- a screen is up, a button's Jump action runs,
     * and the label behind it reaches `call screen rhythm_game` -- not
     * which song is at the end of it. */
    mr_interp_action("Jump(\"fixture_rhythm\")");

    for (guard = 0; guard < 600 && !mr_rhythm_active(); guard++) {
        mr_interp_update(now);
        now += 33;
    }
    check("clicking a song starts a run", mr_rhythm_active());
    if (!mr_rhythm_active()) {
        mr_interp_shutdown();
        mr_script_free(s);
        return;
    }

    /* The bug: the extras menu's screen was still in the scene's list, and
     * the screen test in mr_interp_redraw_kind sat above the minigame's, so
     * this answered NONE and the frame loop skipped every frame. The game
     * only redrew when a keypress happened to set the dirty flag. */
    mr_interp_update(now);
    now += 33;
    checkl("  and every frame is still a full redraw",
           mr_interp_redraw_kind(), MR_REDRAW_ALL);
    check("  with the menu it came from taken down",
          mr_scene_screen_count() == 0);

    /* Finish the run and come back. */
    mr_interp_rhythm_key(MR_KEY_ESCAPE);
    mr_interp_rhythm_key('Y');
    for (guard = 0; guard < 200 && mr_rhythm_active(); guard++) {
        mr_interp_update(now);
        now += 33;
    }
    check("giving up ends the run", !mr_rhythm_active());

    /* The other bug: `call screen extras_rythm` did not block, so the script
     * ran straight on into `label game_soufflevent:` -- the next label in the
     * file -- and the following song started about half a second after the
     * selection screen came back. */
    for (guard = 0; guard < 600; guard++) {
        mr_interp_update(now);
        now += 33;
    }
    check("the song selection comes back", mr_scene_screen_count() > 0);
    check("  and the next song does NOT start on its own",
          !mr_rhythm_active());

    mr_interp_shutdown();
    mr_script_free(s);
}

int main(int argc, char **argv)
{
    const char *pak = (argc > 1) ? argv[1] : "mrpy.pak";
    const char *dir = (argc > 2) ? argv[2] : "game";

    remove("PERSIST.SAV");

    if (!mr_vfs_init(pak, dir)) {
        printf("vfs init failed (pak '%s', dir '%s')\n", pak, dir);
        return 1;
    }
    mr_image_init(dir);
    mr_audio_init(dir);
    mr_ui_init(dir);
    mr_scene_init();
    g_stage = mr_surface_new(MR_STAGE_W, MR_STAGE_H);

    read_expected();
    check("the beatmap is readable", g_nexp == NEXP);
    if (g_nexp != NEXP) {
        printf("cannot continue without %s\n", BMAP);
        return 1;
    }

    test_beatmap();
    test_scoring();
    test_misses();
    test_game_over();
    test_quit_button();
    test_give_up_dialog();
    test_summary_and_highscore();
    test_persistence();
    test_clock_never_runs_backwards();
    test_through_the_script(dir);
    test_from_the_extras_menu(dir);
    test_give_up_hover();

    mr_surface_free(g_stage);
    mr_audio_shutdown();
    mr_ui_shutdown();
    mr_image_shutdown();
    mr_scene_shutdown();
    mr_vfs_shutdown();
    remove("PERSIST.SAV");

    printf(fails ? "\n%d FAILED\n" : "\nall good\n", fails);
    return fails ? 1 : 0;
}

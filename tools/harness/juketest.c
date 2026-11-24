/* The jukebox: screen music_room.
 *
 * Three things it needs that nothing else in the game does -- MusicRoom,
 * which the script builds in `init python` and stores in a screen variable;
 * AudioPositionValue, which the position bar reads; and `area`, which lays
 * the three viewports over one another -- so all three are driven here.
 *
 * The buttons are found in the real focus list and clicked through
 * mr_interp_action, the same path a mouse takes, and the assertions are on
 * what came back out: which file the mixer is on, and where the layout put
 * the bar.
 *
 * Needs the pak (for the audio) and the game directory.
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "mr.h"
#include "expr.h"
#include "interp.h"
#include "vfs.h"
#include "audio.h"
#include "script.h"
#include "image.h"
#include "scene.h"
#include "slrender.h"
#include "ui.h"

static mr_surface *g_st;
mr_surface *mr_stage(void) { return g_st; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); }
void mr_quit(void) {}
void mr_invalidate(void) {}

/* The layout dump goes through mr_log, so the log is captured and searched
 * rather than read by eye. */
#define LOGMAX 400
static char g_log[LOGMAX][160];
static int  g_nlog;

void mr_log(const char *fmt, ...)
{
    va_list ap;
    if (g_nlog >= LOGMAX)
        return;
    va_start(ap, fmt);
    vsprintf(g_log[g_nlog], fmt, ap);
    va_end(ap);
    g_nlog++;
}

/* The line of the layout dump for the first node of type `type` whose text
 * contains `want` (or any, if `want` is NULL). */
static const char *layout_line(const char *type, const char *want)
{
    int i;
    for (i = 0; i < g_nlog; i++) {
        const char *p = strstr(g_log[i], "layout: ");
        if (!p)
            continue;
        p += 8;
        while (*p == ' ')
            p++;
        if (strncmp(p, type, strlen(type)) || p[strlen(type)] != ' ')
            continue;
        if (want && !strstr(p, want))
            continue;
        return p;
    }
    return NULL;
}

/* The rectangle off such a line: "<type> [<style>] x,y wxh". */
static int layout_rect(const char *type, const char *want, int r[4])
{
    const char *p = layout_line(type, want);
    if (!p)
        return 0;
    p = strchr(p, ']');
    if (!p)
        return 0;
    return sscanf(p + 1, "%d,%d %dx%d", &r[0], &r[1], &r[2], &r[3]) == 4;
}

/* Is there a node of this type at exactly this size? The screen has three
 * viewports and two of them are told their size outright. */
static int layout_sized(const char *type, int w, int h, int r[4])
{
    int i;
    for (i = 0; i < g_nlog; i++) {
        const char *p = strstr(g_log[i], "layout: ");
        int got[4];

        if (!p)
            continue;
        p += 8;
        while (*p == ' ')
            p++;
        if (strncmp(p, type, strlen(type)) || p[strlen(type)] != ' ')
            continue;
        p = strchr(p, ']');
        if (!p || sscanf(p + 1, "%d,%d %dx%d",
                         &got[0], &got[1], &got[2], &got[3]) != 4)
            continue;
        if (got[2] == w && got[3] == h) {
            memcpy(r, got, sizeof(got));
            return 1;
        }
    }
    return 0;
}

/* How many pixels of exactly this colour are inside a rectangle. */
static int count_colour(const int r[4], unsigned long want)
{
    int x, y, n = 0;

    for (y = r[1]; y < r[1] + r[3] && y < MR_STAGE_H; y++) {
        for (x = r[0]; x < r[0] + r[2] && x < MR_STAGE_W; x++) {
            if (x < 0 || y < 0)
                continue;
            if ((g_st->px[y * g_st->pitch + x] & 0xFFFFFFUL) == want)
                n++;
        }
    }
    return n;
}

static void sink(const short *d, int n) { (void)d; (void)n; }
void (*hostshim_wave_sink)(const short *, int) = sink;

static long T;
static int  fails;

static void draw(void)
{
    mr_interp_update(T);
    T += 33;
    mr_sl_focus_reset();
    mr_interp_render(g_st, T);
    mr_ui_draw(g_st);
}

static void check(const char *label, int ok)
{
    printf("%-52s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

static int find_action(const char *want)
{
    int id;
    for (id = 0; id < 200; id++) {
        const char *a = mr_sl_focus_action(id);
        if (a && strstr(a, want))
            return id;
    }
    return -1;
}

static void click(int id)
{
    const char *a = mr_sl_focus_action(id);
    if (!a)
        return;
    mr_sl_focus_scope(id);
    mr_interp_action(a);
    mr_sl_focus_scope_end();
}

static const char *playing(void)
{
    static char buf[128];
    int loop;
    mr_audio_current(MR_CH_MUSIC, buf, sizeof(buf), &loop);
    return buf;
}

/* Poll the mixer until it has produced `ms` of audio. The host shim drains
 * every buffer as it is queued, so polling is what advances the stream. */
static void run_ms(long ms)
{
    long want = mr_audio_position_ms(MR_CH_MUSIC) + ms;
    long guard = 0;

    /* A poll fills every buffer the driver has finished with, so the stream
     * moves in jumps of up to the whole queue, over a second. Overshooting is fine;
     * the guard is for a channel that has stopped and will never advance.
     *
     * Scaled by `ms`, not a flat count: the last block here shallows the
     * queue to one buffer, where a poll is 93ms, and a flat forty polls
     * silently delivered 3.8 seconds of a requested 8 and the assertion
     * about where the bar had got to failed with the bar correct. */
    while (mr_audio_position_ms(MR_CH_MUSIC) < want && guard++ < 40 + ms / 20)
        mr_audio_poll();
}

static double bar_value(void)
{
    mr_value v;
    mr_expr_eval("AudioPositionValue()", &v);
    return v.type == MR_V_FLOAT ? v.f : (double)v.i;
}

/* The fixture's playlist, which is tools/harness/common/screens.rpy's
 * `build_music_room` -- seven generated tones always unlocked and two
 * behind persistent flags. They are named after their lengths, and the
 * lengths are what this test spends most of its time on.
 *
 * FIRST is the one this clicks and the one the bar is measured against,
 * so it is the longest: the windows below are seconds wide and a track
 * that loops under them sends the bar backwards. SECOND is both the main
 * menu's music (common/options.rpy) -- something for the bar to follow
 * before any button is clicked, and necessarily not FIRST, or the click
 * would change nothing -- and what Next() reaches from FIRST. LAST is
 * what Previous() wraps round to from the first, which is the seventh
 * entry and not the ninth, because the other two are locked.
 */
static const char *FIRST  = "audio/fix-cbr-44-stereo.mp3";
static const char *SECOND = "audio/fix-vbr-48-stereo.mp3";
static const char *LAST   = "audio/fix-vbr-48-mono.mp3";
static const char *LOCKED = "audio/fix-cbr-48-mono.mp3";

/* FIRST's length, read from the fixture's duration table rather than
 * written here. That table is ffprobe's measurement of the encoded file
 * (tools/mkfixaudio.py), which is the point: dividing the position by the
 * engine's own idea of the duration is self-consistent whatever it says,
 * and passed happily while the bar read 1.39. */
static long g_first_ms;

static long measured_ms(const char *track)
{
    const char *base = strrchr(track, '/');
    char buf[4096];
    long n;
    char *line;
    mr_file *f = mr_vfs_open("audio/durations.lst");

    if (!f)
        return 0;
    n = mr_vfs_read(f, buf, (long)sizeof(buf) - 1);
    mr_vfs_close(f);
    if (n <= 0)
        return 0;
    buf[n] = '\0';

    base = base ? base + 1 : track;
    for (line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        char *tab = strchr(line, '\t');
        if (line[0] == '#' || !tab)
            continue;
        *tab = '\0';
        if (!strcmp(line, base))
            return atol(tab + 1);
    }
    return 0;
}

int main(int argc, char **argv)
{
    mr_script *s;
    int id;

    setbuf(stdout, NULL);
    if (argc < 3) {
        fprintf(stderr, "usage: juketest <pak> <game-dir>\n");
        return 2;
    }

    /* A PERSIST.SAV left in the working directory by another harness would
     * unlock the tracks this expects to be locked. */
    remove("PERSIST.SAV");

    mr_vfs_init(argv[1], argv[2]);
    mr_image_init(argv[2]);
    mr_scene_init();
    if (!mr_audio_init(argv[2])) {
        printf("no audio device\n");
        return 1;
    }
    s = mr_script_load(argv[2]);
    mr_interp_init(s, argv[2]);
    mr_ui_init(argv[2]);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);

    g_first_ms = measured_ms(FIRST);
    if (g_first_ms <= 0) {
        printf("no audio/durations.lst in the pak -- rebuild it with "
               "tools/harness/mkfixpak.sh\n");
        return 1;
    }

    mr_interp_start("start");
    mr_interp_main_menu();

    /* Nothing unlocked, so the room is built without its last two tracks
     * -- which is what the locked-track check below rests on. The room is
     * built when the screen is shown, so this comes first. Cleared rather
     * than assumed: `label start` above sets unlock_a, which is
     * unlocktest's subject and would quietly give this one nine tracks. */
    mr_expr_exec("persistent.unlock_a = False");
    mr_expr_exec("persistent.unlock_b = False");
    mr_interp_show_screen("music_room");

    mr_sl_dump_next();
    draw();

    /* --- the room itself --- */
    {
        mr_value v;
        mr_expr_eval("build_music_room()", &v);
        check("build_music_room() returns a music room",
              v.type == MR_V_STR && v.s[0] == '\001');
    }

    /* --- layout: `area` puts the three viewports where the script says --- */
    {
        int r[4];

        /* `area (0, 42, 921, 478)` and `area (0, 530, 901, 30)`, both
         * relative to the content frame, which starts at 360,130. */
        check("the track list is the size its area asks for",
              layout_sized("viewport", 921, 478, r));
        check("  and where its area puts it", r[1] == 130 + 42);

        check("the bar's viewport is the size its area asks for",
              layout_sized("viewport", 901, 30, r));
        check("  and where its area puts it", r[1] == 130 + 530);

        if (layout_rect("bar", NULL, r)) {
            printf("   (the position bar: %d,%d %dx%d)\n",
                   r[0], r[1], r[2], r[3]);
            check("the position bar is as wide as its viewport",
                  r[2] >= 880 && r[2] <= 901);
            check("  and as short", r[3] <= 30);
            check("  and near the bottom of the content frame", r[1] > 500);
        } else {
            check("the position bar was laid out", 0);
        }

        check("Previous and Next are above the list",
              layout_rect("textbutton", "cédent", r) && r[1] < 200);
    }

    /* The bar tracks the music channel whatever started it, so it moves for
     * the main menu's own track before any button here is touched. */
    {
        double a, b;
        a = bar_value();
        run_ms(3000);
        b = bar_value();
        printf("   (menu music: %s, bar %.4f then %.4f)\n", playing(), a, b);
        check("the bar follows the menu's music too", b > a && b > 0.0);
    }

    /* --- clicking a track --- */
    id = find_action("fix-cbr-44-stereo");
    check("found a track button", id >= 0);
    if (id < 0)
        return 1;

    click(id);
    /* The room fades out whatever the menu was playing before its track
     * starts -- fadeout=1.0 -- so this is not immediate. */
    run_ms(1500);
    printf("   (playing: %s)\n", playing());
    check("clicking a track plays it", !strcmp(playing(), FIRST));

    /* --- the playing track is drawn selected ---
     *
     * MusicRoom's Play reports itself selected while its file is the one
     * playing, and gui.button_text_selected_color is #ffffff against an idle
     * #888888. So the playing track's button must have white pixels in it and
     * the others must not. The rectangles come from a fresh layout dump. */
    {
        int r[4];

        g_nlog = 0;
        mr_sl_dump_next();
        draw();

        /* By the button's label, which is the track's length written out:
         * FIRST is "Twenty seconds", and "One second" is a track that is
         * not playing. */
        if (layout_rect("textbutton", "Twenty seconds", r))
            printf("   (playing track's button: %d,%d %dx%d)\n",
                   r[0], r[1], r[2], r[3]);
        check("the playing track is drawn in the selected colour",
              layout_rect("textbutton", "Twenty seconds", r) &&
              count_colour(r, 0xFFFFFF) > 20);
        check("  and another track is not",
              layout_rect("textbutton", "One second", r) &&
              count_colour(r, 0xFFFFFF) == 0);
        check("  but that one is drawn at all",
              layout_rect("textbutton", "One second", r) &&
              count_colour(r, 0x888888) > 20);
    }

    /* --- the position bar --- */
    {
        double a, b;

        a = bar_value();
        run_ms(4000);
        b = bar_value();
        printf("   (bar: %.4f then %.4f)\n", a, b);
        check("the bar has a position at all", b > 0.0);
        check("and it advances", b > a);
        check("without running off the end", b < 1.0);
    }

    /* --- Next and Previous walk the playlist --- */
    id = find_action("Next");
    check("found the Next button", id >= 0);
    if (id >= 0) {
        click(id);
        run_ms(1500);        /* it fades the old track out first */
        printf("   (after Next: %s)\n", playing());
        check("Next advances to the following track",
              !strcmp(playing(), SECOND));
    }

    id = find_action("Previous");
    check("found the Previous button", id >= 0);
    if (id >= 0) {
        click(id);
        run_ms(1500);
        printf("   (after Previous: %s)\n", playing());
        check("Previous goes back", !strcmp(playing(), FIRST));
    }

    /* Previous from the first track wraps to the last, as Ren'Py's modulo
     * does -- and "the last" is the seventh entry, not the ninth, because
     * the other two are locked. */
    {
        int first = find_action("fix-cbr-44-stereo");
        if (first >= 0) {
            click(first);
            run_ms(1500);
            check("the first track plays", !strcmp(playing(), FIRST));
            id = find_action("Previous");
            click(id);
            run_ms(1500);
            printf("   (Previous from the first: %s)\n", playing());
            check("Previous from the first wraps to the last",
                  !strcmp(playing(), LAST));
        }
    }

    /* --- a locked track is not in the playlist --- */
    {
        int seen = 0, i;

        /* LOCKED is only added when persistent.unlock_a is set, so with it
         * unset the room should never land on it. */
        for (i = 0; i < 12; i++) {
            click(find_action("Next"));
            run_ms(1200);
            if (!strcmp(playing(), LOCKED))
                seen = 1;
        }
        check("a locked track never plays", !seen);
    }

    /* --- the bar asks for a frame only when its picture would change ---
     *
     * The frame loop skips rendering when nothing has moved, which is why the
     * bar used to sit still until the mouse moved. It now reports itself as
     * needing a redraw exactly when one more of its pixels would be filled:
     * a drawn frame must leave nothing pending, and enough audio must make it
     * pending again. */
    {
        long dur, px_ms;

        draw();
        check("a drawn frame leaves nothing to redraw",
              !mr_interp_needs_redraw());

        dur = mr_audio_duration_ms(MR_CH_MUSIC);
        px_ms = dur / 901;                  /* the bar is 901px wide */
        printf("   (one bar pixel is %ldms of a %lds track)\n",
               px_ms, dur / 1000);

        run_ms(px_ms + 200);
        check("enough audio to move a pixel asks for one",
              mr_interp_needs_redraw());

        draw();
        check("and drawing it settles again", !mr_interp_needs_redraw());
    }

    /* --- the bar's reading is the right size, not merely rising ---
     *
     * Everything above only asked whether the bar moved forward, and that is
     * true of any wrong duration whatsoever: a jukebox bar that filled in
     * fifteen seconds of a 209-second track passed every check here, as did
     * one that never moved at all because its duration had come out negative.
     * So: play a known track, let a known amount of audio through, and
     * require the fraction to be the quotient of the two.
     *
     * The queue is shallowed to one buffer first. A poll advances the
     * stream by as much as the whole queue, and against a 20-second
     * fixture track that overshoot is a tenth of the bar -- a tolerance
     * wide enough to admit a duration that is half right. With one buffer
     * the overshoot is 93ms, so the tolerance can stay near the half a
     * percent this check was written with. Nothing after this block
     * depends on the depth.
     */
    {
        double got, want, slack;
        long   dur, pos, played;

        mr_audio_set_queue_ms(1);
        mr_audio_stop(MR_CH_MUSIC, 0);
        mr_audio_poll();
        mr_audio_play(MR_CH_MUSIC, FIRST, 1, 0, 1.0f);
        mr_audio_poll();

        dur = mr_audio_duration_ms(MR_CH_MUSIC);
        printf("   (the engine says %ldms; ffprobe said %ldms)\n",
               dur, g_first_ms);
        check("the track has a plausible length",
              dur > g_first_ms - 500 && dur < g_first_ms + 500);

        pos = mr_audio_position_ms(MR_CH_MUSIC);
        run_ms(8000);
        played = mr_audio_position_ms(MR_CH_MUSIC) - pos;

        /* Against g_first_ms, not against `dur`: dividing by the engine's
         * own duration is self-consistent whatever it says, and passed
         * happily while the bar read 1.39. g_first_ms is ffprobe's answer
         * for this file, recorded when the fixture was generated. */
        got   = bar_value();
        want  = (double)(pos + played) / (double)g_first_ms;
        slack = 0.005 + 200.0 / (double)g_first_ms;

        printf("   (%ldms of a %ldms track: bar %.4f, expected %.4f)\n",
               pos + played, dur, got, want);
        check("the bar reads position over duration",
              got > want - slack && got < want + slack);

        /* The same stated as the thing the player sees, independent of the
         * arithmetic above: eight seconds of a twenty-second track is
         * somewhere in the middle and nowhere near either end. */
        check("eight seconds into a twenty-second track the bar is mid-way",
              got > 0.25 && got < 0.60);
    }

    /* --- the position bar reads zero with nothing playing --- */
    mr_audio_stop(MR_CH_MUSIC, 0);
    check("no track, no position", bar_value() == 0.0);

    draw();
    mr_audio_poll();
    mr_audio_poll();
    check("and an idle jukebox asks for no frames at all",
          !mr_interp_needs_redraw());

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    mr_audio_shutdown();
    mr_vfs_shutdown();
    return fails ? 1 : 0;
}

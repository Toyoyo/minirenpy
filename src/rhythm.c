/* rhythm.c - the rhythm minigame. See rhythm.h.
 *
 * Laid out for the 1280x720 stage, with every constant taken from
 * game/00-renpythm/rhythm_game_displayable.rpy so the two builds place things
 * identically: tracks 360px apart starting at x=100, a 28px hit-zone bar with
 * its top at y=682, and 108x108 note art that grows to 135 inside the hit
 * window.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mr.h"
#include "keys.h"      /* MR_KEY_*, which this file used to spell VK_* */
#include "rhythm.h"
#include "audio.h"
#include "image.h"
#include "expr.h"
#include "ui.h"
#include "tl.h"
#include "slrender.h"
#include "vfs.h"
#include "script.h"
#include "config.h"

/* --- layout (all from the .rpy) ----------------------------------------- */

#define RG_TRACKS        4
#define RG_X_OFFSET      100
#define RG_TRACK_WIDTH   4
#define RG_SPACING       ((MR_STAGE_W - RG_X_OFFSET * 2) / (RG_TRACKS - 1))
#define RG_BAR_H         28
#define RG_TRACK_H       (MR_STAGE_H - RG_BAR_H - 10)
#define RG_BAR_MID_Y     (RG_TRACK_H + RG_BAR_H / 2)
#define RG_NOTE_W        108
#define RG_NOTE_TRAVEL   (RG_BAR_MID_Y + RG_NOTE_W / 2)
#define RG_BAR_X         (RG_X_OFFSET - RG_TRACK_WIDTH / 2)
#define RG_BAR_W         ((MR_STAGE_W - RG_X_OFFSET * 2) + RG_TRACK_WIDTH)

/* zoom_scale 1.25 on a 108px note */
#define RG_NOTE_BIG_W    (RG_NOTE_W * 5 / 4)

/* --- timing (milliseconds; the .rpy speaks in seconds) ------------------- */

#define RG_COUNTDOWN_MS    3000
#define RG_NOTE_OFFSET_MS  2000    /* RHYTHM_NOTE_OFFSET, and the lead-in */
#define RG_HIT_MS          300     /* hit_threshold */
#define RG_PERFECT_MS      200     /* RHYTHM_PERFECT_WINDOW */
#define RG_JUDGE_MS        400     /* judgement_duration */
#define RG_JUDGE_FADE_MS   100     /* judgement_fade */
#define RG_END_GRACE_MS    800     /* before the summary accepts a dismiss */

#define RG_MAX_HEALTH      100     /* RHYTHM_MAX_HEALTH */
#define RG_MISS_DAMAGE     5       /* RHYTHM_MISS_DAMAGE */

/* points = 10 + combo, x1.5 for a PERFECT. Kept as a *3/2 so there is no
 * float in the scoring path and the two builds agree exactly. */
#define RG_PERFECT_NUM     3
#define RG_PERFECT_DEN     2

/* --- colours ------------------------------------------------------------- */

#define RG_C_BAR       MR_RGB(0x4d, 0xc8, 0xff)
#define RG_C_BAR_FILL  MR_RGB(0x2a, 0x6e, 0x99)
#define RG_C_WHITE     MR_RGB(0xff, 0xff, 0xff)
#define RG_C_BLACK     MR_RGB(0, 0, 0)
#define RG_C_GREEN     MR_RGB(0x43, 0xd3, 0x5a)
#define RG_C_AMBER     MR_RGB(0xff, 0xc8, 0x82)
#define RG_C_RED       MR_RGB(0xff, 0x80, 0x80)
#define RG_C_CYAN      MR_RGB(0x4d, 0xc8, 0xff)

/* The give-up dialog's geometry, computed once and used by both the drawing
 * and the hit test. Declared here because the click handler comes first in
 * the file; see draw_pause for the layout itself. */
typedef struct {
    int x, y, w, h;             /* the frame */
    int prompt_y;
    int yes_x, no_x, btn_y, btn_w, btn_h;
} rg_dialog;

static void dialog_layout(rg_dialog *d);

/* Likewise: the quit button's rectangle is wanted by the hover and click
 * handlers, which come before the drawing it belongs with. */
static int quit_rect(int *x, int *y, int *w, int *h);

/* Which of the give-up dialog's two buttons the pointer is over: 0 for Oui,
 * 1 for Non, -1 for neither. The dialog is drawn natively rather than as a
 * screen, so it has no focus entry and the screen layer's hover cannot reach
 * it -- this is the whole of its hover state. Without it the buttons never
 * lit, where the `confirm` screen they imitate does. */
static int g_dlg_hover = -1;

/* The quit button's own hover flag, for the same reason: it is drawn over a
 * displayable the screen system does not own, so it has no focus entry and
 * the screen layer's hover cannot reach it. */
static int g_quit_hover;

/* --- notes --------------------------------------------------------------- */

/* The longest beatmap here is 1076 onsets and the smallest stride the screen
 * is ever called with is 2, but a stride of 1 has to fit too. */
#define RG_MAX_NOTES 2048

/* Reserved image names, so a run cannot collide with anything the script
 * defined. \001 cannot appear in a name the parser produces. */
#define RG_IMG_BG   "\001rgbg"

/* The give-up button in the top-right corner. The .rpy calls it a quit button
 * "for touch screens which have no Escape key"; here it is simply the mouse
 * way in, since the dialog it opens can otherwise only be reached from the
 * keyboard. Two states, as an imagebutton has. */
#define RG_IMG_QUIT_IDLE  "\001rgqi"
#define RG_IMG_QUIT_HOVER "\001rgqh"
#define RG_QUIT_IDLE_FILE  "00-renpythm/images/quit_idle.png"
#define RG_QUIT_HOVER_FILE "00-renpythm/images/quit_hover.png"

/* `align (1.0, 0.0) xoffset -10 yoffset 10` from the .rpy: hard against the
 * top-right corner, then nudged in and down by ten. */
#define RG_QUIT_MARGIN 10

static const char *const RG_NOTE_FILES[RG_TRACKS] = {
    "00-renpythm/images/note_left.png",
    "00-renpythm/images/note_up.png",
    "00-renpythm/images/note_down.png",
    "00-renpythm/images/note_right.png"
};
static const char *const RG_NOTE_NAMES[RG_TRACKS] = {
    "\001rgn0", "\001rgn1", "\001rgn2", "\001rgn3"
};

typedef enum {
    RG_OFF = 0,
    RG_COUNTDOWN,
    RG_PLAYING,
    RG_ENDED,          /* song over: the summary is up */
    RG_DONE            /* dismissed; the caller should collect the result */
} rg_phase;

typedef enum {
    RG_J_NONE = 0,
    RG_J_PERFECT,
    RG_J_GREAT
} rg_judge;

static struct {
    rg_phase phase;

    long  onset_ms[RG_MAX_NOTES];
    unsigned char track[RG_MAX_NOTES];
    signed char   status[RG_MAX_NOTES];   /* 0 pending, 1 hit, -1 missed */
    int   n_notes;
    int   first_live;      /* notes below this are all settled */

    long  duration_ms;     /* of the track, once it is playing */
    long  last_onset_ms;

    /* clocks, all in the frame loop's milliseconds */
    long  t0;
    int   clock_set;       /* t0 is valid -- it may legitimately be 0 */
    long  music_at;        /* when the track is due to start */
    int   music_started;
    long  anchor;          /* now - song_ms; see song_ms() */
    long  now;

    /* pausing (the give-up dialog) */
    int   paused;
    long  pause_at;
    long  paused_total;
    float pre_pause_volume;   /* the music slider's value, to be put back */

    /* scoring */
    long  score;
    long  hits;
    long  combo;
    int   full_combo;
    long  n_perfect;
    long  n_great;
    int   health;
    int   game_over;
    int   new_highscore;
    int   is_full_combo;

    rg_judge judge;
    long     judge_at;

    long  ended_at;        /* song time the run stopped, so the bar freezes */
    int   have_ended_at;
    long  end_wall;        /* wall clock the summary appeared */

    int   hit_channel;     /* round-robin over the four */
    char  hit_sound[RG_TRACKS][160];
    int   have_hit_sounds;

    char  audio[160];
    char  beatmap[160];
    int   have_bg;

    /* The quit button's size, measured once at start. Kept here rather than
     * asked of mr_image_get where it is needed: the hover handler runs on
     * every mouse move, and a cache lookup -- which may evict, and on a miss
     * decodes a PNG -- is not something to do per WM_MOUSEMOVE on a Pentium
     * III. 0 means the images were missing and there is no button. */
    int   quit_w, quit_h;
} g;

/* --- persistent high scores ---------------------------------------------
 *
 * Ren'Py keeps `persistent.rhythm_game_highscores`, a dict keyed by the
 * beatmap's bare file name. There are no dicts here, but the variable store
 * holds dotted names whole and persist.c writes out everything under
 * `persistent.`, so one variable per song is the same thing by another
 * spelling -- and the high-score screen reaches it through
 * rhythm_game_get_highscore() either way, which is the part that has to
 * match.
 *
 * The `.beatmap.txt` suffix is dropped from the key. Names are capped at 64
 * bytes in the persistent file and
 * `persistent.rhythm_game_highscores.resilience-mujou_no_bi.beatmap.txt`
 * is 68; without the suffix the longest in this game is 57.
 */
#define RG_SCORE_PREFIX "persistent.rhythm_game_highscores."

static void score_key(const char *beatmap_path, char *out, int outsz)
{
    const char *base = beatmap_path;
    const char *p;
    int n;

    for (p = beatmap_path; *p; p++)
        if (*p == '/' || *p == '\\')
            base = p + 1;

    n = (int)strlen(base);
    if (n > 12 && !strcmp(base + n - 12, ".beatmap.txt"))
        n -= 12;

    sprintf(out, "%s", RG_SCORE_PREFIX);
    {
        int pre = (int)strlen(out);
        if (n > outsz - pre - 1)
            n = outsz - pre - 1;
        memcpy(out + pre, base, (size_t)n);
        out[pre + n] = '\0';
    }
}

long mr_rhythm_highscore_get(const char *beatmap_path)
{
    char key[160];
    score_key(beatmap_path, key, sizeof(key));
    return mr_var_get_int(key, 0);
}

int mr_rhythm_highscore_set(const char *beatmap_path, long score)
{
    char key[160];
    score_key(beatmap_path, key, sizeof(key));
    if (score <= mr_var_get_int(key, 0))
        return 0;
    mr_var_set_int(key, score);
    return 1;
}

/* --- beatmap ------------------------------------------------------------- */

/* "seconds" or "seconds<TAB>lane". Every beatmap this game ships carries the
 * lane, because generate_beatmap-pentatonique.py picks it from the music --
 * the note you hear on that lane is the note that is played. The older format
 * without one is still accepted, and then the lanes are dealt round-robin
 * from a hash of the file name, so a given song always looks the same.
 *
 * Lanes are only believed when *every* onset has one, matching
 * read_beatmap_file(): a half-annotated file is treated as unannotated. */
static int load_beatmap(const char *path, int stride)
{
    char *text;
    long  size;
    char *p;
    int   kept = 0;
    int   with_lane = 0, total = 0;
    unsigned int seed;

    text = (char *)mr_vfs_read_all(path, &size);
    if (!text) {
        mr_log("rhythm: cannot read beatmap %s", path);
        return 0;
    }

    p = text;
    while (p < text + size && kept < RG_MAX_NOTES) {
        char *line = p;
        char *end;
        double t;
        int lane = -1;

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

        t = atof(line);
        {
            char *tab = line;
            while (*tab && *tab != ' ' && *tab != '\t')
                tab++;
            while (*tab == ' ' || *tab == '\t')
                tab++;
            if (*tab) {
                lane = atoi(tab);
                with_lane++;
            }
        }
        total++;

        /* The stride is applied over the onsets in file order, as
         * `onset_times[::beatmap_stride]` does. */
        if ((total - 1) % stride)
            continue;

        g.onset_ms[kept] = (long)(t * 1000.0 + 0.5);
        g.track[kept] = (unsigned char)(lane >= 0 ? (lane % RG_TRACKS) : 0);
        g.status[kept] = 0;
        kept++;
    }

    free(text);

    if (!kept) {
        mr_log("rhythm: beatmap %s has no onsets", path);
        return 0;
    }

    g.n_notes = kept;
    g.last_onset_ms = g.onset_ms[kept - 1];

    /* Not every onset carried a lane: deal them instead. This is the djb2 +
     * LCG pair RhythmGameRandom reproduces, so the pattern matches the other
     * two builds for a beatmap in the old format. */
    if (with_lane != total) {
        const char *base = path;
        const char *q;
        int i;

        for (q = path; *q; q++)
            if (*q == '/' || *q == '\\')
                base = q + 1;

        seed = 5381;
        for (q = base; *q; q++)
            seed = seed * 33u + (unsigned char)*q;

        for (i = 0; i < kept; i++) {
            seed = seed * 1103515245u + 12345u;
            g.track[i] = (unsigned char)((seed >> 16) % RG_TRACKS);
        }
        mr_log("rhythm: %s has no lanes, dealing them from the name", path);
    }

    return 1;
}

/* --- clock --------------------------------------------------------------- */

/* Where the run is in the song, in milliseconds. Negative during the lead-in:
 * a note at onset 0 appears at the top of the screen at -RG_NOTE_OFFSET_MS
 * and reaches the bar exactly as the track starts.
 *
 * Once the music is audible this follows the sound card rather than the frame
 * loop, but smoothly: the card is asked where it is, and the wall-clock
 * anchor is nudged toward its answer instead of being replaced by it. Taken
 * raw, the position is quantised to whole buffers on some drivers (and to
 * whole buffers in the host shim), which would make the notes stutter; taken
 * as pure wall clock, the notes would slowly drift away from the music. */
static long song_ms(void)
{
    long now = g.now - g.paused_total;

    if (g.music_started) {
        long pos = mr_audio_output_position_ms(MR_CH_MUSIC);
        if (pos > 0) {
            long diff = (now - pos) - g.anchor;

            /* Only ever eased, never replaced -- the anchor already holds the
             * schedule the notes have been falling to, and adopting the
             * card's reading outright is what used to make them jump. A gap
             * this big means something genuinely stalled, and a jump is then
             * better than staying wrong. */
            if (diff > 400 || diff < -400)
                g.anchor += diff;
            else
                g.anchor += diff / 8;
        }
    }

    return now - g.anchor;
}

/* --- lifecycle ----------------------------------------------------------- */

static void pick_hit_sounds(const char *beatmap_path)
{
    int i;
    int n = (int)strlen(beatmap_path);
    char base[160];

    g.have_hit_sounds = 0;

    /* "foo.beatmap.txt" -> "foo.hit0.wav" .. "foo.hit3.wav", which is what
     * the generator writes beside the beatmap. All four or none, as
     * rhythm_game_song_sounds() requires. */
    if (n <= 12 || strcmp(beatmap_path + n - 12, ".beatmap.txt"))
        return;
    if (n - 12 >= (int)sizeof(base))
        return;
    memcpy(base, beatmap_path, (size_t)(n - 12));
    base[n - 12] = '\0';

    for (i = 0; i < RG_TRACKS; i++) {
        sprintf(g.hit_sound[i], "%s.hit%d.wav", base, i);
        if (!mr_vfs_exists(g.hit_sound[i])) {
            mr_log("rhythm: %s missing, playing no hit sounds",
                   g.hit_sound[i]);
            return;
        }
    }
    g.have_hit_sounds = 1;
}

int mr_rhythm_start(const char *audio, const char *beatmap,
                    const char *background, int stride)
{
    int i;

    memset(&g, 0, sizeof(g));

    if (stride < 1)
        stride = 2;

    if (!load_beatmap(beatmap, stride))
        return 0;

    strncpy(g.audio, audio ? audio : "", sizeof(g.audio) - 1);
    strncpy(g.beatmap, beatmap ? beatmap : "", sizeof(g.beatmap) - 1);

    for (i = 0; i < RG_TRACKS; i++)
        mr_image_define(RG_NOTE_NAMES[i], RG_NOTE_FILES[i], 0);

    mr_image_define(RG_IMG_QUIT_IDLE, RG_QUIT_IDLE_FILE, 0);
    mr_image_define(RG_IMG_QUIT_HOVER, RG_QUIT_HOVER_FILE, 0);
    {
        mr_surface *q = mr_image_get(RG_IMG_QUIT_IDLE);
        g.quit_w = q ? q->w : 0;
        g.quit_h = q ? q->h : 0;
        if (!q)
            mr_log("rhythm: %s missing, no quit button", RG_QUIT_IDLE_FILE);
    }

    if (background && *background) {
        mr_image_define(RG_IMG_BG, background, 0);
        g.have_bg = 1;
    }

    pick_hit_sounds(beatmap);

    g_quit_hover = 0;         /* a run must not start with it already lit */
    g.health = RG_MAX_HEALTH;
    g.full_combo = 1;
    g.phase = RG_COUNTDOWN;
    g.first_live = 0;

    /* Shorten the mixer's queue for the duration of the run. The queue is the
     * latency of a hit sound -- it cannot be heard until everything already
     * handed to the driver has played -- and at the default depth that is the
     * whole runway, several times this screen's own 300ms hit window.
     *
     * Safe here and nowhere else: this screen draws a background, a few notes
     * and some text, nothing like the 1280x720 transitions the deep queue
     * exists for. The queue drains to the new depth over the lead-in, so
     * there is no gap at the start. mr_rhythm_stop puts it back.
     *
     * The ceiling is the runway itself, asked for rather than written down:
     * it was 743 here, which quietly became half the queue when NBUF went
     * from 16 to 24. */
    mr_audio_set_queue_ms(mr_config_int("rhythm_latency_ms", 140, 0,
                                        mr_audio_max_queue_ms()));

    mr_log("rhythm: %s, %d notes (stride %d), hit sounds %s",
           beatmap, g.n_notes, stride, g.have_hit_sounds ? "yes" : "no");
    return 1;
}

int mr_rhythm_active(void)
{
    return g.phase != RG_OFF;
}

int mr_rhythm_finished(void)
{
    return g.phase == RG_DONE;
}

void mr_rhythm_result(long *hits, long *notes, long *score)
{
    /* A game over reports zeroes, so the script can tell a lost run from a
     * finished one; giving up keeps whatever was achieved. */
    if (hits)  *hits  = g.game_over ? 0 : g.hits;
    if (notes) *notes = g.n_notes;
    if (score) *score = g.game_over ? 0 : g.score;
}

void mr_rhythm_stats(long *combo, int *health, long *perfect, long *great,
                     int *over)
{
    if (combo)   *combo   = g.combo;
    if (health)  *health  = g.health;
    if (perfect) *perfect = g.n_perfect;
    if (great)   *great   = g.n_great;
    if (over)    *over    = g.game_over;
}

long mr_rhythm_song_ms(void)
{
    /* A pure read: song_ms() eases the anchor as a side effect, so a caller
     * that only wants to watch the clock must not go through it. */
    return g.now - g.paused_total - g.anchor;
}

int mr_rhythm_ended(void)
{
    return g.phase == RG_ENDED || g.phase == RG_DONE;
}

int mr_rhythm_paused(void)
{
    return g.paused;
}

/* The give-up dialog holds the run. There is no pause in this mixer, so the
 * music is ducked to silence rather than stopped -- stopping it would lose
 * the position and the run could not be resumed.
 *
 * What comes back afterwards is the slider's own value, not 1.0. Putting 1.0
 * back threw away the player's music setting, and leaving the run while the
 * dialog was up left the channel silent for good: the next track played at
 * volume zero, in the minigame and in the story alike. */
static void pause_run(void)
{
    g.paused = 1;
    g.pause_at = g.now;
    g.pre_pause_volume = mr_audio_get_volume(MR_CH_MUSIC);
    mr_audio_set_volume(MR_CH_MUSIC, 0.0f);
}

/* Undo the ducking, however the pause is ending. */
static void unduck(void)
{
    if (!g.paused)
        return;
    g.paused = 0;
    mr_audio_set_volume(MR_CH_MUSIC, g.pre_pause_volume);
}

static void resume_run(void)
{
    if (!g.paused)
        return;
    g.paused_total += g.now - g.pause_at;
    unduck();
}

void mr_rhythm_stop(void)
{
    if (g.phase == RG_OFF)
        return;
    unduck();
    mr_audio_set_queue_ms(0);          /* back to the deep default */
    mr_audio_stop(MR_CH_MUSIC, 0);
    {
        int i;
        for (i = 0; i < MR_CH_HIT_COUNT; i++)
            mr_audio_stop((mr_channel)(MR_CH_HIT0 + i), 0);
    }
    g.phase = RG_OFF;
}

/* The song reached its end without the health running out: this is the only
 * path that records a high score, matching the .rpy, where game_over() and
 * quit_game() both set has_ended themselves and skip the block that does. */
static void finish_song(void)
{
    g.phase = RG_ENDED;
    g.end_wall = g.now;
    g.new_highscore = mr_rhythm_highscore_set(g.beatmap, g.score);
    g.is_full_combo = g.full_combo && g.hits > 0;
}

static void game_over(void)
{
    g.game_over = 1;
    g.phase = RG_ENDED;
    g.end_wall = g.now;
    mr_audio_stop(MR_CH_MUSIC, 500);
}

/* --- scoring ------------------------------------------------------------- */

static void play_hit_sound(int track)
{
    mr_channel ch;

    if (!g.have_hit_sounds)
        return;
    ch = (mr_channel)(MR_CH_HIT0 + g.hit_channel);
    g.hit_channel = (g.hit_channel + 1) % MR_CH_HIT_COUNT;
    mr_audio_play(ch, g.hit_sound[track], 0, 0, 1.0f);
}

/* Validate the first pending note of the lane inside the hit window. Returns
 * 1 if a note was hit. */
static int try_hit(int track, long t)
{
    int i;

    for (i = g.first_live; i < g.n_notes; i++) {
        long delta;

        if (g.onset_ms[i] - t > RG_NOTE_OFFSET_MS)
            break;                       /* not on screen yet */
        if (g.status[i] != 0 || g.track[i] != track)
            continue;

        delta = g.onset_ms[i] - t;
        if (delta < 0)
            delta = -delta;
        if (delta > RG_HIT_MS)
            continue;

        g.status[i] = 1;
        g.hits++;
        {
            long points = 10 + g.combo;
            if (delta <= RG_PERFECT_MS) {
                points = (points * RG_PERFECT_NUM + RG_PERFECT_DEN / 2)
                         / RG_PERFECT_DEN;
                g.n_perfect++;
                g.judge = RG_J_PERFECT;
            } else {
                g.n_great++;
                g.judge = RG_J_GREAT;
            }
            g.score += points;
        }
        g.combo++;
        g.judge_at = t;
        play_hit_sound(track);
        return 1;
    }
    return 0;
}

/* A note that went past the window without being hit breaks the combo and
 * costs health. */
static void detect_misses(long t)
{
    int i;
    int missed = 0;

    for (i = g.first_live; i < g.n_notes; i++) {
        if (g.onset_ms[i] + RG_HIT_MS >= t)
            break;
        if (g.status[i] == 0) {
            g.status[i] = -1;
            g.combo = 0;
            g.full_combo = 0;
            g.health -= RG_MISS_DAMAGE;
            if (g.health < 0)
                g.health = 0;
            missed = 1;
        }
        /* Everything up to here is settled, so no later pass has to look at
         * it again. Without this the miss scan is O(notes) every frame on a
         * thousand-note beatmap. */
        if (i == g.first_live)
            g.first_live++;
    }

    if (missed && g.health <= 0 && !g.game_over)
        game_over();
}

/* --- update -------------------------------------------------------------- */

void mr_rhythm_update(long now_ms)
{
    long t;

    if (g.phase == RG_OFF || g.phase == RG_DONE)
        return;

    /* Anchored on the first update, not on whether t0 is still zero: the
     * frame loop's clock starts at zero too, and testing the value re-anchored
     * the run on every frame until it happened to be non-zero. */
    if (!g.clock_set) {
        g.clock_set = 1;
        g.t0 = now_ms;
        g.music_at = now_ms + RG_COUNTDOWN_MS + RG_NOTE_OFFSET_MS;
        /* Song time zero is when the track becomes audible, which is what the
         * schedule above names. The anchor starts there and is only ever
         * nudged from it once the card can be asked. */
        g.anchor = g.music_at;
    }
    g.now = now_ms;

    if (g.paused)
        return;

    t = song_ms();

    if (g.phase == RG_COUNTDOWN) {
        /* The countdown is over once the notes start falling, one note
         * lifetime before the music. */
        if (t > -RG_NOTE_OFFSET_MS)
            g.phase = RG_PLAYING;
    }

    if (g.phase == RG_PLAYING || g.phase == RG_COUNTDOWN) {
        /* Issued early by the mixer's latency, so the first sample is *heard*
         * at music_at rather than merely handed over then. Handing it over at
         * music_at instead left the notes running on the wall clock for the
         * up-to-a-runway the card took to reach it, and then falling back by
         * that much the moment its position could be read: once per song,
         * a few seconds in, the notes jumped back up the screen. */
        long lead = mr_audio_latency_ms();

        if (!g.music_started &&
            (g.now - g.paused_total) >= g.music_at - lead) {
            mr_audio_play(MR_CH_MUSIC, g.audio, 0, 0, 1.0f);
            g.music_started = 1;
            g.duration_ms = mr_audio_duration_ms(MR_CH_MUSIC);
        }
    }

    if (g.phase != RG_PLAYING)
        return;

    detect_misses(t);
    if (g.phase != RG_PLAYING)
        return;                    /* detect_misses ended the run */

    /* The song played to the end.
     *
     * Not "the mixer has no more to do": the mixer runs most of a second
     * ahead of the speaker, so a channel going idle means the last of the
     * track is still on its way out. The duration is what the player hears,
     * so that is what the summary waits for. */
    if (g.music_started && g.duration_ms > 0 && t >= g.duration_ms)
        finish_song();
    else if (g.music_started && g.duration_ms <= 0) {
        /* No duration for this file: fall back to the last onset plus a tail,
         * which is what the beatmap can tell us. */
        if (t >= g.last_onset_ms + 2000)
            finish_song();
    }
}

/* --- input --------------------------------------------------------------- */

static int track_for_key(int key)
{
    switch (key) {
    case MR_KEY_LEFT:  case 'A': case 'Q':   return 0;
    case MR_KEY_UP:    case 'S':             return 1;
    case MR_KEY_DOWN:  case MR_KEY_NUMPAD1:  return 2;
    case MR_KEY_RIGHT: case MR_KEY_NUMPAD2:  return 3;
    default:                                 return -1;
    }
}

/* The summary must not be dismissed by the same tap that hit the last note,
 * so it ignores input for a moment first (the .rpy's `timer 0.8`). */
static int summary_ready(void)
{
    return g.phase == RG_ENDED &&
           (g.now - g.end_wall) >= RG_END_GRACE_MS;
}

int mr_rhythm_key(int key)
{
    int track;

    if (g.phase == RG_OFF || g.phase == RG_DONE)
        return 0;

    if (g.paused) {
        /* Escape resumes, as the dialog's `key "K_ESCAPE" action no_action`
         * does; Y gives up. */
        if (key == MR_KEY_ESCAPE || key == 'N') {
            resume_run();
        } else if (key == 'Y' || key == MR_KEY_RETURN) {
            mr_rhythm_stop();     /* puts the music volume back */
            g.phase = RG_DONE;
        }
        return 1;
    }

    if (g.phase == RG_ENDED) {
        if (summary_ready() && (key == MR_KEY_SPACE || key == MR_KEY_RETURN)) {
            mr_rhythm_stop();
            g.phase = RG_DONE;
        }
        return 1;
    }

    if (key == MR_KEY_ESCAPE) {
        pause_run();
        return 1;
    }

    track = track_for_key(key);
    if (track < 0)
        return 0;
    if (g.phase == RG_PLAYING)
        try_hit(track, song_ms());
    return 1;
}

/* Nearest lane, as long as the click is inside its half-lane -- the whole
 * column is active, which is what the help screen promises. */
static int track_at(int x)
{
    int track = (x - RG_X_OFFSET + RG_SPACING / 2) / RG_SPACING;
    int dx;

    if (x < RG_X_OFFSET - RG_SPACING / 2)
        return -1;
    if (track < 0 || track >= RG_TRACKS)
        return -1;
    dx = x - (RG_X_OFFSET + track * RG_SPACING);
    if (dx < 0)
        dx = -dx;
    if (dx > RG_SPACING / 2)
        return -1;
    return track;
}

int mr_rhythm_hover(int x, int y)
{
    rg_dialog d;
    int was = g_dlg_hover;

    /* The quit button, which is up only while the dialog is not.
     *
     * Claims the motion only when the pointer is actually over it, and falls
     * through otherwise. The first version returned 1 whenever the button was
     * on screen, which meant this function started reporting "taken" for
     * every motion during a run -- it had always returned 0 there. Nothing
     * visible broke, and the existing hover test caught it. Over-claiming
     * input is the kind of change that surfaces somewhere else entirely.
     *
     * When the button is not up, the flag is cleared: without this, opening
     * the dialog with the pointer over the button and closing it again
     * brought the button back already lit. */
    {
        int qx, qy, qw, qh;
        int on = quit_rect(&qx, &qy, &qw, &qh) &&
                 x >= qx && x < qx + qw && y >= qy && y < qy + qh;

        if (on != g_quit_hover) {
            g_quit_hover = on;
            mr_invalidate();
        }
        if (on)
            return 1;
    }

    if (g.phase == RG_OFF || g.phase == RG_DONE || !g.paused) {
        g_dlg_hover = -1;
        return 0;
    }

    /* The same rectangle the drawing and the click use, so the lit button and
     * the one that responds cannot disagree. */
    dialog_layout(&d);
    g_dlg_hover = -1;
    if (y >= d.btn_y && y < d.btn_y + d.btn_h) {
        if (x >= d.yes_x && x < d.yes_x + d.btn_w)
            g_dlg_hover = 0;
        else if (x >= d.no_x && x < d.no_x + d.btn_w)
            g_dlg_hover = 1;
    }

    if (g_dlg_hover != was)
        mr_invalidate();
    return 1;
}

int mr_rhythm_click(int x, int y)
{
    int track;

    if (g.phase == RG_OFF || g.phase == RG_DONE)
        return 0;

    if (g.paused) {
        /* The two buttons of the give-up dialog, from the same layout that
         * drew them. */
        rg_dialog d;
        dialog_layout(&d);
        if (y >= d.btn_y && y < d.btn_y + d.btn_h) {
            if (x >= d.yes_x && x < d.yes_x + d.btn_w) {
                mr_rhythm_stop();     /* puts the music volume back */
                g.phase = RG_DONE;
                return 1;
            }
            if (x >= d.no_x && x < d.no_x + d.btn_w) {
                resume_run();
                return 1;
            }
        }
        return 1;
    }

    if (g.phase == RG_ENDED) {
        if (summary_ready()) {
            mr_rhythm_stop();
            g.phase = RG_DONE;
        }
        return 1;
    }

    /* The quit button, before the lanes: it sits above the top lane's travel
     * and a click there is not a missed note. `action Function(pause_game)`
     * in the .rpy, which is what Escape does. */
    {
        int qx, qy, qw, qh;
        if (quit_rect(&qx, &qy, &qw, &qh) &&
            x >= qx && x < qx + qw && y >= qy && y < qy + qh) {
            pause_run();
            return 1;
        }
    }

    track = track_at(x);
    if (track >= 0 && g.phase == RG_PLAYING)
        try_hit(track, song_ms());
    return 1;
}

/* --- drawing helpers ----------------------------------------------------- */

static void fill(mr_surface *s, int x, int y, int w, int h, mr_u32 c)
{
    int cx, cy, cw, chh, py, px;
    mr_u32 *row;

    mr_clip_rect(s, &cx, &cy, &cw, &chh);
    if (x < cx) { w -= cx - x; x = cx; }
    if (y < cy) { h -= cy - y; y = cy; }
    if (x + w > cx + cw) w = cx + cw - x;
    if (y + h > cy + chh) h = cy + chh - y;
    if (w <= 0 || h <= 0)
        return;

    for (py = y; py < y + h; py++) {
        row = s->px + (size_t)py * s->pitch;
        for (px = x; px < x + w; px++)
            row[px] = c;
    }
}

/* Blend a flat colour over what is there, for the dimmed backdrops the .rpy
 * draws with Solid('#000000a0'). */
static void fill_alpha(mr_surface *s, int x, int y, int w, int h,
                       mr_u32 c, unsigned a)
{
    int cx, cy, cw, chh, py, px;
    unsigned ia = 255u - a;
    mr_u32 *row;

    mr_clip_rect(s, &cx, &cy, &cw, &chh);
    if (x < cx) { w -= cx - x; x = cx; }
    if (y < cy) { h -= cy - y; y = cy; }
    if (x + w > cx + cw) w = cx + cw - x;
    if (y + h > cy + chh) h = cy + chh - y;
    if (w <= 0 || h <= 0)
        return;

    for (py = y; py < y + h; py++) {
        row = s->px + (size_t)py * s->pitch;
        for (px = x; px < x + w; px++) {
            mr_u32 d = row[px];
            row[px] = MR_ARGB(255,
                MR_DIV255(MR_R(c) * a + (unsigned)MR_R(d) * ia),
                MR_DIV255(MR_G(c) * a + (unsigned)MR_G(d) * ia),
                MR_DIV255(MR_B(c) * a + (unsigned)MR_B(d) * ia));
        }
    }
}

/* Centred, outlined text. `size` is the size the .rpy asks for; the nearest
 * baked font is used, which for the big headings is smaller than requested --
 * the atlas stops at 50 and baking a 120 of this game's charset would cost
 * well over a megabyte for three digits shown for three seconds. */
static void text_at(mr_surface *s, int size, const char *str,
                    int cx, int y, mr_u32 color)
{
    mr_font *f = mr_ui_font(size);
    int w;

    if (!f || !str || !*str)
        return;
    w = mr_font_text_width(f, str);
    mr_font_draw_outlined(s, f, cx - w / 2, y, str, color, RG_C_BLACK);
}

/* Every string the minigame shows is one the .rpy wrote as `_("...")`, so it
 * goes through the catalogue like any other button label. The text is drawn
 * from the French original the script is written in, which is also the key;
 * a lookup is two hashes and a binary search, cheap enough per frame.
 *
 * Score: and Combo x are deliberately absent: the .rpy builds those without
 * `_()`, so there is nothing to look up. "GAME OVER", "PERFECT" and "GREAT"
 * have no catalogue entry either -- they read the same in both languages. */
#define T(s) mr_tl_string(s)

static int text_h(int size)
{
    mr_font *f = mr_ui_font(size);
    return f ? mr_font_line_height(f) : size;
}

/* `align (ax, ay)` on the stage: the y a box of height h lands at. */
static int align_y(float ay, int h)
{
    return (int)((MR_STAGE_H - h) * ay);
}

/* The quit button's rectangle, and whether it is on screen at all.
 *
 * `showif not is_paused and not has_ended and countdown_text is None` in the
 * .rpy: during the run proper and at no other time. countdown_text is None
 * exactly once the countdown has finished, which is RG_PLAYING here, and
 * that also rules out the summary without needing has_ended separately.
 *
 * One function for the geometry because the drawing, the hover and the click
 * all need it; dialog_layout exists for the same reason, after the dialog's
 * lit button and its responding button once disagreed.
 *
 * Returns 0 when the button is not up, in which case nothing may draw or
 * hit-test it. The size is the idle image's, measured at start; the hover
 * image is assumed to match it, as an imagebutton's states do. */
static int quit_rect(int *x, int *y, int *w, int *h)
{
    if (g.phase != RG_PLAYING || g.paused || g.quit_w <= 0)
        return 0;

    *w = g.quit_w;
    *h = g.quit_h;
    *x = MR_STAGE_W - g.quit_w - RG_QUIT_MARGIN;
    *y = RG_QUIT_MARGIN;
    return 1;
}

/* --- drawing ------------------------------------------------------------- */

static void draw_quit_button(mr_surface *stage)
{
    int x, y, w, h;
    mr_surface *s;

    if (!quit_rect(&x, &y, &w, &h))
        return;

    s = mr_image_get(g_quit_hover ? RG_IMG_QUIT_HOVER : RG_IMG_QUIT_IDLE);
    if (s)
        mr_blit(stage, s, x, y);
}

static void draw_background(mr_surface *stage)
{
    mr_surface *bg = g.have_bg ? mr_image_get(RG_IMG_BG) : NULL;

    if (bg)
        mr_blit(stage, bg, 0, 0);
    else
        mr_surface_fill(stage, RG_C_BLACK);
}

static void draw_health(mr_surface *stage)
{
    int w;
    mr_u32 c;

    /* fixed xpos 10 ypos 10 xysize (228,26), Solid('#000000a0') behind */
    fill_alpha(stage, 10, 10, 228, 26, RG_C_BLACK, 160);

    if (g.health <= 0)
        return;

    w = 224 * g.health / RG_MAX_HEALTH;
    if (w <= 0)
        return;

    /* green, then amber, then red as the run gets dangerous */
    if (g.health * 2 > RG_MAX_HEALTH)
        c = RG_C_GREEN;
    else if (g.health * 4 > RG_MAX_HEALTH)
        c = RG_C_AMBER;
    else
        c = RG_C_RED;

    fill(stage, 12, 12, w, 22, c);
}

static void draw_hud(mr_surface *stage)
{
    char buf[64];
    mr_font *f = mr_ui_font(28);

    draw_health(stage);

    if (!f)
        return;

    sprintf(buf, "Score: %ld", g.score);
    mr_font_draw_outlined(stage, f, 250, 5, buf, RG_C_WHITE, RG_C_BLACK);

    if (g.combo > 1) {
        sprintf(buf, "Combo x%ld (+%ld%%)", g.combo, g.combo * 10);
        mr_font_draw_outlined(stage, f, 250, 5 + mr_font_line_height(f) + 2,
                              buf, RG_C_AMBER, RG_C_BLACK);
    }
}

static void draw_tracks_and_bar(mr_surface *stage, long t)
{
    int i;
    long progress_den;
    int filled;

    for (i = 0; i < RG_TRACKS; i++)
        fill(stage, RG_X_OFFSET + i * RG_SPACING - RG_TRACK_WIDTH / 2, 0,
             RG_TRACK_WIDTH, RG_TRACK_H, RG_C_WHITE);

    /* The bar fills as the song progresses. Once the run is over the clock
     * stops, so it does not keep filling behind the summary. */
    if (g.have_ended_at)
        t = g.ended_at;

    progress_den = g.duration_ms > 0 ? g.duration_ms : g.last_onset_ms;
    if (progress_den <= 0)
        progress_den = 1;

    filled = (int)((t > 0 ? t : 0) * RG_BAR_W / progress_den);
    if (filled > RG_BAR_W)
        filled = RG_BAR_W;

    if (filled > 0)
        fill(stage, RG_BAR_X, RG_TRACK_H, filled, RG_BAR_H, RG_C_BAR_FILL);
    if (filled < RG_BAR_W)
        fill(stage, RG_BAR_X + filled, RG_TRACK_H, RG_BAR_W - filled,
             RG_BAR_H, RG_C_BAR);
}

static void draw_notes(mr_surface *stage, long t)
{
    int i;

    for (i = g.first_live; i < g.n_notes; i++) {
        long before = g.onset_ms[i] - t;
        int  track, big, y_center, w, x, y;
        mr_surface *art;

        if (before > RG_NOTE_OFFSET_MS)
            break;                       /* and neither is anything after */
        if (g.status[i] != 0)
            continue;
        /* A note stays on screen until it leaves the hit window, so a late
         * tap counts as much as an early one. */
        if (before < -RG_HIT_MS)
            continue;

        track = g.track[i];
        art = mr_image_get(RG_NOTE_NAMES[track]);
        if (!art)
            continue;

        big = (before <= RG_HIT_MS && before >= -RG_HIT_MS);
        w = big ? RG_NOTE_BIG_W : RG_NOTE_W;

        /* 1 = just appeared at the top, 0 = at the hit-zone bar */
        y_center = RG_BAR_MID_Y -
                   (int)(before * (long)RG_NOTE_TRAVEL / RG_NOTE_OFFSET_MS);

        x = RG_X_OFFSET + track * RG_SPACING - w / 2;
        y = y_center - w / 2;

        if (big)
            mr_blit_stretch(stage, art, 0, 0, art->w, art->h, x, y, w, w);
        else
            mr_blit(stage, art, x, y);
    }
}

static void draw_judgement(mr_surface *stage, long t)
{
    long elapsed;
    const char *str;
    mr_u32 color;

    if (g.judge == RG_J_NONE)
        return;

    elapsed = t - g.judge_at;
    if (elapsed < 0 || elapsed > RG_JUDGE_MS) {
        g.judge = RG_J_NONE;
        return;
    }

    if (g.judge == RG_J_PERFECT) {
        str = "PERFECT";
        color = RG_C_AMBER;
    } else {
        str = "GREAT";
        color = RG_C_CYAN;
    }

    /* The .rpy dissolves this in and out over 0.1s at each end. There is no
     * per-draw alpha for text here, so it simply appears and goes; on a
     * machine drawing ten frames a second the fade was never going to be
     * more than a frame or two anyway. */
    text_at(stage, 44, str, MR_STAGE_W / 2,
            RG_TRACK_H - text_h(44) - 20, color);
}

static void draw_countdown(mr_surface *stage, long t)
{
    long before = -t - RG_NOTE_OFFSET_MS;   /* ms until the notes start */
    char d[2];

    if (before <= 0)
        return;
    d[0] = (char)('0' + (int)(before / 1000) + 1);
    d[1] = '\0';
    if (d[0] < '1' || d[0] > '9')
        return;

    text_at(stage, 120, d, MR_STAGE_W / 2,
            align_y(0.5f, text_h(120)), RG_C_WHITE);
}

static void draw_summary(mr_surface *stage)
{
    char buf[64];

    if (g.game_over)
        text_at(stage, 72, "GAME OVER", MR_STAGE_W / 2,
                align_y(0.35f, text_h(72)), RG_C_RED);

    /* Both banners can happen on the same run, so they get their own line. */
    if (g.is_full_combo)
        text_at(stage, 48, T("Full combo !"), MR_STAGE_W / 2,
                align_y(0.33f, text_h(48)), RG_C_GREEN);

    if (g.new_highscore)
        text_at(stage, 48, T("Nouveau record !"), MR_STAGE_W / 2,
                align_y(0.45f, text_h(48)), RG_C_AMBER);

    sprintf(buf, "PERFECT %ld    GREAT %ld", g.n_perfect, g.n_great);
    text_at(stage, 32, buf, MR_STAGE_W / 2,
            align_y(0.57f, text_h(32)), RG_C_WHITE);

    if (summary_ready())
        text_at(stage, 28, T("Appuyez sur Espace ou cliquez pour continuer."),
                MR_STAGE_W / 2, align_y(0.68f, text_h(28)), RG_C_WHITE);
}

/* --- the give-up dialog --------------------------------------------------
 *
 * Laid out to look like `screen confirm`, which is what the player sees
 * everywhere else: the same gui/frame.png nine-slice with the same
 * gui.confirm_frame_borders, a prompt, and two plain text buttons under it.
 * The box is sized to its contents rather than given fixed numbers, so a
 * translation that is longer than the French is not clipped.
 *
 * One rectangle is computed and then both drawn and hit-tested from it. The
 * two used to be written out separately and agreeing by hand is exactly the
 * sort of thing that drifts.
 */
#define RG_DLG_PAD      40      /* gui.confirm_frame_borders */
#define RG_DLG_PROMPT   32
#define RG_DLG_BUTTON   30
#define RG_DLG_GAP      34      /* prompt to buttons */
#define RG_DLG_SPACING  60      /* between the two buttons */
#define RG_DLG_BTN_PAD  30      /* around a button's label */

static const char RG_DLG_TEXT[] = "Quitter le jeu de rythme ?";
static const char RG_DLG_YES[]  = "Oui";
static const char RG_DLG_NO[]   = "Non";

static int text_w(int size, const char *str)
{
    mr_font *f = mr_ui_font(size);
    return f ? mr_font_text_width(f, str) : 0;
}

static void dialog_layout(rg_dialog *d)
{
    /* Measured on the translation, not the original: "Quit the rhythm game?"
     * is a different width, and the box is sized to what is drawn. */
    int prompt_w = text_w(RG_DLG_PROMPT, T(RG_DLG_TEXT));
    int prompt_h = text_h(RG_DLG_PROMPT);
    int yes_w = text_w(RG_DLG_BUTTON, T(RG_DLG_YES));
    int no_w = text_w(RG_DLG_BUTTON, T(RG_DLG_NO));
    int row_w;

    /* Both buttons get the width of the wider label, as a Ren'Py hbox of
     * equally styled buttons ends up looking. */
    d->btn_w = (yes_w > no_w ? yes_w : no_w) + RG_DLG_BTN_PAD * 2;
    d->btn_h = text_h(RG_DLG_BUTTON) + RG_DLG_BTN_PAD;
    row_w = d->btn_w * 2 + RG_DLG_SPACING;

    d->w = (prompt_w > row_w ? prompt_w : row_w) + RG_DLG_PAD * 2;
    d->h = prompt_h + RG_DLG_GAP + d->btn_h + RG_DLG_PAD * 2;
    d->x = (MR_STAGE_W - d->w) / 2;
    d->y = (MR_STAGE_H - d->h) / 2;

    d->prompt_y = d->y + RG_DLG_PAD;
    d->btn_y = d->prompt_y + prompt_h + RG_DLG_GAP;
    d->yes_x = MR_STAGE_W / 2 - RG_DLG_SPACING / 2 - d->btn_w;
    d->no_x = MR_STAGE_W / 2 + RG_DLG_SPACING / 2;
}

static void draw_pause(mr_surface *stage)
{
    rg_dialog d;

    /* `screen confirm` dims with gui/overlay/confirm.png. A flat fill is the
     * same thing to look at and does not stretch a full-screen PNG onto the
     * stage every frame, which this display redraws completely. */
    fill_alpha(stage, 0, 0, MR_STAGE_W, MR_STAGE_H, RG_C_BLACK, 160);

    dialog_layout(&d);
    mr_sl_draw_frame_image(stage, "gui/frame.png", 40, 40, 40, 40,
                           d.x, d.y, d.w, d.h);

    text_at(stage, RG_DLG_PROMPT, T(RG_DLG_TEXT), MR_STAGE_W / 2,
            d.prompt_y, RG_C_WHITE);

    /* The colours come from the style the dialog is imitating rather than
     * being picked here, so it lights the same blue as a real confirm button
     * and follows the game's gui.hover_color instead of a constant that would
     * be right for one game only. */
    {
        mr_u32 idle = mr_sl_style_color("confirm_button_text", "idle_",
                                        "color", RG_C_WHITE);
        mr_u32 hover = mr_sl_style_color("confirm_button_text", "hover_",
                                         "color", idle);
        int ty = d.btn_y + (d.btn_h - text_h(RG_DLG_BUTTON)) / 2;

        text_at(stage, RG_DLG_BUTTON, T(RG_DLG_YES), d.yes_x + d.btn_w / 2,
                ty, g_dlg_hover == 0 ? hover : idle);
        text_at(stage, RG_DLG_BUTTON, T(RG_DLG_NO), d.no_x + d.btn_w / 2,
                ty, g_dlg_hover == 1 ? hover : idle);
    }
}

void mr_rhythm_render(mr_surface *stage, long now_ms)
{
    long t;

    if (g.phase == RG_OFF)
        return;

    g.now = now_ms;
    t = g.paused ? (g.pause_at - g.paused_total - g.anchor) : song_ms();

    /* Freeze the progress bar where the run stopped. */
    if (g.phase == RG_ENDED && !g.have_ended_at) {
        g.ended_at = t;
        g.have_ended_at = 1;
    }

    mr_image_begin_frame();
    draw_background(stage);

    if (g.phase == RG_COUNTDOWN) {
        /* Only the big centred digit during the countdown, as the .rpy
         * returns early before drawing anything else. */
        draw_countdown(stage, t);
        /* ...except the give-up dialog, which can be opened during the
         * countdown too: without this the run froze with nothing on screen
         * to say why or how to get out of it. */
        if (g.paused)
            draw_pause(stage);
        return;
    }

    draw_tracks_and_bar(stage, t);

    if (g.phase == RG_PLAYING) {
        draw_notes(stage, t);
        draw_judgement(stage, t);
        draw_hud(stage);
        /* After the HUD, so it sits on top of anything the HUD puts near the
         * corner, and before the dialog, which covers everything. */
        draw_quit_button(stage);
    } else {
        draw_summary(stage);
    }

    if (g.paused)
        draw_pause(stage);
}

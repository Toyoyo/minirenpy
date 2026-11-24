/* `renpy.movie_cutscene()` stood in for by a frame and a soundtrack.
 *
 * There is no video decoder on the target, so a cutscene shows the video's
 * first frame while its audio plays, for as long as the video ran. Both
 * stand-ins are derived from the video's filename by tools/mkcutscene.py at
 * build time and re-derived from the same filename here at runtime, so what
 * this checks is that the two derivations agree and that the wait behaves.
 *
 * Two things went wrong on the way in and are pinned below:
 *
 *   - the splashscreen label was never executed at all, which is the real
 *     reason this game's intro never appeared;
 *   - mr_audio_duration_ms returns 0 immediately after mr_audio_play, because
 *     an MP3's length is worked out from the first decoded frame, so asking
 *     at that moment cut every cutscene down to the fallback.
 *
 * Takes the pak first and the game directory second: it needs the real
 * extracted audio/intro.mp3, which only the archive has.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "mr.h"
#include "expr.h"
#include "interp.h"
#include "vfs.h"
#include "script.h"
#include "scene.h"
#include "audio.h"
#include "ui.h"
#include "image.h"

static mr_surface *g_st;
mr_surface *mr_stage(void) { return g_st; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); exit(1); }
void mr_log(const char *f, ...) { (void)f; }
void mr_quit(void) {}
void mr_invalidate(void) {}
void (*hostshim_wave_sink)(const short *, int) = NULL;

static int g_fail;
static void check(const char *l, long got, long want)
{
    if (got != want) g_fail++;
    printf("%-54s %-8ld %s\n", l, got, got == want ? "ok" : "** WANT DIFFERENT **");
}

static void check_ge(const char *l, long got, long least)
{
    int ok = got >= least;
    if (!ok) g_fail++;
    printf("%-54s %-8ld %s\n", l, got,
           ok ? "ok" : "** WANT DIFFERENT **");
    if (!ok)
        printf("%-54s wanted at least %ld\n", "", least);
}

static long g_t = 1000;

/* main.c's loop calls mr_audio_poll every frame, and it has to be here too:
 * the mixer only advances when something pulls audio, and an MP3's length is
 * worked out from its first *decoded* frame. Without the poll the duration
 * never becomes known, the cutscene falls back to its floor, and the test
 * looks like an engine bug rather than a missing pump. */
static void frame(void)
{
    mr_audio_poll();
    mr_interp_update(g_t);
    g_t += 33;
}

static int shown(const char *name)
{
    int i;
    for (i = 0; i < mr_scene_screen_count(); i++) {
        const char *n = mr_scene_screen_name(i);
        if (n && !strcmp(n, name))
            return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    mr_script *s;
    const char *pak = (argc > 1) ? argv[1] : "mrpy.pak";
    const char *dir = (argc > 2) ? argv[2] : "game";

    setbuf(stdout, NULL);

    mr_vfs_init(pak, dir);
    mr_image_init(dir);
    mr_scene_init();
    s = mr_script_load(dir);
    if (!s) { printf("no script\n"); return 1; }
    mr_interp_init(s, dir);
    mr_ui_init(dir);
    mr_audio_init(dir);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);

    /* A game without a cutscene is not a failure, it is a game without a
     * cutscene -- so say so and skip, rather than printing three red lines
     * about missing assets. 77 is the convention run.sh understands.
     *
     * The alternative was to put a cutscene in the fixture, which would
     * test the engine against assets we generated to suit it. The subject
     * here is the *build convention* -- `wmake data` turning intro.webm
     * into audio/intro.mp3 and images/intro.png, and the script finding
     * them by those names -- and a fixture cannot be evidence about a
     * convention it was written to satisfy. */
    if (!mr_vfs_exists("audio/intro.mp3") ||
        !mr_vfs_exists("images/intro.png") ||
        mr_script_find_label(s, "splashscreen") < 0) {
        printf("this game has no cutscene (audio/intro.mp3, "
               "images/intro.png, label splashscreen) -- skipping\n");
        return 77;
    }

    /* The assets the build step is supposed to have produced, named after the
     * video. If these are missing, `wmake data` has not been run and the rest
     * of this measures the fallback rather than the feature. */
    check("the soundtrack was extracted",
          mr_vfs_exists("audio/intro.mp3"), 1);
    check("the first frame was extracted",
          mr_vfs_exists("images/intro.png"), 1);

    /* The splashscreen has to actually run. This is the label the game puts
     * the cutscene in, and nothing executed it before. */
    check("the game has a splashscreen label",
          mr_script_find_label(s, "splashscreen") >= 0, 1);

    mr_interp_splashscreen();
    frame();

    /* Asked *after* mr_interp_start, because that is what runs the init
     * phase which registers the images. Asked before it, this read 0 and
     * looked like a missing asset. */
    check("the frame is registered as an image, so `scene intro` resolves",
          mr_image_exists("intro"), 1);

    /* One frame in, the cutscene is up and waiting. */
    check("the cutscene is waiting", mr_interp_waiting(), 1);
    check("  with the quick menu suppressed", shown("quick_menu"), 0);

    /* The length comes off the track, not from a constant: intro.webm is
     * 3.003s, so the wait must be far past the 2000ms floor that applies
     * while the length is still unknown. Stepping until it ends and counting
     * is what proves the lazy resolve happened. */
    {
        long start = g_t;
        long dur;
        int i, limit;

        /* The frame budget comes off the track, not from a constant.
         *
         * This was `i < 400`, which is 13.2 seconds and was chosen for a
         * 3.003-second intro. Another game's cutscene is as long as its own
         * cutscene: Hinata's outlasted the budget, so the loop gave up
         * while the engine was correctly still waiting and "and it did end"
         * failed. The floor check below is the one that needs a constant,
         * and 2500 is about the 2000ms unknown-length floor rather than
         * about any particular video.
         *
         * The duration is read *inside* the loop, not before it. One frame
         * in it still reads 0 -- the length comes from the first decoded
         * frame, which is the whole point of the lazy resolve this test is
         * about (see the note at the top of this file) -- so asking early
         * gives nothing and asking once gives nothing either. The budget
         * therefore starts at the old 400 and grows the moment the track
         * admits its length. If it never does, 400 frames still bound the
         * loop and the assertions below still fail. */
        dur = 0;
        limit = 400;
        for (i = 0; i < limit && mr_interp_waiting(); i++) {
            frame();
            if (!dur) {
                dur = mr_audio_duration_ms(MR_CH_MUSIC);
                if (dur > 0)
                    limit = (int)(dur / 33) + 90;
            }
        }
        printf("   soundtrack %ldms, budget %d frames, used %d\n",
               dur, limit, i);
        check_ge("it lasted the soundtrack's length, not the floor",
                 g_t - start, 2500);
        check("  and it did end", mr_interp_waiting(), 0);
    }

    /* Escape and the right-click do nothing during a splashscreen. Ren'Py's
     * `label _game_menu` returns at once while `_game_menu_screen` is unset
     * (00gamemenu.rpy:166), and _start unsets it for exactly the length of
     * the splashscreen (00start.rpy:249). Here it used to open the main menu
     * over the intro. */
    /* Back to how startup looks. The run above ended in the main menu, so
     * without this the checks below read that leftover state instead of the
     * toggle's effect -- which is exactly what they did at first, and they
     * failed for that reason rather than for a fault in the guard. */
    mr_var_set_int("main_menu", 0);
    mr_scene_clear();

    mr_interp_splashscreen();
    frame();
    check("a fresh cutscene is waiting", mr_interp_waiting(), 1);
    check("  starting from no menu", mr_var_get_int("main_menu", 0), 0);
    mr_interp_game_menu_toggle();
    frame();
    /* The screen count is the assertion that bites. `main_menu` is *not* --
     * opening the game menu leaves it 0, since that variable means "we are
     * on the title screen", so a check on it reads 0 whether the guard works
     * or not and proves nothing. */
    check("  Escape opens no screen over it", mr_scene_screen_count(), 0);
    check("  and the cutscene is still running", mr_interp_waiting(), 1);

    /* A click, though, cuts it short: Ren'Py always adds a saybehavior, so a
     * cutscene is always interruptible (exports/mediaexports.py:60). */
    mr_interp_advance();
    frame();
    check("  a click ends it early", mr_interp_waiting(), 0);

    /* And its `return` lands in the main menu, which is what Ren'Py's
     * _start does after the splashscreen returns. */
    {
        int i;
        for (i = 0; i < 60 && !mr_var_get_int("main_menu", 0); i++)
            frame();
        check("the splashscreen returns to the main menu",
              mr_var_get_int("main_menu", 0), 1);
    }

    printf("\n%d failed\n", g_fail);
    return g_fail != 0;
}

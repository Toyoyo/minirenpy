/* Skipping, and the three "Avance rapide" preferences.
 *
 * The model is Ren'Py's: Ctrl held skips, Tab toggles, and the skip stops by
 * itself at anything the preferences say it may not cross.
 *
 *   "Texte non lu"    off -> stop at a line never read before
 *   "Apres les choix" off -> stop at a menu
 *   "Transitions"     off -> suppress `with` globally, skip or no skip
 *
 * The last one is not really a skip preference at all: Ren'Py checks it in
 * displayexports.py:684 whatever the skip state is. It is checked here
 * anyway because it shares the column and is easy to wire to the wrong
 * condition.
 *
 * Seen-text is the part with teeth. It is keyed on the translation
 * identifier, and persists, so this also checks it survives a save/load of
 * PERSIST.SAV -- a skip that stops at every line after a restart would make
 * the whole feature pointless.
 *
 * Takes the story fixture.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "mr.h"
#include "expr.h"
#include "interp.h"
#include "persist.h"
#include "vfs.h"
#include "script.h"
#include "image.h"
#include "scene.h"
#include "ui.h"
#include "slrender.h"
#include "pyfn.h"

static mr_surface *g_st;
mr_surface *mr_stage(void) { return g_st; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); exit(1); }
#include <stdarg.h>
void mr_log(const char *f, ...) { (void)f; }
void mr_quit(void) {}
void mr_invalidate(void) {}
void (*hostshim_wave_sink)(const short *, int) = NULL;

static int g_fail;
static void check(const char *l, long got, long want)
{
    if (got != want) g_fail++;
    printf("%-52s %-6ld %s\n", l, got, got == want ? "ok" : "** WANT DIFFERENT **");
}

static long g_t;
static void step(int frames)
{
    int i;
    for (i = 0; i < frames; i++) {
        g_t += 33;
        mr_interp_update(g_t);
    }
}

/* Read the current line, or "" when none is up. */
static const char *line(void)
{
    static char buf[1024];
    const char *who = NULL, *what = NULL;
    mr_u32 col;
    int shown;
    if (!mr_interp_dialogue(&who, &what, &col, &shown) || !what)
        return "";
    strncpy(buf, what, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    return buf;
}

/* Step until a line of dialogue is up. The fixture opens with a transition,
 * so the first say is several frames away and a single update lands before
 * it -- which made the first version of this test report that skipping had
 * not moved when there was nothing to move. */
static int g_wait_frames;
static int wait_line(void)
{
    int i;
    for (i = 0; i < 200; i++) {
        if (line()[0]) {
            g_wait_frames = i;
            return 1;
        }
        step(1);
    }
    g_wait_frames = -1;
    return 0;
}

/* Advance by clicking, the way a player would. */
static void advance(int n)
{
    int i;
    for (i = 0; i < n; i++) {
        mr_interp_advance();
        step(1);
    }
}

int main(int argc, char **argv)
{
    mr_script *s;
    char first[1024];
    int seen_before;

    setbuf(stdout, NULL);
    (void)argc;
    remove("PERSIST.SAV");
    mr_vfs_init(NULL, argv[1]); mr_image_init(argv[1]); mr_scene_init();
    s = mr_script_load(argv[1]);
    mr_interp_init(s, argv[1]);
    mr_interp_start("start");
    mr_ui_init(argv[1]);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);

    check("a line is up", wait_line(), 1);
    strcpy(first, line());
    /* The set starts empty and the line just displayed is now in it: seen is
     * recorded on display, as Ren'Py does, not on dismissal. */
    check("displaying a line records it as seen",
          mr_persist_seen_count() > 0, 1);
    printf("  seen after one line: %d\n", mr_persist_seen_count());

    /* --- skipping is off by default --- */
    check("not skipping to begin with", mr_interp_skipping(), 0);
    step(5);
    check("  so the line stays put", !strcmp(line(), first), 1);

    /* --- Ctrl, over unread text, with "Texte non lu" off --- */
    mr_var_set_int("_preferences.skip", 0);
    mr_interp_skip_hold(1);
    check("Ctrl starts a skip", mr_interp_skipping(), 1);
    step(10);
    /* Everything ahead is unread, so it must stop almost at once -- one
     * line at most, the one already on screen. */
    check("  it stops at unread text", mr_interp_skipping(), 0);
    mr_interp_skip_hold(0);

    /* --- read some, then skip back over it --- */
    {
        int n = mr_persist_seen_count();
        advance(6);
        check("reading advances the seen set",
              mr_persist_seen_count() > n, 1);
        printf("  seen now: %d\n", mr_persist_seen_count());
    }

    seen_before = mr_persist_seen_count();

    /* Start again and skip. The read lines go past one per frame, and the
     * skip must stop of its own accord at the first line beyond them --
     * which is the whole point of the feature, so it is asserted as a
     * boundary rather than as "it moved".
     *
     * Counting frames is what makes it exact: the run stops after as many
     * frames as there were read lines. An earlier version stepped a fixed 4
     * and landed one frame short of the boundary, reporting that skipping
     * had not moved when it had moved four times. */
    mr_interp_start("start");
    wait_line();
    strcpy(first, line());
    mr_interp_skip_hold(1);
    {
        int f, moved = 0, stopped_at = -1;
        for (f = 0; f < 30; f++) {
            step(1);
            if (strcmp(line(), first))
                moved = 1;
            if (!mr_interp_skipping()) {
                stopped_at = f;
                break;
            }
        }
        check("skipping crosses text already read", moved, 1);
        check("  and stops on its own at the first unread line",
              stopped_at >= 0, 1);
        printf("  stopped after %d frames, %d lines were read\n",
               stopped_at, seen_before);
        /* One frame past the read lines, not level with them: the first
         * unread line is *displayed* and the skip stops on the frame after.
         * That is the right way round -- stopping before drawing it would
         * leave the previous line on screen and the player would never see
         * the one they stopped at. */
        check("  having crossed exactly the lines that were read",
              stopped_at, seen_before + 1);
    }
    mr_interp_skip_hold(0);

    /* Which is also why the seen count goes up by exactly one: the line the
     * skip stopped at has now been shown, so it counts as read. */
    check("  and only the line it stopped at is newly seen",
          mr_persist_seen_count(), seen_before + 1);
    seen_before = mr_persist_seen_count();

    /* --- "Texte non lu" on skips everything ---
     *
     * after choices on as well, or the fixture's menu stops the run before
     * 40 frames are up and this reads as a failure of the wrong thing. */
    mr_interp_start("start");
    wait_line();
    mr_var_set_int("_preferences.skip", 1);
    mr_var_set_int("_preferences.after choices", 1);
    mr_interp_skip_toggle();
    check("Tab starts a skip", mr_interp_skipping(), 1);
    step(20);
    check("  with unseen allowed it keeps going",
          mr_interp_skipping(), 1);
    check("  and reached new text",
          mr_persist_seen_count() > seen_before, 1);
    mr_interp_skip_stop();

    /* --- a menu stops it unless allowed --- */
    mr_interp_start("start");
    wait_line();
    mr_var_set_int("_preferences.skip", 1);
    mr_var_set_int("_preferences.after choices", 0);
    mr_interp_skip_toggle();
    step(400);                    /* the fixture ends in a menu */
    check("a menu stops the skip", mr_interp_skipping(), 0);
    {
        int n = 0;
        const char *texts[16];
        mr_interp_menu(&n, texts, 16);
        check("  and the menu is waiting", n > 0, 1);
    }

    /* --- the seen set has to outlive the process ---
     *
     * Without this the feature is pointless: every restart would put skip
     * back to stopping at the first line. It rides in a trailing section of
     * PERSIST.SAV, so a file written before the section existed still loads
     * -- which is what the second load here is really checking, since the
     * reader has to cope with the section being absent as well as present.
     */
    {
        int before = mr_persist_seen_count();

        check("there is something to persist", before > 0, 1);
        mr_persist_save();
        mr_persist_clear(1);          /* progress: drops the seen set */
        check("  clearing progress forgets it",
              mr_persist_seen_count(), 0);
        mr_persist_load();
        check("  and it comes back from the file",
              mr_persist_seen_count(), before);

        /* Reset progress for real and make sure that sticks through a
         * write/read cycle too, or "reset persistent data" would appear to
         * do nothing after a restart. */
        mr_persist_clear(1);
        mr_persist_save();
        mr_persist_load();
        check("  a cleared set stays cleared",
              mr_persist_seen_count(), 0);
    }

    /* --- a pause must not stop the skip ---
     *
     * The chapter title cards are `show screen title` + `$ renpy.pause()`,
     * an *untimed* pause, so a skip that does not cross it waits there for a
     * click and the skip is over in practice. Ren'Py crosses it: the pause
     * is an interaction, and skipping ends interactions -- "the pause will
     * still end by other means - when the user clicks or skips"
     * (statementexports.py:121), hard pauses included (line 131).
     *
     * The fixture puts its menu before the title card, so getting here at
     * all means answering the menu first. That is also why the earlier menu
     * case never caught this.
     */
    {
        int i, crossed = 0;

        mr_interp_action("Start()");
        wait_line();
        mr_var_set_int("_preferences.skip", 1);          /* skip everything */
        mr_var_set_int("_preferences.after choices", 1);
        mr_interp_skip_toggle();

        /* Run to the menu, answer it, and keep going to the title card. */
        for (i = 0; i < 200; i++) {
            int n = 0;
            const char *texts[16];
            step(1);
            if (mr_interp_menu(&n, texts, 16) && n > 0) {
                mr_interp_choose(0);
                break;
            }
        }
        check("reached and answered the menu", i < 200, 1);
        check("  still skipping afterwards", mr_interp_skipping(), 1);

        /* chapter_two opens with the title card. Past the pause there is
         * dialogue again, which is the observable. */
        for (i = 0; i < 300; i++) {
            step(1);
            if (strstr(line(), "second chapter")) {
                crossed = 1;
                break;
            }
        }
        check("  skipping crosses renpy.pause()", crossed, 1);
        mr_interp_skip_stop();
    }

    /* --- `$ _skipping = False` fences off a stretch of script ---
     *
     * What the credits sequence uses so it cannot be skipped. It is a fence,
     * not a cancel: a player holding Ctrl through it is still holding it
     * afterwards, and skipping resumes on the far side. Both halves are
     * checked, because a version that simply called skip_stop would pass the
     * first and fail the second, and "the credits played" would look right
     * while the rest of the game had quietly stopped skipping.
     */
    {
        mr_interp_action("Start()");
        wait_line();
        mr_var_set_int("_preferences.skip", 1);       /* skip everything */
        strcpy(first, line());

        mr_var_set_int("_skipping", 0);
        mr_interp_skip_hold(1);
        check("Ctrl is held", 1, 1);
        check("  but _skipping = False blocks it",
              mr_interp_skipping(), 0);
        step(10);
        check("  so the line does not move", !strcmp(line(), first), 1);
        /* And transitions come back, since they were only suppressed
         * because a skip was running. */
        check("  transitions are not suppressed either",
              mr_var_get_int("_preferences.transitions", 1) != 0, 1);

        mr_var_set_int("_skipping", 1);
        check("  putting it back resumes the held skip",
              mr_interp_skipping(), 1);
        step(4);
        check("  and the line moves again", strcmp(line(), first) != 0, 1);
        mr_interp_skip_hold(0);

        /* config.allow_skipping is the same switch at game scope. */
        mr_var_set_int("config.allow_skipping", 0);
        mr_interp_skip_hold(1);
        check("config.allow_skipping = False blocks it too",
              mr_interp_skipping(), 0);
        mr_var_set_int("config.allow_skipping", 1);
        mr_interp_skip_hold(0);
    }

    /* --- the reset button follows the script, not a hardcoded choice ---
     *
     * `persistent._clear(progress=False)` keeps the read-text record; it is
     * only `progress=True` that drops it (persistent.py:92). Ren'Py was
     * checked on this directly, so the engine has to agree rather than be
     * helpful: reset_persistent reads the `persistent._clear` line out of
     * the script's own body and evaluates it.
     *
     * Both flags are driven here, because "honours the script" is the claim
     * and one value alone cannot show it -- a hardcoded 1 passes the True
     * case and a hardcoded 0 passes the False case.
     */
    {
        int n;

        mr_interp_action("Start()");
        wait_line();
        advance(4);
        n = mr_persist_seen_count();
        check("some lines are read again", n > 0, 1);

        mr_interp_action("persistent._clear(progress=False)");
        check("  progress=False keeps the read lines",
              mr_persist_seen_count(), n);

        mr_interp_action("persistent._clear(progress=True)");
        check("  progress=True drops them",
              mr_persist_seen_count(), 0);

        /* And the button itself, whichever the script says. The fixture's
         * screens.rpy is the game's own, so this asserts agreement rather
         * than a value: whatever the script asked for is what happened. */
        mr_interp_action("Start()");
        wait_line();
        advance(4);
        n = mr_persist_seen_count();
        check("read some more", n > 0, 1);
        {
            const char *l;
            int i, wants_progress = 0;
            for (i = 0; (l = mr_pyfn_body_line("reset_persistent", i)); i++)
                if (strstr(l, "persistent._clear"))
                    wants_progress = strstr(l, "True") != NULL;
            mr_interp_action("reset_persistent");
            printf("  the script asks for progress=%s\n",
                   wants_progress ? "True" : "False");
            check("  and the button does exactly that",
                  mr_persist_seen_count() == 0, wants_progress);
        }
    }

    /* --- transitions ---
     *
     * Observable without reaching into the scene: the fixture opens with
     * `scene ... with dissolve`, so the first line of dialogue is several
     * frames away while the transition runs, and immediate when it does not.
     * Counting frames to the first line measures exactly that.
     *
     * Both reasons to suppress are checked. They are independent in Ren'Py
     * -- displayexports.py:684 tests the preference, and line 687 tests
     * config.skipping separately -- so wiring one to the other would pass a
     * test that only looked at one of them. */
    check("transitions are on by default",
          mr_var_get_int("_preferences.transitions", 1) != 0, 1);

    {
        int with_trans, without, skipping_frames;

        /* Start() rather than mr_interp_start: the latter only jumps, so
         * the scene is still showing what the last run left there and the
         * opening `scene ... with dissolve` has nothing to dissolve *to*.
         * Measured that way, the second run is instant whatever the
         * preference says -- which is how the first version of this block
         * passed against a build that ignored the preference entirely. */
        mr_var_set_int("_preferences.transitions", 1);
        mr_interp_skip_stop();
        mr_interp_action("Start()");
        wait_line();
        with_trans = g_wait_frames;

        mr_var_set_int("_preferences.transitions", 0);
        mr_interp_action("Start()");
        wait_line();
        without = g_wait_frames;

        printf("  frames to the first line: %d with, %d without\n",
               with_trans, without);
        /* 32 frames against 1: the surviving frame is Start() itself
         * needing one update to reach the first line, not a transition. The
         * assertion is the separation, not an exact zero. */
        check("a transition delays the first line", with_trans > 20, 1);
        check("  and turning transitions off removes the delay",
              without < 5, 1);

        /* And skipping suppresses them whatever the preference says. */
        mr_var_set_int("_preferences.transitions", 1);
        mr_var_set_int("_preferences.skip", 1);
        mr_interp_action("Start()");
        mr_interp_skip_hold(1);
        wait_line();
        skipping_frames = g_wait_frames;
        mr_interp_skip_hold(0);
        printf("  while skipping, with transitions on: %d\n", skipping_frames);
        check("skipping suppresses them too", skipping_frames < 5, 1);
    }

    printf("\n%d failed\n", g_fail);
    remove("PERSIST.SAV");
    return g_fail != 0;
}

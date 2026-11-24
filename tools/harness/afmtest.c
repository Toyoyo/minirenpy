/* Auto-forward: the quick menu's "Auto" button.
 *
 *     textbutton _("Auto") action Preference("auto-forward", "toggle")
 *
 * It did nothing whatsoever. The wait itself was implemented and correct, and
 * it reads `_preferences.afm_enable` -- Ren'Py's field name. The action stored
 * `_preferences.auto-forward`, the Preference's own first argument, because
 * that is the convention the rest of the preferences use and nothing had ever
 * needed the real field. So the button lit up when clicked, stayed lit,
 * survived a save, and the story never advanced by itself.
 *
 * That is the shape of bug this file is here to catch, so the checks are
 * deliberately split:
 *
 *   - the action moves the field the *interpreter* reads, not a name only the
 *     button and its own selected-query agree on. Asserting the button reads
 *     back selected is exactly what passes with the bug present.
 *   - a line then advances with no input at all, on the clock.
 *   - and the negative control: the same span of time with Auto off must not
 *     advance it, or the first check is measuring the story running away for
 *     some other reason.
 *
 * Plus the after-click rule (behavior.py:797-804): with auto-forward running,
 * a click turns it off and is spent doing that -- it does not also advance.
 *
 * And the two places where "does Auto apply here?" has a different answer
 * from "is there a line on screen", because in Ren'Py the question is really
 * "is there a SayBehavior here, and was it given afm text?": an untimed
 * `renpy.pause()` has one and is crossed, and a choice menu has none at all,
 * so it waits however long it has to.
 *
 * And the hard pause, which no key or mouse button can end -- its dismiss
 * keysym is bound to the empty list -- so the cases for it are the two things
 * that *can*: auto-forward and skipping. `hard` used to be read on the timed
 * branch only, so the untimed form was click-dismissable, and a save taken
 * inside one came back that way too.
 *
 * Takes the story fixture directory.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "mr.h"
#include "expr.h"
#include "interp.h"
#include "vfs.h"
#include "script.h"
#include "image.h"
#include "scene.h"
#include "ui.h"
#include "save.h"

static mr_surface *g_st;
mr_surface *mr_stage(void) { return g_st; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); exit(1); }
void mr_log(const char *f, ...) { (void)f; }
void mr_quit(void) {}
void mr_invalidate(void) {}
static void sink(const short *d, int n) { (void)d; (void)n; }
void (*hostshim_wave_sink)(const short *, int) = sink;

static int  g_fail;
static long T;

static void check(const char *l, long got, long want)
{
    if (got != want) g_fail++;
    printf("%-52s %-6ld %s\n", l, got, got == want ? "ok" : "** WANT DIFFERENT **");
}

/* The line on screen, or "" when there is none. Returned into a buffer of
 * our own: the engine's pointer is to the live dialogue and would appear to
 * change under us, which is the one thing this test must be able to see. */
static char g_line[1024];
static const char *line_now(void)
{
    const char *who, *what;
    mr_u32 color;
    int shown;

    if (!mr_interp_dialogue(&who, &what, &color, &shown))
        g_line[0] = '\0';
    else {
        strncpy(g_line, what, sizeof(g_line) - 1);
        g_line[sizeof(g_line) - 1] = '\0';
    }
    return g_line;
}

/* Run the clock for `ms` with no input, stopping early if the line changes.
 * Returns how long it took to change, or -1 if it never did. */
static long wait_for_change(long ms)
{
    char was[1024];
    long start = T;

    strcpy(was, line_now());
    while (T - start < ms) {
        mr_interp_update(T);
        T += 33;
        if (strcmp(line_now(), was) != 0)
            return T - start;
    }
    return -1;
}

int main(int argc, char **argv)
{
    mr_script *s;
    long took;

    setbuf(stdout, NULL);
    (void)argc;
    mr_vfs_init(NULL, argv[1]);
    mr_image_init(argv[1]);
    mr_scene_init();
    s = mr_script_load(argv[1]);
    mr_interp_init(s, argv[1]);
    mr_ui_init(argv[1]);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);

    mr_interp_start("start");
    mr_interp_main_menu();
    mr_interp_action("Start()");

    /* Instant text, so the auto-forward clock -- which starts when the line
     * has finished appearing, not when it began -- starts on the first frame
     * and the delay under test is the only delay in play. */
    mr_expr_exec("_preferences.text_cps = 0");
    mr_expr_exec("_preferences.afm_time = 10");

    for (took = 0; took < 100 && !*line_now(); took++) {
        mr_interp_update(T);
        T += 33;
    }
    check("there is a line on screen", *line_now() != '\0', 1);

    /* --- the action has to move the field the interpreter reads --- */
    check("auto-forward starts off",
          mr_interp_pref_selected("auto-forward", "toggle"), 0);

    mr_interp_action("Preference(\"auto-forward\", \"toggle\")");

    check("clicking Auto sets afm_enable",
          mr_var_get_int("_preferences.afm_enable", 0), 1);
    /* This one passes with the bug present, which is the point of having it
     * next to the one above: a button that reads back its own invented
     * storage looks perfectly healthy. */
    check("  and the button reads back selected",
          mr_interp_pref_selected("auto-forward", "toggle"), 1);
    check("  with nothing left under the Preference's own name",
          mr_var_get_int("_preferences.auto-forward", 0), 0);

    /* --- and the story then advances on its own ---
     *
     * ((25 + length) / 250) * afm_time, so a few seconds for these lines.
     * Ten is generous enough that a slow line is not a failure. */
    took = wait_for_change(10000);
    printf("  advanced after %ld ms\n", took);
    check("the line advances with no input", took > 0, 1);
    check("  and it took some time doing it", took > 500, 1);

    took = wait_for_change(10000);
    printf("  and again after %ld ms\n", took);
    check("it keeps going", took > 0, 1);

    /* --- negative control --- */
    mr_interp_action("Preference(\"auto-forward\", \"toggle\")");
    check("Auto is off again",
          mr_var_get_int("_preferences.afm_enable", 0), 0);
    check("  and now nothing moves on its own",
          wait_for_change(10000), -1);

    /* --- afm_time 0 means never, not instantly --- */
    mr_expr_exec("_preferences.afm_enable = 1");
    mr_expr_exec("_preferences.afm_time = 0");
    check("afm_time 0 with Auto on still waits forever",
          wait_for_change(5000), -1);

    /* --- the after-click rule --- */
    mr_expr_exec("_preferences.afm_time = 10");
    {
        char was[1024];
        strcpy(was, line_now());

        mr_interp_advance();
        mr_interp_update(T);

        check("a click while auto-forwarding turns it off",
              mr_var_get_int("_preferences.afm_enable", 0), 0);
        check("  and is spent doing that, not advancing",
              strcmp(line_now(), was) == 0, 1);

        /* The next one is an ordinary click again. */
        mr_interp_advance();
        mr_interp_update(T);
        T += 33;
        check("  the click after it advances normally",
              strcmp(line_now(), was) != 0, 1);
    }

    /* With "auto-forward after click" on, the click advances and auto-forward
     * stays on -- the whole point of that preference. */
    mr_expr_exec("_preferences.afm_enable = 1");
    mr_expr_exec("_preferences.afm_after_click = 1");
    {
        char was[1024];
        strcpy(was, line_now());

        mr_interp_advance();
        mr_interp_update(T);
        T += 33;

        check("after-click on: the click advances",
              strcmp(line_now(), was) != 0, 1);
        check("  and auto-forward survives it",
              mr_var_get_int("_preferences.afm_enable", 0), 1);
    }

    /* --- a choice menu ---
     *
     * Ren'Py's auto-forward lives on the SayBehavior, and a choice menu has
     * none: menuexports.py:320 adds one only for fixed rollback, and even
     * that one is built with afm=None. So a menu is where auto-forward stops
     * and waits, however long the player leaves it -- the one thing it must
     * never do is pick for them. config.auto_choice_delay does that, and is a
     * separate feature for automated testing, default None.
     *
     * Equally, nothing *clears* afm_enable there: answering the menu is not a
     * dismiss event, so the line after the menu auto-forwards again without
     * the player having to turn Auto back on.
     *
     * A short afm_time to walk the fixture to its menu on the clock, which
     * also means the walk itself is a test of auto-forward crossing a
     * dissolve, a move, `window hide` and a `$`. */
    mr_expr_exec("_preferences.afm_after_click = 0");
    mr_expr_exec("_preferences.afm_enable = 1");
    mr_expr_exec("_preferences.afm_time = 1");
    {
        const char *texts[16];
        int count = 0;
        int i;
        long t0;

        for (i = 0; i < 4000; i++) {
            if (mr_interp_menu(&count, texts, 16) && count > 0)
                break;
            mr_interp_update(T);
            T += 33;
        }
        check("auto-forward walked the story to the menu", count, 2);
        check("  without any input at all",
              mr_var_get_int("_preferences.afm_enable", 0), 1);

        /* Twenty seconds of it, against a per-line delay of about a third of
         * a second at this afm_time. */
        t0 = T;
        while (T - t0 < 20000) {
            mr_interp_update(T);
            T += 33;
        }
        count = 0;
        check("  it is still waiting 20s later",
              mr_interp_menu(&count, texts, 16) && count == 2, 1);
        check("  and Auto was not turned off by it",
              mr_var_get_int("_preferences.afm_enable", 0), 1);

        mr_interp_choose(0);
        mr_interp_update(T);
        T += 33;
        check("  answering it leaves Auto on",
              mr_var_get_int("_preferences.afm_enable", 0), 1);

        /* --- and the untimed pause after it ---
         *
         * `show screen title` + `$ renpy.pause()`, the chapter title card.
         * renpy.pause() with no delay passes afm=" " to its SayBehavior
         * (statementexports.py:176-179), so auto-forward crosses it; a pause
         * *with* a delay passes None and relies on its own timer. Without
         * this, Auto mode stops dead on the first title card, which looks
         * exactly like Auto being broken. */
        for (i = 0; i < 2000 && !mr_interp_waiting(); i++) {
            mr_interp_update(T);
            T += 33;
        }
        check("the story reached the title card's pause",
              mr_interp_waiting(), 1);

        t0 = T;
        for (i = 0; i < 2000 && mr_interp_waiting(); i++) {
            mr_interp_update(T);
            T += 33;
        }
        printf("  the pause ended after %ld ms\n", T - t0);
        check("  auto-forward crosses it", mr_interp_waiting(), 0);
        /* One character of afm text, so about a tenth of afm_time -- far
         * shorter than a line, and not instant. */
        check("  after waiting, not instantly", T - t0 > 33, 1);
    }

    /* --- the other two kinds of pause ---
     *
     * A timed pause is given afm=None and a hard one keeps its own dismiss
     * keysym, so each row of the table in NOTES gets a case here rather than
     * being argued from the one pause the story happens to contain. The
     * fixture's `afm_pauses` label is three pauses in a row with a line
     * between them; auto-forward is on throughout, so the lines carry us from
     * one to the next without any input.
     *
     * The three are told apart by how long they last, which is the whole
     * point: at afm_time 1 an untimed pause is crossed in about 100ms and a
     * three-second pause must still take three seconds. */
    mr_interp_start("afm_pauses");
    mr_expr_exec("_preferences.afm_enable = 1");
    mr_expr_exec("_preferences.afm_after_click = 0");
    mr_expr_exec("_preferences.afm_time = 1");
    {
        int i;
        long t0, held;

        /* --- 1: timed --- */
        for (i = 0; i < 2000 && !mr_interp_waiting(); i++) {
            mr_interp_update(T);
            T += 33;
        }
        check("reached the timed pause", mr_interp_waiting(), 1);

        t0 = T;
        for (i = 0; i < 2000 && mr_interp_waiting(); i++) {
            mr_interp_update(T);
            T += 33;
        }
        held = T - t0;
        printf("  the timed pause held %ld ms\n", held);
        /* The failure this is here for: a timed pause auto-forwarded like an
         * untimed one would end after ~100ms, cutting three seconds of held
         * screen down to a flicker. */
        check("  auto-forward does not shorten it", held > 2500, 1);

        /* --- 2: hard --- */
        for (i = 0; i < 2000 && !mr_interp_waiting(); i++) {
            mr_interp_update(T);
            T += 33;
        }
        check("reached the hard pause", mr_interp_waiting(), 1);

        /* A click cannot end it, and -- because Ren'Py's afm cancel lives
         * inside the dismiss loop and a hard pause's dismiss keysym is
         * "dismiss_hard_pause" -- it must not switch auto-forward off either.
         * Getting that wrong would be invisible here and maddening in the
         * credits roll: ninety seconds the player cannot skip, with Auto
         * silently turning itself off on the way in. */
        t0 = T;
        mr_interp_advance();
        mr_interp_update(T);
        T += 33;
        check("  a click does not end it", mr_interp_waiting(), 1);
        check("  and does not switch Auto off",
              mr_var_get_int("_preferences.afm_enable", 0), 1);

        for (i = 0; i < 2000 && mr_interp_waiting(); i++) {
            mr_interp_update(T);
            T += 33;
        }
        held = T - t0;
        printf("  the hard pause held %ld ms\n", held);
        check("  auto-forward does not shorten it either", held > 2500, 1);

        /* --- 3: untimed, and the after-click rule on a pause --- */
        for (i = 0; i < 2000 && !mr_interp_waiting(); i++) {
            mr_interp_update(T);
            T += 33;
        }
        check("reached the untimed pause", mr_interp_waiting(), 1);

        mr_interp_advance();
        check("  a click here switches Auto off",
              mr_var_get_int("_preferences.afm_enable", 0), 0);
        check("  and is spent doing that, not ending the pause",
              mr_interp_waiting(), 1);

        mr_interp_advance();
        mr_interp_update(T);
        T += 33;
        check("  the click after it ends the pause", mr_interp_waiting(), 0);

        /* --- 4: an untimed hard pause ---
         *
         * `hard` used to be read on the timed branch only, so
         * `renpy.pause(hard=True)` with no delay became an ordinary
         * click-dismissable pause. It has exactly two exits in Ren'Py, which
         * is why the fixture has two of these in a row -- each case below
         * consumes one.
         *
         * Auto is off here, left that way by the block above. */
        for (i = 0; i < 2000 && !mr_interp_waiting(); i++) {
            mr_interp_advance();           /* the lines in between */
            mr_interp_update(T);
            T += 33;
        }
        check("reached the untimed hard pause", mr_interp_waiting(), 1);
        check("  with Auto off",
              mr_var_get_int("_preferences.afm_enable", 0), 0);

        t0 = T;
        for (i = 0; i < 100; i++) {
            mr_interp_advance();
            mr_interp_update(T);
            T += 33;
        }
        printf("  survived %ld ms of clicking\n", T - t0);
        check("  clicking does not end it", mr_interp_waiting(), 1);

        /* A save taken inside one has to come back hard. The restore arm for
         * an untimed pause hardcoded 0, so it came back dismissable -- the
         * field was in the save format already and simply unused for this
         * kind of pause. */
        mr_save_write("1-7", g_st);
        check("  saved inside it", mr_save_exists("1-7"), 1);
        mr_save_load("1-7");
        mr_interp_update(T);
        T += 33;
        check("  the load lands back in the pause", mr_interp_waiting(), 1);
        mr_interp_advance();
        mr_interp_update(T);
        T += 33;
        check("  and it is still hard after loading",
              mr_interp_waiting(), 1);

        /* Exit one: auto-forward. behavior.py:732 is above the dismiss loop,
         * so the empty keymap entry does not stop it. */
        mr_expr_exec("_preferences.afm_enable = 1");
        for (i = 0; i < 200 && mr_interp_waiting(); i++) {
            mr_interp_update(T);
            T += 33;
        }
        check("  auto-forward ends it", mr_interp_waiting(), 0);

        /* And the flag must not outlive the pause, or the next thing to wait
         * inherits it. The fixture's next statement is a line of dialogue;
         * `hard` belongs to the pause that asked for it. */
        mr_expr_exec("_preferences.afm_enable = 0");
        for (i = 0; i < 2000 && !mr_interp_waiting(); i++) {
            mr_interp_advance();
            mr_interp_update(T);
            T += 33;
        }
        check("reached the second untimed hard pause",
              mr_interp_waiting(), 1);

        /* Exit two: skipping. behavior.py:814 is below the dismiss loop. */
        mr_interp_skip_toggle();
        for (i = 0; i < 200 && mr_interp_waiting(); i++) {
            mr_interp_update(T);
            T += 33;
        }
        check("  skipping ends it", mr_interp_waiting(), 0);
        mr_interp_skip_stop();
    }

    printf("\n%d failed\n", g_fail);
    return g_fail != 0;
}

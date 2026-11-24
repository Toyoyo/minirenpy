/* Rollback and history. Each "line" below is one dialogue interaction, which
 * is what a rollback step moves between. */
#include <stdio.h>
#include <string.h>
#include "mr.h"
#include "save.h"
#include "history.h"
#include "expr.h"
#include "interp.h"
#include "anygame.h"
#include "vfs.h"
#include "script.h"
#include "image.h"
#include "scene.h"
#include "ui.h"
#include "slrender.h"

static mr_surface *g_st;
mr_surface *mr_stage(void) { return g_st; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); }
#include <stdarg.h>
void mr_log(const char *f, ...) {
    va_list a; if (strncmp(f, "layout", 6) && strncmp(f, "rbdbg", 5)) return;
    va_start(a, f); vprintf(f, a); va_end(a); printf("\n"); }
void mr_quit(void) {}
void mr_invalidate(void) {}
void hostshim_wave_sink(const short *d, int n) { (void)d; (void)n; }

static long T;
static char g_cur[200];

static void snap(void)
{
    const char *who, *what; mr_u32 c; int shown;
    if (mr_interp_dialogue(&who, &what, &c, &shown))
        sprintf(g_cur, "%.40s", what);
    else
        strcpy(g_cur, "(none)");
}

/* Settle on the *first* line of dialogue, without advancing past it.
 *
 * Not the same as next_line, and the difference is why six assertions
 * failed against a game while passing on the fixture: next_line advances
 * until the text *changes*, so where a label opens with a scene and a
 * transition it lands on line one, and where a label opens with a say --
 * a line already on screen by the first update -- it lands on line two.
 * Three calls then produced four history entries, and "history entries 3"
 * reported a rollback bug. The counts here are of lines this test has
 * seen, so it has to know which line it is standing on. */
static int first_line(void)
{
    int guard = 0;

    snap();
    while (!strcmp(g_cur, "(none)") && guard++ < 50) {
        mr_interp_update(T); T += 33;
        mr_interp_advance();
        mr_interp_update(T);
        snap();
    }
    return strcmp(g_cur, "(none)") != 0;
}

/* Advance until the displayed line changes: one interaction. */
static int next_line(void)
{
    char was[200];
    int guard = 0;
    snap(); strcpy(was, g_cur);
    while (guard++ < 50) {
        mr_interp_update(T); T += 33;
        mr_interp_advance();
        mr_interp_update(T);
        snap();
        if (strcmp(g_cur, was) && strcmp(g_cur, "(none)"))
            return 1;
    }
    return 0;
}

static int fails;

/* `got` is evaluated exactly once: it is often a call with side effects.
 *
 * And it counts, which it did not. `main` ended in `return 0` and run.sh
 * judges by exit code, so this harness reported `pass` with assertions
 * visibly failing in its own output -- three of them on the fixture and
 * seven against a game, unnoticed for as long as that has been true. Six
 * harnesses were in this state; see NOTES, "A test that cannot go red". */
static void check(const char *label, long got, long want)
{
    printf("%-26s %-8ld %s\n", label, got,
           got == want ? "ok" : "** WANT DIFFERENT **");
    if (got != want)
        fails++;
}
#define CHECK(label, got, want) check(label, (long)(got), (long)(want))

int main(int argc, char **argv)
{
    mr_script *s;
    char l1[200], l2[200], l3[200];

    setbuf(stdout, NULL);
    mr_vfs_init(NULL, argv[1]);
    mr_image_init(argv[1]);
    mr_scene_init();
    s = mr_script_load(argv[1]);
    mr_interp_init(s, argv[1]);
    anygame_start_story(s);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);
    mr_interp_update(T);

    first_line(); strcpy(l1, g_cur);
    mr_var_set_int("probe", 100);           /* exists from line 1 onward */

    next_line(); strcpy(l2, g_cur);
    mr_var_set_int("probe", 200);           /* changed during line 2 */

    next_line(); strcpy(l3, g_cur);
    printf("line1: %s\nline2: %s\nline3: %s\n", l1, l2, l3);
    CHECK("probe at line 3", mr_var_get_int("probe", -1), 200);
    CHECK("history entries", mr_history_count(), 3);
    CHECK("rollback depth", mr_rollback_depth(), 2);

    mr_interp_rollback(); mr_interp_update(T); snap();
    printf("after 1 back: %s\n", g_cur);
    CHECK("  back on line 2", strcmp(g_cur, l2) == 0, 1);
    CHECK("  probe unwound to 100", mr_var_get_int("probe", -1), 100);
    CHECK("  history dropped to 2", mr_history_count(), 2);

    mr_interp_rollback(); mr_interp_update(T); snap();
    printf("after 2 back: %s\n", g_cur);
    CHECK("  back on line 1", strcmp(g_cur, l1) == 0, 1);
    CHECK("  probe now unset", mr_var_get_int("probe", -1), -1);

    mr_interp_rollback(); mr_interp_update(T); snap();
    CHECK("3rd back is a no-op", strcmp(g_cur, l1) == 0, 1);

    next_line();
    CHECK("forward reaches line 2", strcmp(g_cur, l2) == 0, 1);

    /* Render the history screen: exercises `for h in _history_list`, the
     * `in` test on h.who_args and the h.who_args["color"] subscript. */
    mr_ui_init(argv[1]);
    mr_interp_show_screen("history");
    mr_interp_update(T);
    mr_sl_dump_next();
    mr_sl_focus_reset();
    mr_interp_render(g_st, T);

    {
        const char *who, *what; mr_u32 c;
        if (mr_history_get(0, &who, &what, &c))
            printf("history newest: %.40s\n", what);
        CHECK("  newest is line 2", mr_history_get(0, &who, &what, &c) &&
              strncmp(what, l2, 30) == 0, 1);
    }

    /* Close the history screen: while a menu owns the display nothing
     * advances, which is the point of it. */
    mr_interp_action("Return()");
    mr_interp_update(T);

    /* --- rollback after loading, which is the whole point ---
     *
     * Fresh state, so nothing here depends on the earlier rollbacks. */
    printf("\n-- save, wander off, load, then roll back --\n");
    {
        char sa[200], sb[200];
        int hist_at_save;

        CHECK("reached line sa", next_line(), 1); strcpy(sa, g_cur);
        mr_var_set_int("probe2", 500);      /* changes during line sa */
        CHECK("reached line sb", next_line(), 1); strcpy(sb, g_cur);
        mr_var_set_int("probe2", 600);      /* changes during line sb */
        CHECK("reached line sc", next_line(), 1);

        hist_at_save = mr_history_count();
        printf("saving on: %s (history=%d depth=%d)\n",
               g_cur, hist_at_save, mr_rollback_depth());
        CHECK("probe2 before save", mr_var_get_int("probe2", -1), 600);
        printf("   en at save = %ld\n", mr_var_get_int("config.rollback_enabled", -7));
        CHECK("write save", mr_save_write("3-1", g_st), 1);

        /* Destroy everything the save should restore.
         *
         * Somewhere else means *another label*, derived rather than named:
         * this said `mr_interp_start("credits")`, and a script without a
         * credits label simply stays where it is. "wandered away" then
         * fails, which reads as the save having restored nothing when in
         * fact nothing had moved. */
        {
            char here[200], elsewhere[MR_MAX_NAME];
            strcpy(here, g_cur);
            mr_history_reset();
            mr_rollback_reset();
            mr_var_set_int("probe2", -999);
            if (!anygame_other_label(s, anygame_label,
                                     elsewhere, sizeof(elsewhere))) {
                printf("   (only one story label; advancing instead)\n");
                next_line();
            } else {
                printf("   wandering off to %s\n", elsewhere);
                mr_interp_start(elsewhere);
            }
            mr_interp_update(T);
            /* The new label need not open with a line: Hinata's chap7
             * begins with a scene and a transition, so one update later
             * the *old* line is still the one on screen and "wandered
             * away" failed on a jump that had worked perfectly. Step
             * until the text actually changes, bounded. */
            next_line();
            printf("   en after wandering = %ld\n",
                   mr_var_get_int("config.rollback_enabled", -7));
            snap();
            CHECK("wandered away", strcmp(g_cur, here) != 0, 1);
        }

        CHECK("load", mr_save_load("3-1"), 1);
        mr_interp_update(T); snap();
        /* --- rollforward over a loaded log ------------------------------
         *
         * Reported from play as "no rollforward after a load", so all three
         * shapes are pinned here. None of this was covered: the section used
         * to load, roll back twice and stop.
         */
        {
            char at_load[200];

            strcpy(at_load, g_cur);

            /* Straight after a load there is no future, and that is correct
             * rather than a bug: nothing past the save point was ever
             * recorded, so there is nothing to replay. Ren'Py is the same --
             * `RollbackLog.forward` is pickled, but it is empty whenever the
             * player saved while playing forward, which is the normal case.
             * Pressing PageDown on a freshly loaded slot does nothing in
             * both engines. */
            CHECK("  no future straight after a load",
                  mr_rollforward_depth(), 0);

            /* One back, one forward: the saved line comes back. */
            mr_interp_rollback(); mr_interp_update(T); snap();
            CHECK("  one step back gives one step forward",
                  mr_rollforward_depth(), 1);
            mr_interp_rollforward(); mr_interp_update(T); snap();
            CHECK("  and it returns to the saved line",
                  strcmp(g_cur, at_load) == 0, 1);

            /* And across the join: points that came out of the file, then
             * points recorded in this session, walked back over in one go
             * and forward again in order. This is the mixed case, and the
             * one with the most room to be wrong -- the forward slices are
             * captured from the live store at rollback time, over deltas
             * that were read from the file. */
            mr_save_load("3-1");
            mr_interp_update(T); snap();
            {
                char seen[4][200];
                int n, i, ok = 1;

                /* The *destinations*, not the departures: four advances from
                 * the saved line, and a forward step from the saved line has
                 * to land on the first of them. */
                for (n = 0; n < 4; n++) {
                    next_line();
                    mr_interp_update(T); snap();
                    strcpy(seen[n], g_cur);
                }
                for (n = 0; n < 4; n++) {
                    mr_interp_rollback(); mr_interp_update(T);
                }
                mr_interp_update(T); snap();
                CHECK("  four new lines, four back, lands on the saved line",
                      strcmp(g_cur, at_load) == 0, 1);
                CHECK("    with four steps forward available",
                      mr_rollforward_depth(), 4);

                for (i = 0; i < 4; i++) {
                    mr_interp_rollforward(); mr_interp_update(T); snap();
                    if (strcmp(g_cur, seen[i])) {
                        printf("     forward %d -> %.40s (wanted %.40s)\n",
                               i, g_cur, seen[i]);
                        ok = 0;
                    }
                }
                CHECK("    and each forward step lands on its own line",
                      ok, 1);
            }

            /* Put it back where the rest of the section expects it. */
            mr_save_load("3-1");
            mr_interp_update(T); snap();
        }
        CHECK("  history restored", mr_history_count(), hist_at_save);
        CHECK("  probe2 restored", mr_var_get_int("probe2", -1), 600);
        CHECK("  rollback available", mr_rollback_depth() > 0, 1);

        mr_interp_rollback(); mr_interp_update(T); snap();
        printf("after load+back: %s\n", g_cur);
        CHECK("  rolled back to sb", strcmp(g_cur, sb) == 0, 1);
        CHECK("  probe2 unwound to 500", mr_var_get_int("probe2", -1), 500);

        mr_interp_rollback(); mr_interp_update(T); snap();
        CHECK("  rolled back to sa", strcmp(g_cur, sa) == 0, 1);

        /* And forward again, over ground whose log came out of a file.
         *
         * This path was untested: the section covered load, back and back
         * again, and stopped there. The question it answers is whether a
         * loaded log can be walked in both directions -- it can, and both
         * the line and the variable come back, so the forward slices the
         * rollback captured are being built from restored points correctly.
         *
         * A menu is the exception, and it is a gap in the format rather than
         * in this: the option the player took (`answer1`) is not written to
         * the file, so rollforward stops at any menu whose checkpoint was
         * loaded rather than recorded in this session. Ren'Py carries it --
         * `Rollback.forward` is an ordinary attribute of a pickled log entry
         * (rollback.py:278) -- so this is a known divergence, see NOTES. */
        printf("   fwd depth after load+back+back = %d\n",
               mr_rollforward_depth());
        mr_interp_rollforward(); mr_interp_update(T); snap();
        printf("   after one rollforward: %s\n", g_cur);
        CHECK("  rolled forward to sb again", strcmp(g_cur, sb) == 0, 1);
        CHECK("  probe2 forward to 500",
              mr_var_get_int("probe2", -1), 500);
    }

    /* --- rollforward -----------------------------------------------------
     *
     * Ren'Py's PageDown and wheel-down. Four claims, and the last two are
     * the ones that make it rollforward rather than a second advance key:
     * it does nothing at the newest point, and a rollback followed by going
     * somewhere else throws the future away rather than jumping back into
     * an abandoned branch.
     */
    {
        char at[200], back_to[200];
        long probe_at;

        printf("\n-- rollforward --\n");

        /* A variable that actually differs across the step. The first
         * version of this used probe2, which is unset here -- so it compared
         * -1 against -1 and would have passed with the delta capture ripped
         * out entirely. Written through the store so the rollback journal
         * sees it, which is the thing under test. */
        next_line();
        mr_var_set_int("rfprobe", 111);
        next_line();
        mr_var_set_int("rfprobe", 222);
        next_line();
        mr_interp_update(T); snap(); strcpy(at, g_cur);
        probe_at = mr_var_get_int("rfprobe", -1);
        CHECK("the probe is set at the newest point", probe_at, 222);

        CHECK("nothing to go forward to", mr_rollforward_depth(), 0);
        mr_interp_rollforward(); mr_interp_update(T); snap();
        CHECK("  so it does nothing", strcmp(g_cur, at) == 0, 1);

        mr_interp_rollback(); mr_interp_update(T); snap();
        strcpy(back_to, g_cur);
        CHECK("rolled back", strcmp(g_cur, at) != 0, 1);
        CHECK("  the probe unwound with it",
              mr_var_get_int("rfprobe", -1) != probe_at, 1);
        CHECK("  a step is now available", mr_rollforward_depth() > 0, 1);

        mr_interp_rollforward(); mr_interp_update(T); snap();
        CHECK("forward returns to the line", strcmp(g_cur, at) == 0, 1);
        CHECK("  and the variables with it",
              mr_var_get_int("rfprobe", -1), probe_at);
        CHECK("  the step is spent", mr_rollforward_depth(), 0);

        /* Back, then *onward* by a normal advance: the future is gone. A
         * forward step that survived this would replay a branch the player
         * has left, restoring variables from a story that no longer
         * happened. */
        /* Several steps, not one.
         *
         * This is the check that was missing, and the bug it would have
         * caught shipped past two tests: rollforward moved exactly one line
         * and then stopped dead, because the step advanced the ring head as
         * well as restoring the point, so the resumed statement's checkpoint
         * landed in the *next* forward point's slot and wiped it. Both the
         * earlier tests here and in rbmusictest happened to take exactly one
         * forward step, so neither could see it.
         *
         * Four lines are remembered on the way back and each one is required
         * on the way forward, in order. Counting steps is not enough -- the
         * depth could tick down correctly while landing on the wrong lines. */
        {
            char seen[4][200];
            int n, i, ok = 1;

            for (n = 0; n < 4; n++) {
                mr_interp_update(T); snap();
                strcpy(seen[n], g_cur);
                mr_interp_rollback(); mr_interp_update(T);
            }
            printf("   walked back over 4 lines, fwd=%d\n",
                   mr_rollforward_depth());
            CHECK("four rollbacks give four forward steps",
                  mr_rollforward_depth(), 4);

            for (i = 3; i >= 0; i--) {
                mr_interp_rollforward(); mr_interp_update(T); snap();
                printf("   forward -> %.40s\n", g_cur);
                if (strcmp(g_cur, seen[i])) {
                    printf("     (wanted %.40s)\n", seen[i]);
                    ok = 0;
                }
            }
            CHECK("  and each one lands on the line it came from", ok, 1);
            CHECK("  all four are spent", mr_rollforward_depth(), 0);
        }

        /* Advancing along the same path is *not* a branch.
         *
         * Three steps back, then one forward by pressing Space rather than
         * PageDown: that re-runs the same statement the forward log is built
         * on, so two steps must remain. This is the behaviour, not a
         * concession -- retracing your own path should not throw the future
         * away, and Ren'Py keeps it too. An earlier version of this test
         * asserted 0 here and was simply wrong about what it was watching. */
        mr_interp_rollback(); mr_interp_update(T);
        mr_interp_rollback(); mr_interp_update(T);
        mr_interp_rollback(); mr_interp_update(T);
        CHECK("three steps back gives several", mr_rollforward_depth() > 1, 1);
        {
            long was = mr_rollforward_depth();
            next_line();
            mr_interp_update(T);
            CHECK("  advancing along the same path keeps the rest",
                  mr_rollforward_depth(), was - 1);
        }

        /* Going somewhere else *is* a branch. The future those steps
         * described no longer happened, and replaying it would drop the
         * player into an abandoned line with its variables.
         *
         * Asserted on where the player ends up, not on the counter. The
         * counter decays to zero on its own as new checkpoints are taken, so
         * a mutant that cleared nothing still read 0 by the time it was
         * looked at while remaining perfectly capable of jumping into the
         * old branch one keypress earlier. What matters is that PageDown
         * does not move you. */
        /* Any label but this one, derived: `Jump("chap2")` was a name out
         * of one game, and a jump to a label that does not exist moves
         * nobody -- so the two checks below reported that the future had
         * survived a branch that never happened. */
        {
            char elsewhere[MR_MAX_NAME], act[MR_MAX_NAME + 16];

            if (anygame_other_label(s, anygame_label,
                                    elsewhere, sizeof(elsewhere))) {
                sprintf(act, "Jump(\"%s\")", elsewhere);
                mr_interp_action(act);
            } else {
                printf("   (only one story label; no branch to make)\n");
            }
        }
        mr_interp_update(T);
        /* Not checked immediately: the future is discarded by the *next
         * checkpoint*, not by the jump itself, so for the instant between a
         * jump and the statement it lands on the count is still stale. That
         * window is inside one update -- the engine is executing, not waiting
         * for a key -- so a player cannot press PageDown in it. Asserting 0
         * here fails against correct code. */
        next_line();
        mr_interp_update(T); snap(); strcpy(back_to, g_cur);
        printf("   after jumping away: %s (fwd=%d)\n",
               back_to, mr_rollforward_depth());

        mr_interp_rollforward(); mr_interp_update(T); snap();
        CHECK("  jumping elsewhere kills the future",
              strcmp(g_cur, back_to) == 0, 1);
        CHECK("  and it did not land back in the old branch",
              strcmp(g_cur, at) != 0, 1);
    }

    /* --- a save taken while rolled back ---------------------------------
     *
     * Reported from play: play four lines, roll back to the second, save
     * there, load. The line is right, advancing works, but the lines ahead
     * are not reachable by rollforward -- the file carries no future.
     *
     * A divergence from Ren'Py, which pops the rolled-over entries into
     * `RollbackLog.forward` (rollback.py:1009) and pickles the whole log
     * (loadsave.py:184). Accepted rather than fixed, on one condition: that
     * it is the behaviour *every* time, since a player can advance or skip
     * through it but cannot cope with a future that sometimes replays.
     *
     * So this pins it. It is a test of a known limitation, not of a
     * correctness property -- if it ever goes red because a future survived,
     * that is the limitation being lifted and the thing to check is that it
     * is lifted *completely*, not that this assertion was wrong.
     */
    {
        char at_rb[200];
        int i;

        printf("\n-- a save taken while rolled back --\n");

        for (i = 0; i < 4; i++)
            next_line();
        for (i = 0; i < 2; i++) {
            mr_interp_rollback();
            mr_interp_update(T);
        }
        mr_interp_update(T); snap(); strcpy(at_rb, g_cur);
        printf("   rolled back to: %.40s (fwd=%d)\n", at_rb,
               mr_rollforward_depth());
        CHECK("a future exists before the save",
              mr_rollforward_depth() > 0, 1);

        CHECK("  save from there", mr_save_write("3-2", g_st), 1);
        CHECK("  load it back", mr_save_load("3-2"), 1);
        mr_interp_update(T); snap();

        CHECK("  the line is the one saved at", strcmp(g_cur, at_rb) == 0, 1);
        CHECK("  and the future is gone, every time",
              mr_rollforward_depth(), 0);
        mr_interp_rollforward(); mr_interp_update(T); snap();
        CHECK("  so PageDown does not move",
              strcmp(g_cur, at_rb) == 0, 1);

        /* And the part that does work, which is what makes it liveable: the
         * player advances instead. */
        next_line();
        mr_interp_update(T); snap();
        CHECK("  but advancing moves on", strcmp(g_cur, at_rb) != 0, 1);
        mr_save_delete("3-2");
    }

    mr_save_delete("3-1");

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}

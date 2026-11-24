/* Re-deciding a choice, and what that must do to the forward log.
 *
 * The question this answers: roll back past a menu, pick the *other* option,
 * and press Page Down. Is the future invalidated?
 *
 * The forward log is cut when a checkpoint lands on a point we rolled back
 * over and the statement is not the same one -- compared on pc. For a menu
 * whose branches go to different places that is enough. But a visual novel
 * menu almost always **converges**: branch for a line or two, set a flag,
 * then rejoin. The fixture's menu is exactly that shape, both options ending
 * in `jump chapter_two`, one of them setting `visited` on the way.
 *
 * After converging, the next say statement has the *same pc* on either
 * branch. So the pc test says "same statement, keep the future" when the
 * player has in fact chosen differently, and rolling forward would re-apply
 * variable values captured during the branch they abandoned.
 *
 * Runs on tools/harness/story, which has the converging menu and the flag.
 */
#include <stdio.h>
#include <string.h>

#include "mr.h"
#include "history.h"
#include "save.h"
#include "expr.h"
#include "interp.h"
#include "vfs.h"
#include "script.h"
#include "image.h"
#include "scene.h"
#include "ui.h"
#include "audio.h"

static mr_surface *g_st;
mr_surface *mr_stage(void) { return g_st; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); }
void mr_log(const char *f, ...) { (void)f; }
void mr_quit(void) {}
void mr_invalidate(void) {}
void hostshim_wave_sink(const short *d, int n) { (void)d; (void)n; }

static long T;
static int  fails;
static char cur[400];

static void check(const char *label, int ok)
{
    printf("  %-56s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

static void step(void) { mr_interp_update(T); T += 33; }

static const char *line(void)
{
    const char *who, *what; mr_u32 c; int shown;
    if (mr_interp_dialogue(&who, &what, &c, &shown))
        sprintf(cur, "%.200s", what);
    else
        strcpy(cur, "(no text)");
    return cur;
}

static void advance(void) { mr_interp_advance(); step(); }

static int choice_up(void)
{
    const char *t[8];
    int n = 0;
    return mr_interp_menu(&n, t, 8) && n > 0;
}

/* Walk forward until the choice menu is on screen. */
static int to_the_menu(void)
{
    int i;
    for (i = 0; i < 300; i++) {
        step();
        if (choice_up())
            return 1;
        mr_interp_advance();
    }
    return 0;
}

int main(int argc, char **argv)
{
    mr_script *s;
    int i;
    long after_first;

    setbuf(stdout, NULL);
    if (argc < 2) {
        fprintf(stderr, "usage: branchtest <story-dir>\n");
        return 2;
    }

    mr_vfs_init(NULL, argv[1]);
    mr_image_init(argv[1]);
    mr_scene_init();
    s = mr_script_load(argv[1]);
    mr_interp_init(s, argv[1]);
    mr_ui_init(argv[1]);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);
    mr_interp_start("start");

    printf("\n-- take one branch --\n");
    check("the choice menu came up", to_the_menu());

    /* Option 1 *first*, deliberately: it is the branch that writes the flag.
     *
     * The order matters and the first version of this test had it the other
     * way round, so it proved nothing. The forward log stores the values of
     * variables *written between* the two checkpoints -- take the branch
     * that writes nothing and there is no delta to restore, and rolling
     * forward cannot clobber anything whatever the logic does. The hazard is
     * abandoning the branch that *did* write. */
    mr_interp_choose(1);                 /* "...but set a flag" */
    step();
    for (i = 0; i < 6; i++)
        advance();
    after_first = mr_var_get_int("visited", -1);
    printf("   option 0 -> visited=%ld, on %.34s\n", after_first, line());
    check("this branch set the flag", after_first == 1);

    printf("\n-- roll back past the menu and take the other --\n");

    /* Rolling back lands on say statements; the menu is not one, so it does
     * not come back by rolling onto it. Go back behind it and walk forward
     * until it is offered again, which is what a player does. */
    for (i = 0; i < 9; i++) {
        mr_interp_rollback();
        step();
    }
    printf("   9 rollbacks -> %d forward steps, on %.30s\n",
           mr_rollforward_depth(), line());
    for (i = 0; i < 40 && !choice_up(); i++)
        advance();
    printf("   advanced %d to reach the menu again, %d forward steps\n",
           i, mr_rollforward_depth());
    check("the menu is back", choice_up());

    mr_interp_choose(0);                 /* "Carry on": leaves it alone */
    step();
    for (i = 0; i < 3; i++)
        advance();
    printf("   re-decided to option 0 -> visited=%ld, %d forward steps\n",
           mr_var_get_int("visited", -1), mr_rollforward_depth());
    check("re-deciding left the flag as the new branch has it",
          mr_var_get_int("visited", -1) == 0);

    /* The question. Whatever the forward log decided to keep, rolling
     * forward must not undo the choice the player just made.
     *
     * Asserted on the flag rather than on the step count, because the count
     * is allowed to be non-zero: the branches converge, so the lines ahead
     * really are the same lines, and continuing to offer them is not wrong
     * by itself. What would be wrong is arriving there with the abandoned
     * branch's variables. */
    printf("\n-- now roll forward --\n");
    for (i = 0; i < 6; i++) {
        mr_interp_rollforward();
        step();
        printf("   forward %d: visited=%ld, on %.34s\n",
               i + 1, mr_var_get_int("visited", -1), line());
    }
    check("rolling forward did not resurrect the abandoned branch's flag",
          mr_var_get_int("visited", -1) == 0);

    /* ---------------------------------------------------------------------
     * A menu is a checkpoint, in both directions.
     *
     * It was not, and so the log could not see a menu at all: rolling back
     * crossed it without stopping and the forward log had nothing to stop at
     * either, so rolling forward jumped over the menu and silently restored
     * whichever branch had been taken before. Reported from play as
     * "rollback/rollforward entirely skips choices".
     *
     * Ren'Py's display_menu ends with `renpy.exports.checkpoint(rv)`
     * (menuexports.py:432) -- a checkpoint carrying the option taken, which
     * is what buys both halves at once.
     */
    printf("\n-- a menu is a checkpoint in both directions --\n");

    mr_interp_start("start");
    mr_rollback_reset();
    mr_history_reset();
    mr_expr_exec("visited = 0");
    check("the menu came up again", to_the_menu());

    /* The history as it stands *at* the menu, measured rather than predicted:
     * what follows this menu in the fixture is a title card, not more
     * dialogue, so counting advances would say nothing. Rolling back here
     * again must restore exactly this. */
    {
        int hist_at_menu;
        int hist_first = mr_history_count();

        mr_interp_choose(1);             /* the branch that writes the flag */
        step();
        for (i = 0; i < 2; i++)
            advance();
        check("past it, with the flag set",
              mr_var_get_int("visited", -1) == 1);

        /* --- rolling back has to *stop* at the menu --- */
        for (i = 0; i < 8 && !choice_up(); i++) {
            mr_interp_rollback();
            step();
        }
        printf("   %d rollbacks to get back to the menu, visited=%ld\n",
               i, mr_var_get_int("visited", -1));
        check("rolling back alone reaches the menu -- no advancing",
              choice_up());
        check("  and the flag is back to its pre-menu value",
              mr_var_get_int("visited", -1) == 0);

        /* The history entries go with the lines, and a menu has none of its
         * own: rolling back pops one per *say* checkpoint dropped, not one per
         * checkpoint. Popping per checkpoint ate a real line of dialogue out
         * of the history screen for every menu crossed, and being back at the
         * same menu is the one place that is exactly measurable. */
        hist_at_menu = mr_history_count();
        printf("   history: %d at the menu, %d back at it again\n",
               hist_first, hist_at_menu);
        check("  the history is exactly what it was at this menu",
              hist_at_menu == hist_first);

        /* --- and one more press must leave it ---
         *
         * The reported regression: with the menu a checkpoint but the caller
         * still claiming the newest point was not on screen, rolling back at
         * a menu landed on the menu it was already showing. The first Page Up
         * did nothing and the player had to press again. */
        mr_interp_rollback();
        step();
        printf("   one more rollback -> %.40s\n", line());
        check("one rollback at the menu actually leaves it", !choice_up());

        /* --- rolling forward answers it from the record --- */
        check("there is a future to return to", mr_rollforward_depth() > 0);
        for (i = 0; i < 8 && !choice_up(); i++) {
            mr_interp_rollforward();
            step();
        }
        check("rolling forward comes back to the menu", choice_up());
        check("  without having re-run the branch",
              mr_var_get_int("visited", -1) == 0);

        mr_interp_rollforward();
        step();
        printf("   answered by rollforward -> visited=%ld, on %.34s\n",
               mr_var_get_int("visited", -1), line());
        check("a press at the menu answers it", !choice_up());
        /* The whole point: the option recorded, not option 0, and not a dead
         * end. This is what "rollforward entirely skips choices" cost. */
        check("  with the option that was taken before",
              mr_var_get_int("visited", -1) == 1);
    }

    /* ---------------------------------------------------------------------
     * And all of that again, across a save.
     *
     * A checkpoint knows what kind of statement it was taken at, and that is
     * what decides how many history lines a rollback pops: one per *say*
     * dropped, not one per checkpoint. The kind was not in the save file, so
     * every checkpoint read back from a slot claimed to be a say -- and
     * rolling back over a menu in a loaded game ate a real line of dialogue
     * out of the history screen, exactly the bug the in-memory check above
     * was written for. Found by reading a save rather than by playing, which
     * is why it lasted: the live path and the loaded path disagreed, and only
     * the live one was tested.
     *
     * Measured the same way as above -- the history at the menu, and the
     * history back at that same menu -- because that is the one point where
     * the number is exactly predictable. The menu is deliberately the
     * *uncaptioned* kind: a captioned menu does write a history line, so it
     * pops one correctly whether or not the kind survived, and a fixture
     * built on one would pass with the bug in place.
     */
    printf("\n-- the same thing across a save --\n");
    {
        int hist_at_menu, hist_after_load;

        mr_interp_start("start");
        mr_rollback_reset();
        mr_history_reset();
        mr_expr_exec("visited = 0");
        check("the menu came up once more", to_the_menu());

        hist_at_menu = mr_history_count();
        mr_interp_choose(1);
        step();
        for (i = 0; i < 2; i++)
            advance();
        check("past it again, flag set", mr_var_get_int("visited", -1) == 1);

        check("wrote the save", mr_save_write("4-1", g_st) == 1);

        /* Destroy everything the slot is supposed to restore, so that what
         * is measured afterwards came out of the file. */
        mr_history_reset();
        mr_rollback_reset();
        mr_expr_exec("visited = 0");
        mr_interp_start("start");
        step();

        check("loaded it back", mr_save_load("4-1") == 1);
        step();
        check("  the flag came back with it",
              mr_var_get_int("visited", -1) == 1);
        check("  and so did the rollback log", mr_rollback_depth() > 0);

        for (i = 0; i < 8 && !choice_up(); i++) {
            mr_interp_rollback();
            step();
        }
        check("rolling back in the loaded game reaches the menu", choice_up());
        check("  with the flag back to its pre-menu value",
              mr_var_get_int("visited", -1) == 0);

        hist_after_load = mr_history_count();
        printf("   history: %d at the menu, %d back at it after a load\n",
               hist_at_menu, hist_after_load);
        check("  and the history is what it was at this menu",
              hist_after_load == hist_at_menu);

        mr_save_delete("4-1");
    }

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}

/* Features in combination, which is where this engine's bugs actually live.
 *
 * Every harness here tests one thing well. Three bugs in one week sat in the
 * seam between two of them, and each was found by a player rather than by
 * the suite:
 *
 *   - the game menu came back on a rollback, but only after a language
 *     switch. langtest tested switching; rbtest tested rolling back; the
 *     checkpoint taken by the switch *while the menu was open* belonged to
 *     neither test.
 *   - rollforward moved one line and stopped. Both tests written for it took
 *     exactly one step.
 *   - the rhythm game's quit button was swallowed by the give-up dialog,
 *     because the dialog claims every key.
 *
 * So this file deliberately tests nothing on its own. Every block is two or
 * more features crossed, driven in the order a player would, and the thing
 * asserted is the invariant that survives the crossing.
 *
 * The one invariant worth stating up front, because most of the blocks come
 * back to it: **an excursion into the interface must leave no trace in the
 * story.** Opening a menu, saving, switching language and coming back out
 * must not change what rolling back or forward does, and must never put an
 * interface screen into the story's scene.
 *
 * Needs the pak: the menu screens and the translations are in it.
 */
#include <stdio.h>
#include <string.h>

#include "mr.h"
#include "save.h"
#include "history.h"
#include "expr.h"
#include "interp.h"
#include "vfs.h"
#include "script.h"
#include "anygame.h"
#include "image.h"
#include "scene.h"
#include "ui.h"
#include "tl.h"
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

static void step(void)
{
    mr_interp_update(T);
    T += 33;
}

static const char *line(void)
{
    const char *who, *what;
    mr_u32 c;
    int shown;
    if (mr_interp_dialogue(&who, &what, &c, &shown))
        sprintf(cur, "%.200s", what);
    else
        strcpy(cur, "(no text)");
    return cur;
}

static void advance(void)
{
    mr_interp_advance();
    step();
}

/* Is an *interface* screen on the scene?
 *
 * Asked by name, not by count. The quick menu is a screen as well, so the
 * count is 1 with nothing open and 1 again with the game menu up -- the
 * first version of the language test counted, read 1 both times, and passed
 * while the menu was on screen.
 *
 * Asked of the scene rather than of the pixels because the bug that prompted
 * this put the menu *under* the dialogue box: a check on the visible text
 * would have seen the right line and been satisfied.
 */
static int menu_up(void)
{
    static const char *const ui[] = {
        "game_menu", "navigation", "preferences", "save", "load",
        "about", "help", "history", "confirm", NULL
    };
    int i, k;

    for (i = 0; i < mr_scene_screen_count(); i++) {
        const char *n = mr_scene_screen_name(i);
        if (!n)
            continue;
        for (k = 0; ui[k]; k++)
            if (!strcmp(n, ui[k]))
                return 1;
    }
    return 0;
}

static const char *screens(void)
{
    static char out[220];
    int i;
    out[0] = '\0';
    for (i = 0; i < mr_scene_screen_count(); i++) {
        const char *n = mr_scene_screen_name(i);
        if (strlen(out) + strlen(n ? n : "?") + 2 >= sizeof(out))
            break;
        if (i) strcat(out, " ");
        strcat(out, n ? n : "?");
    }
    return out[0] ? out : "(none)";
}

/* Walk to a line of dialogue, so a block starts somewhere definite. */
static void to_dialogue(void)
{
    int i;
    for (i = 0; i < 60; i++) {
        step();
        if (strcmp(line(), "(no text)"))
            return;
        mr_interp_advance();
    }
}

/* --- the blocks ---------------------------------------------------------- */

/* A menu excursion, then rolling back over the line it happened on. The
 * reported bug needed a language switch to show up, because that is the one
 * action that takes a checkpoint with the menu open -- but the invariant is
 * broader than that one action, so it is asserted broadly. */
static void menu_then_rollback(void)
{
    char before[400];

    printf("\n-- open the menu, come back, roll back --\n");
    to_dialogue();
    strcpy(before, line());
    check("the story is up with no interface over it", !menu_up());

    mr_interp_game_menu_toggle();
    step();
    printf("   with the menu open: %s\n", screens());
    check("the menu opened", menu_up());

    mr_interp_game_menu_toggle();
    step();
    check("  and closed", !menu_up());
    check("  leaving the same line", !strcmp(line(), before));

    advance();
    mr_interp_rollback();
    step();
    printf("   after rolling back: %s\n", screens());
    check("rolling back brings no interface screen", !menu_up());
    check("  and lands on the line it came from", !strcmp(line(), before));
}

/* The reported sequence, in full: the switch is what takes a checkpoint with
 * the menu up. Two rollbacks, not one -- the first version of the fix could
 * have cleaned only the newest checkpoint. */
static void language_inside_the_menu(void)
{
    printf("\n-- switch language from inside the menu, then roll back --\n");
    to_dialogue();

    mr_interp_game_menu_toggle();
    step();
    check("in the menu", menu_up());

    mr_interp_action("Language(\"english\")");
    step();
    printf("   switched, screens: %s\n", screens());

    mr_interp_game_menu_toggle();
    step();
    check("  back out of it", !menu_up());

    advance();
    advance();

    mr_interp_rollback();
    step();
    printf("   back once:  %s | %.40s\n", screens(), line());
    check("one step back is clean", !menu_up());

    mr_interp_rollback();
    step();
    printf("   back twice: %s | %.40s\n", screens(), line());
    check("  and so is two", !menu_up());

    mr_interp_action("Language(None)");
    step();
}

/* Saving is also done from inside the menu, and a save writes a checkpoint's
 * worth of state. Crossing it with rollback is the same shape as the bug. */
static void save_from_the_menu(void)
{
    printf("\n-- save from the menu, then roll back --\n");
    to_dialogue();

    mr_interp_game_menu_toggle();
    step();
    mr_save_screenshot(g_st);
    check("save written from inside the menu",
          mr_save_write("8-1", g_st) == 1);
    mr_interp_game_menu_toggle();
    step();

    advance();
    mr_interp_rollback();
    step();
    printf("   after rolling back: %s\n", screens());
    check("rolling back brings no interface screen", !menu_up());

    /* And loading it back must not restore one either -- same filter, the
     * other side of it. */
    check("save loads", mr_save_load("8-1") == 1);
    step();
    printf("   after loading:      %s\n", screens());
    check("loading brings no interface screen", !menu_up());
    mr_save_delete("8-1");
}

/* Rollforward crossed with a menu excursion. The forward log survives an
 * excursion -- the player did not go anywhere in the story -- but must not
 * carry the menu back with it. */
static void rollforward_across_the_menu(void)
{
    char at[400];
    int fwd, i;

    printf("\n-- roll back, visit the menu, roll forward --\n");
    to_dialogue();

    /* Build a log first, and say so.
     *
     * The block before this one ends with Language(None), which discards the
     * rollback log -- so this started with one step available, rolled back
     * twice, and the second did nothing. The test read that as "rollforward
     * is broken" when it was the setup that was wrong. A precondition is
     * cheaper than re-deriving that twice. */
    for (i = 0; i < 12 && mr_rollback_depth() < 3; i++)
        advance();
    check("there is a log to work with", mr_rollback_depth() >= 3);
    strcpy(at, line());

    mr_interp_rollback();
    step();
    mr_interp_rollback();
    step();
    fwd = mr_rollforward_depth();
    printf("   two back: depth=%d, %d forward steps\n",
           mr_rollback_depth(), fwd);
    check("two rollbacks give two forward steps", fwd == 2);

    mr_interp_game_menu_toggle();
    step();
    mr_interp_game_menu_toggle();
    step();
    check("an excursion keeps them", mr_rollforward_depth() == fwd);

    mr_interp_rollforward();
    step();
    check("  forward brings no interface screen", !menu_up());
    mr_interp_rollforward();
    step();
    printf("   forward twice: %s | %.40s\n", screens(), line());
    check("  nor does the second step", !menu_up());
    check("  and it is back where it started", !strcmp(line(), at));
}

/* A language switch discards the rollback log. The forward steps are part of
 * that log and have to go with it, or PageDown would walk into checkpoints
 * describing text in the other language. */
static void language_clears_the_future(void)
{
    printf("\n-- a language switch discards the forward steps too --\n");
    to_dialogue();
    advance();
    advance();

    mr_interp_rollback();
    step();
    check("a forward step exists", mr_rollforward_depth() > 0);

    mr_interp_action("Language(\"english\")");
    step();
    check("  the switch takes it away", mr_rollforward_depth() == 0);

    {
        char here[400];
        strcpy(here, line());
        mr_interp_rollforward();
        step();
        check("  and rolling forward does nothing", !strcmp(line(), here));
    }

    /* And asked of the log directly, because the route above does not
     * discriminate: Language takes a fresh checkpoint right after the reset,
     * and that checkpoint clears the count incidentally -- so a
     * mr_rollback_reset that forgot the forward log still reads 0 here. The
     * real hazard is a reset with *no* checkpoint behind it, which is
     * renpy.block_rollback() and loading a save; there, a stale count would
     * let rollforward walk into points belonging to a discarded log. */
    advance();
    advance();
    mr_interp_rollback();
    step();
    check("  (a live forward step to clear)", mr_rollforward_depth() > 0);
    mr_rollback_reset();
    check("  and the reset clears it on its own, with no checkpoint",
          mr_rollforward_depth() == 0);

    mr_interp_action("Language(None)");
    step();
}


int main(int argc, char **argv)
{
    mr_script *s;

    setbuf(stdout, NULL);
    if (argc < 3) {
        fprintf(stderr, "usage: seamtest <pak> <game-dir>\n");
        return 2;
    }

    mr_vfs_init(argv[1], argv[2]);
    mr_image_init(argv[2]);
    mr_scene_init();
    s = mr_script_load(argv[2]);
    mr_interp_init(s, argv[2]);
    mr_ui_init(argv[2]);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);
    /* Not "chap12", which is Miwa's. Hinata happens to have one too, so
     * this test passed against both and that proved nothing -- the two
     * games share an author and a template. See anygame.h. */
    {
        char label[MR_MAX_NAME];

        if (!anygame_story_label(s, label, sizeof(label))) {
            /* **A skip, not a failure.** This is "could not reach the thing I
             * test", and the thing tested is the engine's menu/rollback seam,
             * not any game's script. It exited 1 here until 2026-11-06 and
             * went red the moment mrpy.pak was rebuilt for a game whose
             * script anygame.h finds no story label in -- a red suite that
             * said nothing about the engine and cost a round of looking for a
             * regression that was not there. See the harness README and
             * NOTES.md.
             *
             * 77 is run.sh's skip convention, with the reason printed, which
             * is what the five pak-driven harnesses beside this one already
             * do. */
            fprintf(stderr,
                    "seamtest: no story label in this pak's script, so the "
                    "menu seam cannot be\n"
                    "          driven. Skipping -- this is about the pak, "
                    "not the engine. Try\n"
                    "          MR_PAK=<a pak whose script has one>.\n");
            return 77;
        }
        printf("start label: %s\n", label);
        mr_interp_start(label);
    }

    menu_then_rollback();
    language_inside_the_menu();
    save_from_the_menu();
    rollforward_across_the_menu();
    language_clears_the_future();

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}

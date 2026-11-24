/* Saving at a choice menu, and loading the slot again.
 *
 * Reported from play: a save taken on a menu has the right thumbnail but
 * comes back with no menu on screen. The player has to interact with the
 * window -- a click, space, the wheel, PageUp -- before the options appear.
 *
 * The cause is one line of mr_interp_restore. A menu's saved pc is the `menu`
 * statement itself (exec_one leaves it there while it waits, which is what
 * menu_checkpoint's comment relies on), so resuming at it re-executes the
 * menu and offers it again. But the restore decided the run state from
 * g_has_dialogue alone, and at a menu there is almost always a line on
 * screen: either the menu's own caption, or the line before it that was never
 * dismissed. So the load came back as ST_SAY -- a line waiting to be
 * advanced -- and the menu statement was not reached until the player
 * supplied that advance. Hence "interact with the window and it appears".
 *
 * In-session rollback to a menu never had this: apply_checkpoint resumes at
 * ST_RUN unconditionally. The file path was the one that diverged.
 *
 * The three menus differ only in what dialogue is up when they open, because
 * that is the whole of the bug:
 *
 *   - a cold menu, nothing said yet         -- worked before, guard against
 *                                              a fix that breaks it
 *   - a bare menu under an undismissed line -- broken
 *   - a menu with its own caption           -- broken; the reported shape
 *
 * Each assertion is made with *no input at all* after the load: one
 * mr_interp_update and then the question. A test that advanced first would
 * pass against the bug, since an advance is precisely what used to fix it.
 *
 * Runs on tools/harness/story, label save_menu.
 */
#include <stdio.h>
#include <string.h>

#include "mr.h"
#include "script.h"
#include "scene.h"
#include "image.h"
#include "interp.h"
#include "save.h"
#include "history.h"
#include "expr.h"
#include "vfs.h"
#include "ui.h"

static mr_surface *g_st;
mr_surface *mr_stage(void) { return g_st; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); }
void mr_log(const char *f, ...) { (void)f; }
void mr_quit(void) {}
void mr_invalidate(void) {}
void hostshim_wave_sink(const short *d, int n) { (void)d; (void)n; }

static long T = 100000;
static int fails;

static void checkb(const char *label, int got, int want)
{
    int ok = (got == want);
    printf("%-54s %6d want %6d  %s\n", label, got, want,
           ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

static void checks(const char *label, const char *got, const char *want)
{
    int ok = !strcmp(got, want);
    printf("%-54s %s\n   got  '%s'\n   want '%s'\n", label,
           ok ? "ok" : "** FAIL **", got, want);
    if (!ok)
        fails++;
}

static const char *line_now(void)
{
    const char *who, *what;
    mr_u32 c;
    int shown;

    if (mr_interp_dialogue(&who, &what, &c, &shown))
        return what;
    return "(none)";
}

/* The first option on offer, or "(no menu)". This is what the player either
 * does or does not see. */
static const char *first_option(void)
{
    static const char *texts[16];
    int n = 0;

    if (!mr_interp_menu(&n, texts, 16) || n <= 0)
        return "(no menu)";
    return texts[0];
}

static int option_count(void)
{
    const char *texts[16];
    int n = 0;

    if (!mr_interp_menu(&n, texts, 16))
        return 0;
    return n;
}

/* Run forward until a menu is being offered.
 *
 * The question is asked before each advance rather than after: a menu under a
 * line that is still up does not change the displayed text when it opens, so
 * "wait for the line to change" would walk straight past it and then sit
 * there advancing at a menu, where an advance does nothing. */
static int run_to_menu(void)
{
    int guard = 0;

    while (guard++ < 200) {
        mr_interp_update(T);
        T += 33;
        if (option_count() > 0)
            return 1;
        mr_interp_advance();
    }
    return 0;
}

/* Finish whatever caption is typing, so the save is taken from a settled
 * menu rather than from a half-typed one -- the reported case is a player
 * sitting on a menu, not one saving mid-glyph. */
static void settle(int ms)
{
    int i;

    for (i = 0; i < ms; i += 33) {
        T += 33;
        mr_interp_update(T);
        mr_scene_render(g_st, T);
    }
}

/* Save, go elsewhere, let a minute pass, load it back -- and then do nothing
 * at all. No advance, no click: whatever is on screen after this is what the
 * player is looking at when the slot comes up. */
static void round_trip(void)
{
    mr_interp_update(T);
    mr_save_write("1-5", g_st);

    mr_interp_start("start");
    settle(300);

    T += 60000;
    mr_interp_update(T);

    mr_save_load("1-5");
    mr_interp_update(T);
}

/* Each of the three menus, by the text of its first option, and the value its
 * first option sets. */
static void one_menu(const char *what, const char *opt0, int picked)
{
    printf("\n-- %s --\n", what);

    if (!run_to_menu()) {
        printf("fixture: never reached the %s\n", what);
        fails++;
        return;
    }
    settle(600);
    checks("the menu is up before the save", first_option(), opt0);

    round_trip();

    /* The bug, stated directly. */
    checks("and is up again straight after the load", first_option(), opt0);
    checkb("with both options", option_count(), 2);

    /* Not just drawn: answering it has to run that menu's own block, which is
     * what says the pc came back on the menu rather than near it. */
    mr_interp_choose(0);
    mr_interp_update(T);
    checkb("picking the first option runs its block",
           mr_var_get_int("wasadate", -99), picked);
}

/* Walk to the third menu, the captioned one, answering the two before it. */
static int to_third_menu(void)
{
    mr_interp_start("save_menu");
    if (!run_to_menu())
        return 0;
    mr_interp_choose(0);
    if (!run_to_menu())
        return 0;
    mr_interp_choose(0);
    if (!run_to_menu())
        return 0;
    settle(600);
    return 1;
}

/* One Page Up, and whatever is on screen after it. */
static void roll_back_once(void)
{
    mr_interp_rollback();
    mr_interp_update(T);
    T += 33;
    settle(600);
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "story";
    mr_script *s;

    setbuf(stdout, NULL);
    remove("SAVEP1S5.SAV");

    mr_vfs_init(NULL, dir);
    mr_image_init(dir);
    mr_scene_init();
    s = mr_script_load(dir);
    mr_interp_init(s, dir);
    mr_ui_init(dir);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);
    mr_interp_start("save_menu");

    one_menu("cold menu, no dialogue on screen", "Cold, left", 1);
    one_menu("bare menu under an undismissed line", "Warm, left", 3);
    one_menu("menu with a caption of its own", "Captioned, left", 5);

    /* The caption is the shape it was reported in, so pin that the line comes
     * back with it. Re-executing the menu re-runs its caption say, exactly as
     * a rollback to the same point does. */
    printf("\n-- the restored caption --\n");
    if (!to_third_menu()) {
        printf("fixture: never reached the third menu again\n");
        fails++;
    } else {
        checks("the caption is up before the save", line_now(),
               "A caption the menu brought with it.");
        round_trip();
        settle(600);
        checks("and the caption came back with the menu", line_now(),
               "A caption the menu brought with it.");
        checks("over the menu", first_option(), "Captioned, left");
    }

    /* Rolling back from the restored menu.
     *
     * The baseline first, in a session with no save in it, because the whole
     * assertion is that the two behave the same. One Page Up at a menu drops
     * two points -- the menu's own and the one before it -- and resumes at
     * the latter, which here is the bare menu, so the previous choice is
     * offered again. That is what the engine does without a save anywhere
     * near it, and it is the answer the load has to agree with.
     *
     * Reported from play: after loading at a menu the first Page Up showed
     * the same menu again and only the second one moved. The re-executed
     * menu's checkpoint was being appended past the one the file already
     * held, so the log had the menu in it twice and the first step back
     * landed on the copy. */
    printf("\n-- rolling back from the menu, no save involved --\n");
    if (!to_third_menu()) {
        printf("fixture: never reached the third menu\n");
        fails++;
    } else {
        roll_back_once();
        checks("one step back offers the previous menu", first_option(),
               "Warm, left");
    }

    printf("\n-- and after a load, which must be the same --\n");
    if (!to_third_menu()) {
        printf("fixture: never reached the third menu\n");
        fails++;
    } else {
        round_trip();
        settle(600);
        checks("the menu is up after the load", first_option(),
               "Captioned, left");

        roll_back_once();
        checks("one step back offers the previous menu", first_option(),
               "Warm, left");
    }

    remove("SAVEP1S5.SAV");
    printf("\n%s\n", fails ? "** FAILURES **" : "all ok");
    return fails ? 1 : 0;
}

/* Clicks a save slot the way the window procedure does: render the screen
 * with capture on, hit-test a point inside slot 5, then run whatever action
 * that focus entry holds -- including restoring its captured scope, which is
 * where `$ slot = i + 1` lives. */
#include <stdio.h>
#include <string.h>
#include "mr.h"
#include "save.h"
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
void mr_log(const char *f, ...) { (void)f; }
void mr_quit(void) {}
void mr_invalidate(void) {}
void hostshim_wave_sink(const short *d, int n) { (void)d; (void)n; }

/* This harness printed its results and returned 0, so run.sh called it a
 * pass whatever happened -- and what it prints is the one thing the click
 * path can get wrong: the slot number comes out of the captured scope.
 * See NOTES, "A test that cannot go red". */
static int fails;

static void check(const char *l, long got, long want)
{
    printf("%-44s %-6ld %s\n", l, got, got == want ? "ok" : "** WANT DIFFERENT **");
    if (got != want)
        fails++;
}

int main(int argc, char **argv)
{
    mr_script *s;
    long t = 0;
    int i, id;
    const char *act;

    setbuf(stdout, NULL);
    mr_vfs_init(NULL, argv[1]);
    mr_image_init(argv[1]);
    mr_scene_init();
    s = mr_script_load(argv[1]);
    mr_interp_init(s, argv[1]);
    anygame_start_story(s);
    mr_ui_init(argv[1]);

    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);
    mr_surface_fill(g_st, MR_RGB(10, 200, 30));

    for (i = 0; i < 6; i++) { mr_interp_update(t); t += 33; mr_interp_advance(); }
    mr_interp_update(t);

    mr_interp_show_screen("save");
    mr_interp_update(t);

    /* Render with capture on, as an interactive frame does. */
    mr_sl_focus_reset();
    mr_interp_render(g_st, t);

    /* Slot 5 sits in the middle of the second row: 672+138, 415+103. */
    id = mr_sl_hit(672 + 138, 415 + 103);
    act = mr_sl_focus_action(id);
    printf("hit id=%d action='%s'\n", id, act ? act : "(none)");
    check("the point hits a focus entry", id >= 0, 1);
    /* `FileAction(slot)` is what the template's file_slots screen puts on
     * a slot button -- the name comes from the screen, so this is read
     * off the real thing rather than guessed at. */
    check("  and it carries the slot's file action",
          act && strstr(act, "FileAction") != NULL, 1);

    if (act) {
        mr_sl_focus_scope(id);
        mr_interp_action(act);
        mr_sl_focus_scope_end();
    }

    /* Slot *5*, which is the whole point: the number comes from `$ slot =
     * i + 1` in the captured scope, so a click that ran the action
     * without restoring its scope wrote some other slot. */
    printf("name='%s' time='%s'\n",
           mr_save_name("1-5") ? mr_save_name("1-5") : "(none)",
           mr_save_time("1-5") ? mr_save_time("1-5") : "(none)");
    check("slot 1-5 was written", mr_save_exists("1-5"), 1);
    check("  and slot 1-1 was not", mr_save_exists("1-1"), 0);
    check("  the slot has a timestamp", mr_save_time("1-5") != NULL, 1);

    mr_save_delete("1-5");

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}

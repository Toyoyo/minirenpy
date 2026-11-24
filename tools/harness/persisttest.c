/* Persistent data: it has to outlive the process, survive a rollback, survive
 * loading an old save, and not be carried *by* a save. Those last two are the
 * ones that read fine and behave wrongly.
 *
 * Two runs in one process: the second calls mr_persist_load() again, which is
 * what a restart does.
 */
#include <stdio.h>
#include <string.h>
#include "mr.h"
#include "save.h"
#include "history.h"
#include "expr.h"
#include "interp.h"
#include "persist.h"
#include "vfs.h"
#include "script.h"
#include "image.h"
#include "scene.h"
#include "anygame.h"

static mr_surface *g_st;
mr_surface *mr_stage(void) { return g_st; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); }
void mr_log(const char *f, ...) { (void)f; }
void mr_quit(void) {}
void mr_invalidate(void) {}
void hostshim_wave_sink(const short *d, int n) { (void)d; (void)n; }

static long T;
static int  fails;

static void step(void) { mr_interp_update(T); T += 33; }

static void check(const char *label, int ok)
{
    printf("%-46s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

static double getf(const char *name)
{
    mr_value v;
    if (!mr_var_get(name, &v))
        return -1.0;
    if (v.type == MR_V_FLOAT) return v.f;
    if (v.type == MR_V_INT)   return (double)v.i;
    return -1.0;
}

int main(int argc, char **argv)
{
    mr_script *s;
    char label[MR_MAX_NAME];
    int i;

    setbuf(stdout, NULL);
    if (argc < 2) {
        fprintf(stderr, "usage: persisttest <game-dir>\n");
        return 2;
    }

    remove("PERSIST.SAV");

    mr_vfs_init(NULL, argv[1]);
    mr_image_init(argv[1]);
    mr_scene_init();
    s = mr_script_load(argv[1]);
    mr_interp_init(s, argv[1]);

    /* Somewhere with dialogue to step into, rather than Miwa's `chap12`.
     * That name survived here because the other game on hand has a chap12
     * too -- which proves nothing, as anygame.h's header says -- and the
     * failure when it finally met a script without one was not "no such
     * label": the interpreter sat at pc -1, the loop below found no
     * dialogue in forty steps, and the save recorded its position as
     * `?-1`. Loading then refused the slot and "save loaded" went red,
     * which reads as a save bug. */
    if (!anygame_story_label(s, label, sizeof(label))) {
        printf("no story label in %s\n", argv[1]);
        return 1;
    }
    printf("start label: %s\n", label);
    mr_interp_start(label);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);

    /* --- a fresh install --- */
    check("unset persistent flag is falsey",
          mr_var_get_int("persistent.watashi", 0) == 0);

    /* --- what the script does --- */
    mr_expr_exec("persistent.watashi = True");
    mr_interp_pref_set_fraction("music volume", 0.25);
    mr_expr_exec("_preferences.language = \"english\"");
    mr_interp_action("renpy.save_persistent()");
    check("flag set", mr_var_get_int("persistent.watashi", 0) == 1);

    /* --- a restart: forget everything, then load --- */
    mr_var_unset("persistent.watashi");
    mr_expr_exec("_preferences.volume.music = 1.0");
    mr_var_unset("_preferences.language");
    check("cleared in memory", mr_var_get_int("persistent.watashi", 0) == 0);

    mr_persist_load();
    check("flag came back", mr_var_get_int("persistent.watashi", 0) == 1);
    check("volume came back", getf("_preferences.volume.music") > 0.2 &&
                              getf("_preferences.volume.music") < 0.3);
    {
        mr_value v;
        check("language came back",
              mr_var_get("_preferences.language", &v) && v.type == MR_V_STR &&
              !strcmp(v.s, "english"));
    }

    /* --- a save must not carry it --- */
    for (i = 0; i < 40; i++) {
        const char *w, *t; mr_u32 c; int sh;
        step();
        if (mr_interp_dialogue(&w, &t, &c, &sh))
            break;
        mr_interp_advance();
    }
    mr_save_screenshot(g_st);
    check("save written", mr_save_write("9-2", g_st) == 1);

    /* Unlock more and turn the music up, as a later session would. */
    mr_expr_exec("persistent.ending = True");
    mr_interp_pref_set_fraction("music volume", 0.9);

    check("save loaded", mr_save_load("9-2") == 1);
    step();
    check("loading did not re-lock the later unlock",
          mr_var_get_int("persistent.ending", 0) == 1);
    check("loading did not restore the old volume",
          getf("_preferences.volume.music") > 0.8);
    check("and the older unlock is still there",
          mr_var_get_int("persistent.watashi", 0) == 1);
    mr_save_delete("9-2");

    /* --- rollback must not undo an unlock --- */
    {
        int depth = mr_rollback_depth();
        mr_expr_exec("persistent.gamma = True");
        mr_interp_advance();
        step();
        mr_interp_rollback();
        step();
        check("rollback left the log usable", mr_rollback_depth() <= depth + 1);
        check("rollback did not undo the unlock",
              mr_var_get_int("persistent.gamma", 0) == 1);
    }

    /* --- _clear keeps the preferences --- */
    mr_persist_clear(0);
    check("_clear forgot the flags",
          mr_var_get_int("persistent.watashi", 0) == 0 &&
          mr_var_get_int("persistent.ending", 0) == 0);
    check("_clear kept the volume", getf("_preferences.volume.music") > 0.8);

    /* --- quitting writes it --- *
     * The player's route is Quit() -> mr_quit() -> the frame loop ends ->
     * mr_interp_shutdown(), so that is what has to write the file, not the
     * explicit call the script makes. */
    mr_expr_exec("persistent.delta = True");
    remove("PERSIST.SAV");
    mr_interp_shutdown();
    {
        FILE *f = fopen("PERSIST.SAV", "rb");
        check("shutdown wrote the file", f != NULL);
        if (f) fclose(f);
    }
    mr_persist_load();
    check("and it holds what was set since the last save",
          mr_var_get_int("persistent.delta", 0) == 1);

    remove("PERSIST.SAV");
    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}

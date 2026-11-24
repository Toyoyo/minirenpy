/* Unlock flags, driven by the script.
 *
 * Reported against a real game: `$ persistent.unlock_a = True` a few lines
 * into the story unlocked one song, and the other -- gated on a flag set
 * only in the credits, which had not run -- came with it. The fixture has
 * the same shape: `$ persistent.unlock_a = True` in `label start`, and
 * unlock_b set only in a label nothing reaches.
 *
 * flagtest covers the variable store and the persistent file in isolation and
 * they are clean, so this drives the actual thing: run the script from
 * `start` until the first flag is set, then ask the three readers -- the pyfn
 * `if` in build_music_room, the one in build_rhythm_highscores, and an
 * `== True` comparison -- what they see.
 *
 * Nothing here is specific to these two names. The engine is meant for other
 * games, so what is being pinned down is that one `persistent.<name>` is
 * independent of every other.
 *
 * Run from a scratch directory: it writes and removes PERSIST.SAV.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "mr.h"
#include "vfs.h"
#include "audio.h"
#include "image.h"
#include "ui.h"
#include "expr.h"
#include "interp.h"
#include "script.h"
#include "scene.h"
#include "persist.h"
#include "list.h"


/* A game may open on a chapter-select menu -- the one this was written
 * against grew one after the fact -- and anything that wants dialogue has
 * to answer it first. The fixture has no menu, so this does nothing there;
 * it stays because a menu appearing in front of the story is the kind of
 * thing that turns this loop into a timeout. Which option hardly matters,
 * so take the first. */
static void answer_menu(void)
{
    int count = 0;
    const char *texts[16];

    if (mr_interp_menu(&count, texts, 16) && count > 0)
        mr_interp_choose(0);
}

static void sink(const short *d, int n) { (void)d; (void)n; }
void (*hostshim_wave_sink)(const short *, int) = sink;

static mr_surface *g_stage;

void mr_log(const char *fmt, ...)
{
    va_list ap;
    if (!getenv("MR_VERBOSE"))
        return;
    va_start(ap, fmt);
    printf("    log: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}
void mr_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("FATAL: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    exit(1);
}
void mr_quit(void) { }
void mr_invalidate(void) { }
mr_surface *mr_stage(void) { return g_stage; }

static int fails;

static void check(const char *what, int ok)
{
    printf("%-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        fails++;
}

static int truthy(const char *name)
{
    mr_value v;
    if (!mr_var_get(name, &v))
        return 0;
    return mr_value_truthy(&v);
}

/* How many songs a builder function puts in its list. */
static int built_len(const char *call)
{
    mr_value v;
    mr_expr_eval(call, &v);
    return mr_list_len(mr_list_id(&v));
}

int main(int argc, char **argv)
{
    const char *pak = (argc > 1) ? argv[1] : "mrpy.pak";
    const char *dir = (argc > 2) ? argv[2] : "game";
    mr_script *s;
    long now = 0;
    int  i;
    int  base_songs = 0;

    remove("PERSIST.SAV");

    if (!mr_vfs_init(pak, dir)) {
        printf("vfs init failed\n");
        return 1;
    }
    mr_image_init(dir);
    mr_audio_init(dir);
    mr_ui_init(dir);
    mr_scene_init();
    g_stage = mr_surface_new(MR_STAGE_W, MR_STAGE_H);

    s = mr_script_load(dir);
    if (!s || s->n == 0) {
        printf("no script\n");
        return 1;
    }
    mr_interp_init(s, dir);
    mr_interp_start("start");

    printf("-- before anything is unlocked --\n");
    check("unlock_a starts unset", !truthy("persistent.unlock_a"));
    check("unlock_b starts unset", !truthy("persistent.unlock_b"));

    base_songs = built_len("build_rhythm_highscores()");
    printf("   (the high score table starts at %d songs)\n", base_songs);
    check("the table starts with neither unlockable song", base_songs == 7);

    /* Play forward. `label start` sets persistent.unlock_a before it jumps
     * into the story, so this is a short loop -- but it is a loop through
     * the interpreter, which is the point: the flag has to arrive by the
     * script running, not by the test assigning it. */
    for (i = 0; i < 4000 && !truthy("persistent.unlock_a"); i++) {
        mr_interp_update(now);
        now += 33;
        answer_menu();
        mr_interp_advance();
    }

    printf("\n-- after the script sets the first flag --\n");
    check("the script set unlock_a", truthy("persistent.unlock_a"));
    check("  and did NOT set unlock_b", !truthy("persistent.unlock_b"));

    /* The three readers. Each is a different code path: the pyfn `if` in
     * build_rhythm_highscores, the one in build_music_room, and a comparison
     * against True as one of the screens writes it. */
    check("  the high score table gained exactly one song",
          built_len("build_rhythm_highscores()") == base_songs + 1);

    {
        mr_value v;
        mr_expr_eval("persistent.unlock_b == True", &v);
        check("  `persistent.unlock_b == True` is false",
              !mr_value_truthy(&v));
        mr_expr_eval("persistent.unlock_a == True", &v);
        check("  `persistent.unlock_a == True` is true",
              mr_value_truthy(&v));
    }

    /* The jukebox builds a MusicRoom rather than a list, so it is asked how
     * many tracks it ended up with. */
    {
        mr_value v;
        mr_expr_eval("build_music_room()", &v);
        check("  the jukebox was built", v.type != MR_V_NONE);
    }

    /* And the file the script just wrote must carry one flag, not two. */
    mr_persist_save();
    mr_var_unset("persistent.unlock_a");
    mr_var_unset("persistent.unlock_b");
    mr_persist_load();
    check("after a restart, unlock_a is back", truthy("persistent.unlock_a"));
    check("  and unlock_b is still unset", !truthy("persistent.unlock_b"));

    /* --- resetting the persistent data ---------------------------------
     *
     * The reported bug, and it needs the *second* run to show: the baseline
     * is taken at init, after the persistent file has been read, so it held
     * whatever was unlocked at startup. Clearing the persistent data emptied
     * the live store but not the baseline, and the next Start() -- which
     * resets to the baseline -- brought every unlock back. Setting one flag
     * afterwards looked as though it set the other, because the other was
     * already back before it was ever assigned.
     */
    printf("\n-- reset, on a game that starts with both unlocked --\n");

    /* Both set and written, as a finished playthrough leaves them. */
    mr_expr_exec("persistent.unlock_a = True");
    mr_expr_exec("persistent.unlock_b = True");
    mr_persist_save();

    mr_interp_shutdown();
    mr_script_free(s);

    /* A fresh run of the engine, which is what makes the baseline hold
     * them. */
    s = mr_script_load(dir);
    mr_interp_init(s, dir);
    mr_interp_start("start");
    check("a restart has both unlocked", truthy("persistent.unlock_a") &&
                                         truthy("persistent.unlock_b"));

    /* "Effacer les données persistantes" in the preferences. */
    mr_persist_clear(1);
    check("clearing the persistent data clears both",
          !truthy("persistent.unlock_a") && !truthy("persistent.unlock_b"));

    /* Starting a new game resets the story variables to the baseline. The
     * unlocks must not come back with them. */
    mr_var_reset_to_baseline();
    check("  and starting a new game does not bring them back",
          !truthy("persistent.unlock_a") && !truthy("persistent.unlock_b"));

    check("  the high score table is back to its base size",
          built_len("build_rhythm_highscores()") == base_songs);

    /* Now the actual complaint: unlock the first and only the first. */
    mr_expr_exec("persistent.unlock_a = True");
    check("unlocking one after a reset unlocks exactly one",
          truthy("persistent.unlock_a") && !truthy("persistent.unlock_b"));
    check("  the table gained exactly one song",
          built_len("build_rhythm_highscores()") == base_songs + 1);

    /* And a story variable still resets, which is what the baseline is for:
     * the exemption must not have turned it off for everything else. */
    mr_expr_exec("chapter = 7");
    mr_var_reset_to_baseline();
    check("a story variable is still reset by the baseline",
          mr_var_get_int("chapter", -1) != 7);
    check("  while the unlock survives it", truthy("persistent.unlock_a"));

    mr_interp_shutdown();
    mr_script_free(s);
    mr_surface_free(g_stage);
    mr_audio_shutdown();
    mr_ui_shutdown();
    mr_image_shutdown();
    mr_scene_shutdown();
    mr_vfs_shutdown();
    remove("PERSIST.SAV");

    printf(fails ? "\n%d FAILED\n" : "\nall good\n", fails);
    return fails ? 1 : 0;
}

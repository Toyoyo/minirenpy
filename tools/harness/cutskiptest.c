/* Ending a cutscene by skipping, which is not the same exit as a click.
 *
 * Reported from a real run: hold Ctrl during the splashscreen and the engine
 * sticks -- the main menu never appears. Holding Ctrl through a cutscene in
 * the middle of the story was fine, which is what made it look like a
 * splashscreen fault. It is not; it is an ordering fault in
 * mr_interp_update, and the splashscreen is only the shape that exposes it.
 *
 * A cutscene parks the interpreter in ST_WAIT, and four things can take it
 * out again: the deadline, a click, a dismiss key, and the movie running out.
 * cutscene_teardown() is called twice in mr_interp_update to cover them --
 * once at the top, for the exits that happen outside the update, and once
 * after the deadline test. Skipping is a *fifth* exit, and it lives below
 * both of those calls: the block that lets a skip cross a pause turns
 * ST_WAIT into ST_RUN, and then run_until_wait() at the bottom of the same
 * update executes the next statement with the cutscene still up.
 *
 * In mid-story that costs one frame: the next statement is a say, the
 * teardown arrives at the top of the following update, and the only trace is
 * a frame of stale video. After a splashscreen the next statement is
 * `return`, which ends the story in mr_interp_main_menu -- clearing the scene
 * and showing `main_menu` -- and the late teardown then restored the scene
 * captured *before* the cutscene over the top of it, taking the main_menu
 * screen out of the scene with it. Nothing shows it again, so the title
 * screen never appears and no input reaches anything. That is the stick.
 *
 * So the subject here is the order of two things inside one update, and the
 * assertions are written to say which: that the cutscene is gone *before* the
 * statement after it runs, and that what the statement put on screen is still
 * there several frames later. Checking only "the cutscene ended" passes on
 * the broken engine -- the skip did end it.
 *
 * Why this is not in standintest: its label has a say after the cutscene, so
 * the teardown has a frame to spare and every claim it makes comes out green
 * either way. The fixture label here (fixture_splash) is a cutscene followed
 * immediately by a top-level `return`, which is the splashscreen's shape.
 *
 * Takes the fixture pak first and the fixture directory second, for
 * standintest's reason: the fixture declares no `image` statements, so the
 * poster only resolves out of an archive on this host.
 *
 * Both cutscene paths park in the same ST_WAIT and the fault is in the wait,
 * so the stand-in is what is driven here -- it is the half that runs on this
 * host at all. The movie half of the same exit is a frame of video that
 * outlives the cutscene, which no test on this host can see; that claim
 * belongs to the target.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "mr.h"
#include "script.h"
#include "scene.h"
#include "image.h"
#include "interp.h"
#include "vfs.h"
#include "ui.h"
#include "audio.h"

static mr_surface *g_st;
mr_surface *mr_stage(void) { return g_st; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); }
void mr_log(const char *f, ...) { (void)f; }
void mr_quit(void) {}
void mr_invalidate(void) {}
/* A pointer, which is how wavehost.c calls it: this harness reaches
 * mr_audio_poll, because a cutscene stops the music and the main menu starts
 * its own. */
void (*hostshim_wave_sink)(const short *, int) = NULL;

static int fails;

static void check(const char *label, int ok)
{
    printf("  %-56s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

static long T = 1000;

static void frame(void)
{
    mr_audio_poll();
    mr_image_begin_frame();
    mr_interp_update(T);
    T += 33;
}

static mr_scenelist g_list;

static const char *bg_now(void)
{
    mr_scene_capture(&g_list);
    return g_list.bg[0] ? g_list.bg : "(none)";
}

/* Is a named screen in the scene? The display list and the screen list are
 * separate, and the fault destroyed the second one: mr_scene_restore replaces
 * both, so a menu that was shown after the capture is simply not in what
 * comes back. */
static int screen_up(const char *name)
{
    int i;
    for (i = 0; i < mr_scene_screen_count(); i++)
        if (!strcmp(mr_scene_screen_name(i), name))
            return 1;
    return 0;
}

static int poster_up(void)
{
    int i;
    mr_scene_capture(&g_list);
    for (i = 0; i < g_list.n; i++)
        if (!strcmp(g_list.v[i].name, "fixstandin"))
            return 1;
    return 0;
}

int main(int argc, char **argv)
{
    mr_script *s;
    int i;

    setbuf(stdout, NULL);
    if (argc < 3) {
        fprintf(stderr, "usage: cutskiptest <pak> <game-dir>\n");
        return 2;
    }

    mr_vfs_init(argv[1], argv[2]);
    mr_image_init(argv[2]);
    mr_scene_init();
    s = mr_script_load(argv[2]);
    if (!s) {
        fprintf(stderr, "cutskiptest: no script in %s\n", argv[2]);
        return 2;
    }
    mr_interp_init(s, argv[2]);
    mr_ui_init(argv[2]);
    mr_audio_init(argv[2]);

    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);
    if (!g_st) {
        fprintf(stderr, "cutskiptest: could not allocate the stage\n");
        return 2;
    }

    if (mr_script_find_label(s, "fixture_splash") < 0) {
        fprintf(stderr, "cutskiptest: the fixture has no fixture_splash "
                        "label; this is our fixture, not a game's\n");
        return 1;
    }
    mr_interp_start("fixture_splash");

    /* --- the cutscene is up ---------------------------------------------- */
    printf("-- the cutscene --\n");
    for (i = 0; i < 60 && !mr_interp_waiting(); i++) {
        mr_interp_advance();
        frame();
    }
    printf("   it came up after %d frames\n", i);
    check("the cutscene is waiting", mr_interp_waiting());
    /* The precondition: with no poster on the display list, "the poster is
     * gone" below would hold for a reason that has nothing to do with the
     * teardown. */
    check("  with the stand-in's poster on the display list", poster_up());
    check("  and the main menu not up yet", !screen_up("main_menu"));

    /* --- Ctrl, held ------------------------------------------------------- */
    printf("\n-- Ctrl held --\n");
    /* The engine's own entry point for the key, not the flag: input.c calls
     * this and nothing else for Ctrl down, so a test that set state directly
     * would not be testing the reported action. */
    mr_interp_skip_hold(1);

    /* Exactly one update. This is the one that has to do all of it: end the
     * wait, tear the cutscene down, and only then run the `return` that ends
     * the story. Looping here instead would let a late teardown land on the
     * next update and hide the whole fault. */
    frame();

    printf("   bg=%s, poster=%d, main_menu=%d, waiting=%d\n",
           bg_now(), poster_up(), screen_up("main_menu"),
           mr_interp_waiting());
    check("the skip ended the cutscene", !mr_interp_waiting());
    /* Both halves of "the teardown ran before run_until_wait did". The poster
     * coming off is the display list, and the menu being up is the statement
     * after the cutscene having run -- in that order, within this one
     * update. */
    check("  and the poster came off with it", !poster_up());
    check("  and the `return` after it reached the main menu",
          screen_up("main_menu"));

    /* --- and it stays ----------------------------------------------------- */
    printf("\n-- the frames after --\n");
    /* Where the stick was visible: on the broken engine the menu came up on
     * the frame above and was wiped on the next one, by a teardown restoring
     * the scene from before the cutscene. Ctrl is deliberately still held --
     * the player in the report was leaning on it. */
    for (i = 0; i < 10; i++)
        frame();
    printf("   bg=%s, poster=%d, main_menu=%d\n",
           bg_now(), poster_up(), screen_up("main_menu"));
    check("the main menu is still up ten frames later",
          screen_up("main_menu"));
    check("  and the pre-cutscene scene was not restored over it",
          strcmp(bg_now(), "fix-room") != 0);
    check("  and the poster did not come back", !poster_up());

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}

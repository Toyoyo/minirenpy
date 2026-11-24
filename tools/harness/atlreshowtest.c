/* A re-show that says nothing about where the image goes, on top of a
 * running ATL.
 *
 *     show azami magical pout:
 *         linear 0.5 xoffset 500
 *     show azami magical fear with dissolve
 *
 * Reported from play against a Ren'Py reference run: Ren'Py leaves her where
 * she was, we leave her 500 pixels to the right.
 *
 * Nothing draws a frame between those two statements -- a `show` is not an
 * interaction -- so the question is what a re-show does to an animation that
 * has been armed and has not run. The second half of the fixture asks the
 * same question of an animation that *has* run, because the two must not
 * have the same answer.
 *
 * Runs on tools/harness/story, label atl_reshow.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "mr.h"
#include "script.h"
#include "scene.h"
#include "image.h"
#include "interp.h"
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

static long T;
static int fails;

static void check(const char *label, long got, long want)
{
    int ok = (got == want);
    printf("%-54s %6ld want %6ld  %s\n", label, got, want,
           ok ? "ok" : "** FAIL **");
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

static int person_xoffset(void)
{
    mr_scenelist sl;
    int i;

    mr_scene_render(g_st, T);
    mr_scene_capture(&sl);
    for (i = 0; i < sl.n; i++) {
        if (!strcmp(sl.v[i].tag, "person"))
            return sl.v[i].place.has_xoffset ? sl.v[i].place.xoffset : 0;
    }
    return -9999;
}

/* Run to the line that starts with `want`, letting the clock run so any
 * transition and any animation on the way has time to play out. */
static int reach(const char *want)
{
    int guard = 0;

    while (guard++ < 400) {
        if (!strncmp(line_now(), want, strlen(want)))
            return 1;
        T += 33;
        mr_interp_update(T);
        mr_scene_render(g_st, T);
        mr_interp_advance();
        mr_interp_update(T);
    }
    return 0;
}

/* Let the clock run without advancing the story. */
static void settle(int ms)
{
    int i;

    for (i = 0; i < ms; i += 33) {
        T += 33;
        mr_interp_update(T);
        mr_scene_render(g_st, T);
    }
}

int main(int argc, char **argv)
{
    mr_script *s;

    setbuf(stdout, NULL);
    if (argc < 2) {
        fprintf(stderr, "usage: atlreshowtest <story-dir>\n");
        return 2;
    }

    mr_vfs_init(NULL, argv[1]);
    mr_image_init(argv[1]);
    mr_scene_init();
    s = mr_script_load(argv[1]);
    mr_interp_init(s, argv[1]);
    mr_ui_init(argv[1]);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);
    mr_interp_start("atl_reshow");

    reach("Before the move.");
    settle(600);
    check("at centre to begin with", person_xoffset(), 0);

    /* The reported pair in isolation. The ATL is armed and the image swapped
     * in the same batch of statements, so it never gets a frame to run in --
     * and it still has to run, because a re-show that says nothing about
     * position keeps the transform going (scenelists.py:435-441). From
     * centre, 500 is where it ends. */
    reach("Replaced before it could run.");
    settle(900);
    check("an ATL that never ran still runs after a re-show",
          person_xoffset(), 500);

    /* The other order: the move happens, *then* something replaces it. A
     * re-show that says nothing about position must not rewind what already
     * happened -- it is a change of expression, not a change of place. */
    reach("Back at centre.");
    settle(600);
    check("back at centre", person_xoffset(), 0);

    reach("Moving.");
    settle(900);
    check("the move runs when it is given the time",
          person_xoffset(), 500);

    reach("Replaced after it ran.");
    settle(900);
    check("a re-show leaves a finished move alone",
          person_xoffset(), 500);

    /* The shape the report actually has: an offset already in force from an
     * earlier move, and then the reported pair on top of it. */
    reach("Centre again.");
    settle(600);
    check("centre again", person_xoffset(), 0);

    reach("Moved a long way.");
    settle(900);
    check("the earlier move leaves its offset behind",
          person_xoffset(), 830);

    /* The report, with its real numbers. The pending move must survive the
     * re-show and land on 500 -- which on a 1280 stage is about the middle,
     * where the Ren'Py reference run has her. Cancelling it left her at the
     * 830 the earlier move ended on, out to the right. */
    reach("Replaced while a second move was pending.");
    settle(900);
    check("a pending move survives a re-show that says nothing",
          person_xoffset(), 500);

    mr_surface_free(g_st);
    mr_scene_shutdown();
    mr_image_shutdown();
    mr_vfs_shutdown();
    mr_script_free(s);

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}

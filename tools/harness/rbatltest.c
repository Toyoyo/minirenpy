/* Rolling back onto a line whose image is part-way through an ATL.
 *
 *     show azami magical cry:
 *         linear 0.5 yoffset 200
 *     "..."
 *
 * Reported from play: the sprite drops, the player advances one line and
 * rolls back, and she is at the top of the drop again.
 *
 * The cause is a timing one, and it is worth stating because it is not
 * visible in either function on its own. A say checkpoint is taken as the
 * line goes up -- *before* its interaction, so before the half second has
 * run -- and mr_scene_capture was stripping `atl`, `step` and `step_start`
 * out of the list it recorded. So the checkpoint held the image at yoffset 0
 * with no animation left to get it anywhere else, and restoring it was
 * faithful to a state that only ever existed for one frame.
 *
 * Keeping those three fields is the whole fix, and it works because
 * step_start is an absolute timestamp: by the time a rollback restores it the
 * clock is long past the end of the step, so the first atl_update finishes
 * the interpolation and the sprite is where she ended up. Ren'Py arrives at
 * the same place from the other direction -- the SceneListEntry's show_time
 * rolls back too, so the transform's st is large and the ATL renders past its
 * end rather than replaying.
 *
 * Runs on tools/harness/story, label rb_atl.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "mr.h"
#include "script.h"
#include "scene.h"
#include "image.h"
#include "interp.h"
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

static long T;
static int fails;

static void check(const char *label, long got, long want)
{
    int ok = (got == want);
    printf("%-52s %6ld want %6ld  %s\n", label, got, want,
           ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

static void checkb(const char *label, int got, int want)
{
    int ok = (got == want);
    printf("%-52s %6d want %6d  %s\n", label, got, want,
           ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

/* Where the `person` tag sits right now, as the renderer sees it. The
 * animation is advanced by mr_scene_render, so the frame has to be drawn
 * before the answer means anything -- which is also how the player sees it. */
static int person_yoffset(void)
{
    mr_scenelist sl;
    int i;

    mr_scene_render(g_st, T);
    mr_scene_capture(&sl);
    for (i = 0; i < sl.n; i++) {
        if (!strcmp(sl.v[i].tag, "person"))
            return sl.v[i].place.has_yoffset ? sl.v[i].place.yoffset : 0;
    }
    return -9999;
}

static int person_animating(void)
{
    return mr_scene_animating(T);
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

/* One interaction: advance until the displayed line changes. */
static int next_line(void)
{
    char was[200];
    int guard = 0;

    sprintf(was, "%.190s", line_now());
    while (guard++ < 60) {
        mr_interp_update(T);
        T += 33;
        mr_interp_advance();
        mr_interp_update(T);
        if (strcmp(line_now(), was) && strcmp(line_now(), "(none)"))
            return 1;
    }
    return 0;
}

/* Let the clock run without advancing the story, so an ATL can finish. */
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
        fprintf(stderr, "usage: rbatltest <story-dir>\n");
        return 2;
    }

    mr_vfs_init(NULL, argv[1]);
    mr_image_init(argv[1]);
    mr_scene_init();
    s = mr_script_load(argv[1]);
    mr_interp_init(s, argv[1]);
    mr_ui_init(argv[1]);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);
    mr_interp_start("rb_atl");

    /* --- a one-shot ATL ------------------------------------------------ */

    next_line();                       /* "Before the drop." */
    check("before the drop: no offset", person_yoffset(), 0);

    next_line();                       /* "During the drop." -- ATL armed */
    checkb("the drop is running", person_animating(), 1);

    /* Part way in. The exact value depends on the frame clock, so the
     * assertion is the direction, not a number: she has left the top and has
     * not arrived. */
    settle(200);
    {
        int y = person_yoffset();
        checkb("part way down, she is between the two", y > 0 && y < 200, 1);
    }

    settle(500);
    check("the drop finishes at its target", person_yoffset(), 200);
    checkb("  and stops animating", person_animating(), 0);

    next_line();                       /* "After the drop." */
    check("the next line leaves her there", person_yoffset(), 200);

    /* The report. One line back, onto the line the drop played over. */
    mr_interp_rollback();
    mr_interp_update(T);
    check("rolling back onto that line keeps her down",
          person_yoffset(), 200);
    checkb("  and the line really did change",
           !strcmp(line_now(), "During the drop."), 1);

    /* Rolling back *past* the show must undo it, or the fix would just be a
     * display list that never forgets. This is the other half of the
     * behaviour and the one a careless version breaks. */
    mr_interp_rollback();
    mr_interp_update(T);
    check("rolling back past the show undoes it", person_yoffset(), 0);
    checkb("  landing on the line before",
           !strcmp(line_now(), "Before the drop."), 1);

    /* --- a repeating ATL ----------------------------------------------- */

    /* An ATL with `repeat` is never finished. Restoring a stale step_start
     * must leave it running rather than freezing it on whatever step the
     * clock happens to land on -- atl_update restarts the cycle from now
     * when it laps, which is what keeps that true. */
    mr_interp_start("rb_atl");
    while (next_line() && strcmp(line_now(), "Bobbing."))
        ;
    checkb("the bob is running", person_animating(), 1);

    settle(1000);
    checkb("  still running a second later", person_animating(), 1);

    next_line();                       /* "Still bobbing." */
    settle(300);
    mr_interp_rollback();
    mr_interp_update(T);
    mr_scene_render(g_st, T);
    checkb("a rollback leaves a repeating ATL running",
           person_animating(), 1);
    {
        int y = person_yoffset();
        checkb("  and within its range", y >= 0 && y <= 80, 1);
    }

    mr_surface_free(g_st);
    mr_scene_shutdown();
    mr_image_shutdown();
    mr_vfs_shutdown();
    mr_script_free(s);

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}

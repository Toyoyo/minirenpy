/* A named transform whose body animates, applied with `at`.
 *
 *     transform moveleft:
 *         linear 0.5 xalign 0.0
 *     ...
 *     show saza at moveleft
 *
 * Reported from Azami: the sprite ends up at the right coordinates but gets
 * there without travelling -- it jumps, or the following dissolve covers the
 * jump. `show x at left830`, a transform with no timed step in it, is fine,
 * so the placement half of the feature works and the animation half does not.
 *
 * This asks for the *trajectory*, not the endpoint, because the endpoint was
 * already right in the report. Every other ATL test here checks where the
 * image settles, which is exactly the measurement that cannot see this bug.
 *
 * Three shapes, all of them in Azami:
 *   - a body that is only a timed step (`moveleft`)
 *   - the same under the dissolve that follows it in the script, with no
 *     frame drawn in between
 *   - a placement and then a timed step (`leftground`)
 *
 * Runs on tools/harness/story, label tf_atl.
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

/* The measurement this test exists for: a value that has left `from` and not
 * yet arrived at `to`. Stated as a range because the exact number depends on
 * which frame the sample landed on, and pinning that would make the test
 * about the frame clock. */
static void check_between(const char *label, long got, long from, long to)
{
    long lo = from < to ? from : to;
    long hi = from < to ? to : from;
    int ok = (got > lo && got < hi);

    printf("%-54s %6ld want %ld..%ld (exclusive)  %s\n", label, got, lo, hi,
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

static int person_off(int want_y)
{
    mr_scenelist sl;
    int i;

    mr_scene_render(g_st, T);
    mr_scene_capture(&sl);
    for (i = 0; i < sl.n; i++) {
        if (strcmp(sl.v[i].tag, "person"))
            continue;
        if (want_y)
            return sl.v[i].place.has_yoffset ? sl.v[i].place.yoffset : 0;
        return sl.v[i].place.has_xoffset ? sl.v[i].place.xoffset : 0;
    }
    return -9999;
}

static int person_x(void) { return person_off(0); }
static int person_y(void) { return person_off(1); }

/* The resolved stage x of the anchor point, asked with a zero-width image so
 * the answer does not depend on the fixture sprite's size: both halves of
 * mr_place_resolve's x then reduce to "where the anchor goes", which is the
 * thing that has to travel. Read through the resolver rather than off a
 * field because an align and a pos/anchor pair are different fields saying
 * the same thing -- a test that reads one of them cannot see a move that
 * switched representation halfway. */
static int person_stage_x(void)
{
    mr_scenelist sl;
    int i, x, y;

    mr_scene_render(g_st, T);
    mr_scene_capture(&sl);
    for (i = 0; i < sl.n; i++) {
        if (strcmp(sl.v[i].tag, "person"))
            continue;
        mr_place_resolve(&sl.v[i].place, sl.v[i].has_place, 0, 0, &x, &y);
        return x;
    }
    return -9999;
}

/* The fixture sprite is 300x600 of flat (230,210,200) and the `dark`
 * background is flat (42,42,46), so one pixel is enough to answer "is there
 * anything of her here". Compared against a corner of the stage rather than
 * against a literal colour: the stage is whatever format the host build uses
 * and the point is only that the two match. */
static int stage_px(int x, int y)
{
    return (int)g_st->px[y * g_st->pitch + x];
}

/* Run to the line that starts with `want`. Stops on the frame the line first
 * appears on, so an animation armed by the statements just above it has had
 * at most one frame to run -- which is what lets the caller sample it in
 * flight. */
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

static void settle(int ms)
{
    int i;

    for (i = 0; i < ms; i += 33) {
        T += 33;
        mr_interp_update(T);
        mr_scene_render(g_st, T);
    }
}

/* Print the whole path so a failure says what shape it had -- a jump, a stall,
 * or a move that ran to the wrong place -- rather than only that one sample
 * was out of range. */
static void trace(const char *what, int ms, int (*read)(void))
{
    int i;

    printf("  %s:", what);
    for (i = 0; i < ms; i += 100) {
        printf(" %d", read());
        settle(100);
    }
    printf(" %d\n", read());
}

int main(int argc, char **argv)
{
    mr_script *s;

    setbuf(stdout, NULL);
    if (argc < 2) {
        fprintf(stderr, "usage: tfatltest <story-dir>\n");
        return 2;
    }

    mr_vfs_init(NULL, argv[1]);
    mr_image_init(argv[1]);
    mr_scene_init();
    s = mr_script_load(argv[1]);
    mr_interp_init(s, argv[1]);
    mr_ui_init(argv[1]);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);
    mr_interp_start("tf_atl");

    /* A transform with no timed step: the placement arrives at once and
     * nothing moves afterwards. The control for everything below. */
    reach("Parked on the right.");
    settle(600);
    check("`at here940` places and does not animate", person_x(), 940);

    /* The report. 940 -> 100 over half a second. reach() stops on the frame
     * the line first appears on, which is the frame the ATL was armed on, so
     * one frame has to pass before there is any travel to see. */
    reach("Sliding left.");
    settle(100);
    check_between("`at slideleft` has left its start", person_x(), 940, 100);
    trace("slideleft", 600, person_x);
    check("`at slideleft` arrives", person_x(), 100);

    reach("Parked again.");
    settle(600);
    check("back on the right", person_x(), 940);

    /* The same move with a dissolve starting in the same batch of statements.
     * The dissolve must not freeze it: Ren'Py re-renders both sides of a
     * Dissolve every frame, so a transform under one keeps running. */
    reach("Sliding left under a dissolve.");
    settle(100);
    check_between("under a dissolve, still travelling", person_x(), 940, 100);
    trace("slideleft+dissolve", 600, person_x);
    check("under a dissolve, arrives", person_x(), 100);

    /* Place *and* animate. The xoffset is the placement and lands at once;
     * the yoffset is the step and has to travel. */
    reach("Rising from the ground.");
    check("`at groundrise` places at once", person_x(), 0);
    settle(100);
    check_between("`at groundrise` has left its start", person_y(), 0, 300);
    trace("groundrise y", 600, person_y);
    check("`at groundrise` arrives", person_y(), 300);

    /* The report proper: a stock position, then a transform body that names
     * the same axis under the other spelling. 1280 -> 0, and the whole of
     * the bug was that the two ends were right and the middle was not. */
    reach("Over at the right.");
    settle(600);
    check("`at right` is at the right edge", person_stage_x(), 1280);

    reach("Sliding in from the right edge.");
    settle(100);
    check_between("`linear xalign` travels from a stock position",
                  person_stage_x(), 1280, 0);
    trace("slideleftalign", 600, person_stage_x);
    check("`linear xalign` arrives", person_stage_x(), 0);

    /* The mirror. `at truecenter` is 0.5 of the stage; the step's target is
     * the bare word `right`, which place_from_name stores as an align. */
    reach("Dead centre.");
    settle(600);
    check("`at truecenter` is mid-stage", person_stage_x(), 640);

    reach("Sliding back out to the right.");
    settle(100);
    check_between("a step onto a stock position travels",
                  person_stage_x(), 640, 1280);
    trace("slidetoright", 600, person_stage_x);
    check("a step onto a stock position arrives", person_stage_x(), 1280);

    /* The ghost. She starts at xoffset 940, so she covers x 940..1240; by the
     * middle of a half-second slide to 100 she is around 520..820 and x=1100
     * is bare background. If the dissolve is transitioning from a baseline
     * that predates her move, the old side still has her at 940 and that
     * pixel is half sprite.
     *
     * Checked while the dissolve is actually running -- after it ends there
     * is only one list left and the ghost cannot show however wrong the
     * baseline was. */
    reach("Parked for the ghost test.");
    settle(600);
    check("parked for the ghost test", person_stage_x(), 940);

    /* Sampled frame by frame from here rather than with reach()+settle():
     * the interpreter blocks on a transition, so by the time the say line
     * after it is up the dissolve has finished and the ghost -- which exists
     * only while two lists are being blended -- is gone. */
    {
        int guard, worst = stage_px(20, 50), worst_x = -1;

        T += 33;
        mr_interp_update(T);
        mr_scene_render(g_st, T);
        mr_interp_advance();
        mr_interp_update(T);

        for (guard = 0; guard < 60; guard++) {
            T += 33;
            mr_interp_update(T);
            mr_scene_render(g_st, T);
            /* Only once she has left the start box, or the sprite genuinely
             * covering that pixel would read as a ghost. */
            if (mr_scene_in_transition() && person_stage_x() < 800 &&
                stage_px(1150, 300) != stage_px(20, 50)) {
                worst = stage_px(1150, 300);
                worst_x = person_stage_x();
                break;
            }
        }
        printf("  ghost sweep: x at the sample %d\n", worst_x);
        /* (1150,300) is inside her start box and in the flat part of
         * dark.png -- the background has two shades and the darker one
         * covers the middle of the stage, which is not a place to ask this
         * question. */
        check("no half-transparent copy left at the start position",
              worst, stage_px(20, 50));
    }

    mr_surface_free(g_st);
    mr_scene_shutdown();
    mr_image_shutdown();
    mr_vfs_shutdown();
    mr_script_free(s);

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}

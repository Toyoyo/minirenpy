/* A dissolve must start from what is on screen.
 *
 * Reported against the credit roll: open the game menu while the credits are
 * scrolling, close it, let the scroll finish, and the final
 * `show fin` + `with dissolve` briefly replays the credits from where they
 * were when the menu was opened.
 *
 * The mechanism: scene.c keeps two display lists, g_cur (pending) and g_prev
 * (what the player is looking at). atl_update advances running transforms,
 * and it is only ever called on **g_cur** (mr_scene_render). g_prev's copy of
 * an animating entry therefore keeps whatever placement it had at the last
 * mr_scene_commit, which is a plain struct copy of g_cur. take_snapshots
 * renders g_prev into the dissolve's "from" image -- so the dissolve fades
 * *from* a stale position.
 *
 * Opening the game menu commits, and so does closing it, which is why the
 * menu is what makes this visible: without it the last commit is far enough
 * back that the credits are not in g_prev at all.
 *
 * The invariant asserted here is the general one rather than the symptom: at
 * t=0 a dissolve output must equal the frame immediately before it started.
 * Anything else is a visible jump, whatever caused it.
 *
 * Takes the credits fixture.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "mr.h"
#include "expr.h"
#include "interp.h"
#include "vfs.h"
#include "script.h"
#include "image.h"
#include "scene.h"
#include "ui.h"
#include "slrender.h"

static mr_surface *g_st;
mr_surface *mr_stage(void) { return g_st; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); exit(1); }
#include <stdarg.h>
void mr_log(const char *f, ...) { (void)f; }
void mr_quit(void) {}
void mr_invalidate(void) {}
void (*hostshim_wave_sink)(const short *, int) = NULL;

static int g_fail;
static void check(const char *l, long got, long want)
{
    if (got != want) g_fail++;
    printf("%-52s %-8ld %s\n", l, got, got == want ? "ok" : "** WANT DIFFERENT **");
}

static long g_t;
static mr_surface *g_prevframe;

static void frame(void)
{
    g_t += 33;
    mr_interp_update(g_t);
    mr_sl_focus_reset();
    mr_interp_render(g_st, g_t);
}

/* Remember the frame just drawn. */
static void keep(void)
{
    int y;
    for (y = 0; y < g_st->h; y++)
        memcpy(g_prevframe->px + (size_t)y * g_prevframe->pitch,
               g_st->px + (size_t)y * g_st->pitch,
               (size_t)g_st->w * sizeof(mr_u32));
}

/* How many pixels differ between the current stage and the kept frame. */
static long diff_from_kept(void)
{
    long n = 0;
    int y, x;
    for (y = 0; y < g_st->h; y++) {
        const mr_u32 *a = g_st->px + (size_t)y * g_st->pitch;
        const mr_u32 *b = g_prevframe->px + (size_t)y * g_prevframe->pitch;
        for (x = 0; x < g_st->w; x++)
            if ((a[x] & 0xFFFFFF) != (b[x] & 0xFFFFFF))
                n++;
    }
    return n;
}

/* Where the scrolling image has got to: the topmost row that differs from a
 * baseline frame captured with the scene up and the roll still off-stage.
 * -1 when nothing differs, i.e. the roll is not on screen.
 *
 * Two earlier versions of this were wrong in ways worth recording, because
 * both *passed*:
 *
 *   - It sampled only the eight columns either side of the middle. That works
 *     for `roll`, which is given `xalign 0.5`, and reads the background for
 *     the entire run of the real ending, whose roll has no xalign and is
 *     420px wide at x=0.
 *   - Widening it to the full stage and taking px[0] as "the background" is
 *     worse: the roll reaches x=0, so once it arrives the background *is* the
 *     roll, and the first row then trivially differs from itself. It reported
 *     the roll pinned at row 0 for the whole run.
 *
 * A baseline has neither problem and asks the question directly: which rows
 * does the roll change? */
static mr_surface *g_base;
static int          g_have_base;

static void capture_base(void)
{
    int y;
    for (y = 0; y < g_st->h; y++)
        memcpy(g_base->px + (size_t)y * g_base->pitch,
               g_st->px + (size_t)y * g_st->pitch,
               (size_t)g_st->w * sizeof(mr_u32));
    g_have_base = 1;
}

static int roll_top(void)
{
    int y, x;

    if (!g_have_base)
        mr_fatal("roll_top with no baseline captured");

    for (y = 0; y < g_st->h; y++) {
        const mr_u32 *row = g_st->px + (size_t)y * g_st->pitch;
        const mr_u32 *b   = g_base->px + (size_t)y * g_base->pitch;
        for (x = 0; x < g_st->w; x++)
            if ((row[x] & 0xFFFFFF) != (b[x] & 0xFFFFFF))
                return y;
    }
    return -1;
}

/* Where the engine says the roll is: the resolved top edge of the shown image
 * named `name`, in stage coordinates. INT_MIN if it is not on the display
 * list at all.
 *
 * Asked of the display list rather than measured off the pixels. Two pixel
 * probes were tried first and both were dominated by things that are not the
 * roll -- the dialogue window going away, `fin` arriving, the roll reaching
 * x=0 and becoming its own "background". The placement is what the roll
 * actually *is*, so reading it directly is both exact and immune to whatever
 * else is on the stage. The pixel check below covers the other half, that
 * the thing at that placement is really drawn. */
#define NOT_SHOWN (-100000)

static int roll_y(const char *name)
{
    mr_scenelist l;
    mr_surface  *img;
    int i, x, y;

    mr_scene_capture(&l);
    for (i = 0; i < l.n; i++) {
        if (strcmp(l.v[i].name, name))
            continue;
        mr_image_begin_frame();
        img = mr_image_get(name);
        if (!img)
            mr_fatal("roll image '%s' will not load", name);
        mr_place_resolve(&l.v[i].place, 1, img->w, img->h, &x, &y);
        return y;
    }
    return NOT_SHOWN;
}

/* Does the roll actually roll?
 *
 * This harness was written for one specific bug -- a dissolve starting from a
 * stale position -- and asserted only that. It never checked that the credits
 * appear on screen or that they move, so "the credits do not roll at all"
 * passed it. That is the gap this closes.
 *
 * `label`'s roll runs for 5 seconds; this samples the first four of them, so
 * every sample is of the roll itself and not of what follows it.
 */
static void rolls_at_all(const char *label, const char *name)
{
    int i, step;
    int climbed = 0, went_backwards = 0, drawn = 0;
    int first = NOT_SHOWN, last = NOT_SHOWN, prev = NOT_SHOWN;

    printf("\n-- %s: does %s reach the screen, and does it move? --\n",
           label, name);

    mr_interp_start(label);
    g_t += 1000;
    g_have_base = 0;

    /* Past the opening line, which leaves the interpreter on the hard pause
     * that covers the roll. Few frames rather than many: the roll starts the
     * moment the line is dismissed, and every frame spent here is a frame of
     * the roll missed, so a slow start would be sampled already half done. */
    for (i = 0; i < 4; i++)
        frame();
    mr_interp_advance();

    /* The roll starts below the stage (`ypos 720 yanchor 0.0`), so this frame
     * is the scene with the window gone and none of the roll in it: the
     * baseline for "is the roll being drawn". */
    frame();
    capture_base();

    /* 60 samples of two frames is about 4s of the roll's 5. */
    for (step = 0; step < 60; step++) {
        int y;

        for (i = 0; i < 2; i++)
            frame();

        y = roll_y(name);
        if (y == NOT_SHOWN)
            continue;

        if (first == NOT_SHOWN)
            first = y;
        last = y;
        if (prev != NOT_SHOWN) {
            if (y < prev)       climbed++;
            else if (y > prev)  went_backwards++;
        }
        prev = y;

        /* And that something is actually painted once it is on stage. A
         * placement that moves while nothing is drawn is the other way for
         * this to be broken, and the placement alone cannot see it. */
        if (y < MR_STAGE_H && roll_top() >= 0)
            drawn++;
    }

    printf("   top edge %d -> %d, %d steps up, %d down, "
           "painted on %d samples\n", first, last, climbed, went_backwards,
           drawn);

    check("the roll is on the display list", first != NOT_SHOWN, 1);
    /* Starts low on the stage. Not "at or below 720": the first sample is
     * two frames in, by which time it has legitimately climbed a little.
     * This still catches a roll that begins already at the top. */
    check("it starts low on the stage", first > MR_STAGE_H / 2, 1);
    check("it moves upward", climbed > 0, 1);
    check("and never back down", went_backwards, 0);
    /* Not merely "it moved": a roll that twitched a few pixels and stopped
     * would satisfy everything above. */
    check("crossing most of the stage", (first - last) > MR_STAGE_H / 2, 1);
    check("and it is actually painted while on stage", drawn > 0, 1);
}

/* One run of the credit roll. Returns how many pixels changed on the first
 * frame of the closing dissolve, or -1 if no dissolve was seen. */
static long run(int open_menu)
{
    int i;

    mr_interp_start("start");
    g_t += 1000;
    g_have_base = 0;

    /* Past the opening line and into the roll. */
    for (i = 0; i < 60; i++)
        frame();
    mr_interp_advance();

    /* roll_top is measured against a baseline, so this run needs its own --
     * the one rolls_at_all left behind belongs to a different label. Taken on
     * the first frame after the roll statement, while the roll is still below
     * the stage. */
    frame();
    capture_base();

    for (i = 0; i < 20; i++)
        frame();

    if (roll_top() < 0)
        return -2;

    if (open_menu) {
        /* Both the opening and the closing commit, which is what puts a
         * mid-scroll placement into g_prev. */
        mr_interp_game_menu_toggle();
        for (i = 0; i < 10; i++)
            frame();
        mr_interp_game_menu_toggle();
        for (i = 0; i < 10; i++)
            frame();
    }

    for (i = 0; i < 400; i++) {
        int was_busy = mr_scene_busy(g_t);
        keep();
        frame();
        if (!was_busy && mr_scene_busy(g_t))
            return diff_from_kept();
    }
    return -1;
}

int main(int argc, char **argv)
{
    mr_script *s;
    long with_menu, without;

    setbuf(stdout, NULL);
    (void)argc;
    mr_vfs_init(NULL, argv[1]); mr_image_init(argv[1]); mr_scene_init();
    s = mr_script_load(argv[1]);
    mr_interp_init(s, argv[1]);
    mr_interp_start("start");
    mr_ui_init(argv[1]);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);
    g_prevframe = mr_surface_new(MR_STAGE_W, MR_STAGE_H);
    g_base      = mr_surface_new(MR_STAGE_W, MR_STAGE_H);

    /* First the thing this harness never asked: that the credits are on
     * screen and scrolling. Both shapes -- the stage-sized opaque one and
     * the tall RGBA one the real ending uses. */
    rolls_at_all("start", "roll");
    rolls_at_all("tallroll", "rolltall");

    printf("\n-- and a dissolve after it starts from what was on screen --\n");
    without   = run(0);
    with_menu = run(1);

    check("a dissolve happened, undisturbed", without >= 0, 1);
    check("a dissolve happened, after the menu", with_menu >= 0, 1);
    printf("  pixels changed on its first frame: %ld undisturbed, "
           "%ld after the menu\n", without, with_menu);

    /* A dissolve's first frame is t=0, the outgoing image alone, so it must
     * match the frame before it *exactly*. Not a threshold: the snapshot is
     * taken from the same lists, stepped to the same moment, as the frame
     * just drawn, so any difference at all is something being out of step.
     *
     * Worth knowing what the failures looked like, since an approximate
     * assertion would have accepted two of the three:
     *   755200  the reported bug -- g_prev frozen at the last commit
     *   921600  no commit for the pause, so g_prev predates `show roll`
     *     6400  stepping the transforms again at transition time, putting
     *           the snapshot a partial frame ahead of the screen
     * The last is five rows of a 1280-wide image, i.e. one frame of the
     * roll's motion, and is exactly the kind of "close enough" a tolerance
     * would have hidden. */
    check("undisturbed, it starts from what was on screen", without, 0);
    check("and after the menu, likewise", with_menu, 0);

    printf("\n%d failed\n", g_fail);
    return g_fail != 0;
}

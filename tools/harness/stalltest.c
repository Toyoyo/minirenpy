/* The screen must be correct on *every* frame, including the ones the frame
 * loop decides not to draw.
 *
 * bandtest checks that a partial frame looks like a full one, but it obeys
 * the same skip decision for both stages -- so when the loop wrongly skips a
 * frame, bandtest's reference goes stale in exactly the same way and the two
 * still agree. A frame that should have been drawn and was not is invisible
 * to it.
 *
 * That is the shape of the reported bug: the last characters of a line were
 * missing ("Je jou" for "Je joue.") until something forced a repaint. So this
 * keeps `shown`, the stage as the engine would really have left it, and
 * compares it against a full render taken every frame whether the loop drew
 * or not.
 *
 * Takes the story fixture.
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

static mr_surface *g_shown;   /* what the player would be looking at */
static mr_surface *g_ref;     /* the same moment, drawn in full */
mr_surface *mr_stage(void) { return g_shown; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); exit(1); }
void mr_log(const char *f, ...) { (void)f; }
void mr_quit(void) {}
static int g_dirty;
void mr_invalidate(void) { g_dirty = 1; }
void (*hostshim_wave_sink)(const short *, int) = NULL;

static int g_fail;
static void check(const char *l, long got, long want)
{
    if (got != want) g_fail++;
    printf("%-52s %-7ld %s\n", l, got, got == want ? "ok" : "** WANT DIFFERENT **");
}

static long diff(void)
{
    long n = 0;
    int y, x;
    for (y = 0; y < g_shown->h; y++) {
        const mr_u32 *a = g_shown->px + (size_t)y * g_shown->pitch;
        const mr_u32 *b = g_ref->px + (size_t)y * g_ref->pitch;
        for (x = 0; x < g_shown->w; x++)
            if ((a[x] & 0xFFFFFF) != (b[x] & 0xFFFFFF))
                n++;
    }
    return n;
}

static long g_T;

/* One walk through the story. 700 frames rather than more: this renders the
 * stage twice per frame, and seven walks at 1500 took a minute and a half. `cps` is the text speed (0 means instant) and
 * `skip_every` how often to start a skip, 0 for never. Returns the worst
 * number of stale pixels seen. */
static long run(long cps, int skip_every, int advance_every)
{
    int i;
    long worst = 0;
    int worst_frame = -1, worst_kind = -1, worst_drawn = -1;
    long T = g_T;

    mr_interp_skip_stop();
    mr_var_set_int("_preferences.text_cps", cps);
    mr_interp_action("Start()");
    mr_invalidate();

    for (i = 0; i < 700; i++) {
        int kind, moving, band = 0, drew = 0;
        int bx, by, bw, bh;

        mr_interp_update(T);
        T += 33;

        moving = mr_scene_animating(T);
        kind = mr_interp_redraw_kind();

        /* Exactly main.c's decision. */
        if (g_dirty || kind != MR_REDRAW_NONE || moving) {
            if (!g_dirty && !moving && kind == MR_REDRAW_DIALOGUE &&
                mr_ui_dialogue_rect(&bx, &by, &bw, &bh))
                band = 1;

            g_dirty = 0;
            mr_sl_focus_reset();
            mr_clip_reset();
            if (band)
                mr_clip_push(bx, by, bw, bh);
            mr_interp_render(g_shown, T);
            mr_ui_draw(g_shown);
            mr_interp_render_overlay(g_shown);
            if (band)
                mr_clip_pop();
            drew = 1;
        }

        /* The truth, every frame, drawn or not. Note this renders a second
         * time at the same timestamp, which is only meaningful because the
         * scene layer is driven by the timestamp rather than by being
         * called. */
        mr_sl_focus_reset();
        mr_clip_reset();
        mr_interp_render(g_ref, T);
        mr_ui_draw(g_ref);
        mr_interp_render_overlay(g_ref);

        {
            long d = diff();
            if (d > 0 && worst_frame < 0)
                printf("  first stale at frame %d: %ld px, kind=%d drawn=%d "
                       "skipping=%d\n", i, d, kind, drew,
                       mr_interp_skipping());
            if (d > worst) {
                worst = d;
                if (worst_frame < 0) worst_frame = i;
                worst_kind = kind;
                worst_drawn = drew;
            }
        }

        /* A skip every so often, because the reported staleness came back
         * on "the first text block after a skip". A skip stops of its own
         * accord at the first unread line, so this only has to start one. */
        if (skip_every && (i % skip_every) == 0) {
            mr_interp_skip_toggle();
            mr_invalidate();
        }

        /* Advance now and then, so the walk covers many lines.
         * mr_invalidate() because main.c does: every input path sets the
         * dirty flag, and without it this reports a stale frame after each
         * click that is the harness's fault rather than the engine's. */
        if ((i % advance_every) == advance_every - 1) {
            mr_interp_advance();
            mr_invalidate();
        }
    }
    g_T = T;

    if (worst)
        printf("    worst frame %d: %ld px (kind=%d drawn=%d)\n",
               worst_frame, worst, worst_kind, worst_drawn);
    return worst;
}

int main(int argc, char **argv)
{
    mr_script *s;

    setbuf(stdout, NULL);
    (void)argc;
    mr_vfs_init(NULL, argv[1]); mr_image_init(argv[1]); mr_scene_init();
    s = mr_script_load(argv[1]);
    mr_interp_init(s, argv[1]);
    mr_interp_start("start");
    mr_ui_init(argv[1]);
    g_shown = mr_surface_new(MR_STAGE_W, MR_STAGE_H);
    g_ref = mr_surface_new(MR_STAGE_W, MR_STAGE_H);

    /* Several walks, because the staleness depends on where a frame lands
     * relative to the typewriter. The reported case was "the first text
     * block after a skip", so the skip cadences are deliberately coprime
     * with the advance cadence -- a skip then stops at whatever point in a
     * line it happens to reach. */
    mr_var_set_int("_preferences.skip", 0);   /* stop at unread text */
    check("plain, 50 cps",        run(50, 0, 40), 0);
    check("plain, instant text",  run(0, 0, 40), 0);
    check("skipping, 50 cps",     run(50, 137, 40), 0);
    check("skipping, 30 cps",     run(30, 61, 27), 0);
    check("skipping, 200 cps",    run(200, 23, 31), 0);
    check("skipping, instant",    run(0, 43, 17), 0);
    /* Slow text is the worst case: a skip that stops early in a line
     * leaves most of it unrevealed, so a missed frame loses a whole block
     * rather than a glyph. */
    check("skipping, slow text",  run(12, 29, 53), 0);

    printf("\n%d failed\n", g_fail);
    return g_fail != 0;
}

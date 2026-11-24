/* The black fill before the background, and when it can be skipped.
 *
 * render_list cleared the whole stage to black and then, on most frames,
 * covered every pixel of it with an opaque full-stage background. MR_PROF is
 * what noticed: on every full-frame batch the `copy` and `fill` buckets
 * carried *identical* pixel and call counts -- 27,648kpx over 30 calls each,
 * 8,294kpx over 9 each -- which is 921,600 px at 20.7 ns/px, **19.1ms of a
 * ~151ms still frame** written and then thrown away (log-divfix.txt).
 *
 * So the fill is skipped when the background is about to cover it. The risk is
 * entirely one-sided: skip it when the background does *not* cover the stage
 * and whatever the previous frame left is still there, at the edges, through
 * the gaps, or under a translucent background. That is a stale-pixel bug, and
 * the worst kind -- it looks like a compositing error somewhere else.
 *
 * The oracle is therefore differential and whole-stage: render the same moment
 * with the skip available and again with it denied, and require every one of
 * the 921,600 pixels to match. Denying it is done by telling the engine the
 * background has an alpha channel, which is the condition the guard reads --
 * and for a source whose alpha is 255 throughout, mr_blit's two branches
 * produce identical pixels anyway (blendtest proves that exhaustively), so
 * the comparison stays honest rather than comparing two different blends.
 *
 * Then the three cases where the fill must still happen, each checked at a
 * pixel the background cannot reach.
 *
 * Runs on tools/harness/story. Drives mr_scene_* directly rather than through
 * the script: what is under test is render_list, and a label would add a
 * statement sequence between the test and the thing it tests.
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

static mr_surface *g_st;
static mr_surface *g_ref;
mr_surface *mr_stage(void) { return g_st; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); }
void mr_log(const char *f, ...) { (void)f; }
void mr_quit(void) {}
void mr_invalidate(void) {}
void hostshim_wave_sink(const short *d, int n) { (void)d; (void)n; }

static int fails;

static void check(const char *label, int ok)
{
    printf("%-58s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

static mr_u32 px(const mr_surface *s, int x, int y)
{
    return s->px[(size_t)y * s->pitch + x];
}

/* Paint `s` with a colour no composite would produce, so a pixel nobody wrote
 * is distinguishable from a pixel written black. Without this the "the fill
 * still happens" claims below would pass on a stage that was already black
 * from the previous render -- the reason they are checked against a poisoned
 * surface and not a fresh one. */
static void poison(mr_surface *s)
{
    int i;
    for (i = 0; i < s->h * s->pitch; i++)
        s->px[i] = 0xFFFF00FFUL;        /* magenta */
}

static long differs(const mr_surface *a, const mr_surface *b)
{
    int x, y;
    long n = 0;

    for (y = 0; y < MR_STAGE_H; y++)
        for (x = 0; x < MR_STAGE_W; x++)
            if (px(a, x, y) != px(b, x, y))
                n++;

    return n;
}

/* The guard reads has_alpha off the background's surface, so this is how the
 * skip is denied without touching the engine: ask the registry for the same
 * surface the renderer will use and flag it.
 *
 * **The same name the scene uses**, which is the whole trick and was wrong
 * first time round. `scene room` makes render_list call mr_image_get("room"),
 * and "room" and "room" are *separate registry entries* with
 * separate surfaces -- so poking the path left the rendered surface untouched,
 * the skip was never denied, and the differential claim compared two
 * identical renders. It passed, and so did two deliberate breaks. */
static int force_fill(const char *name, int on)
{
    mr_surface *s;

    mr_image_begin_frame();
    s = mr_image_get(name);
    if (!s)
        return 0;
    s->has_alpha = on;
    return 1;
}

int main(int argc, char **argv)
{
    mr_script *s;
    long bad;

    setbuf(stdout, NULL);
    if (argc < 2) {
        fprintf(stderr, "usage: filltest <game-dir>\n");
        return 2;
    }

    mr_vfs_init(NULL, argv[1]);
    mr_image_init(argv[1]);
    mr_scene_init();
    s = mr_script_load(argv[1]);
    mr_interp_init(s, argv[1]);
    mr_ui_init(argv[1]);

    /* Not optional, and not obvious: the init pass that executes the script's
     * `image room = "room"` statements, and mr_image_autodefine_dir
     * behind it, both run inside mr_interp_start -- not mr_interp_init. Without
     * this no image *name* resolves at all, only a literal file path, so
     * mr_scene_scene("room") drew nothing and the stage came out black.
     *
     * Which the differential claim below could not have noticed, because two
     * blank stages agree perfectly. What noticed was the "not an empty frame"
     * guard beside it, and that is the whole argument for pairing an
     * equality with a liveness check. */
    mr_interp_start("start");

    check("the script's image names resolve", mr_image_exists("room"));

    g_st  = mr_surface_new(MR_STAGE_W, MR_STAGE_H);
    g_ref = mr_surface_new(MR_STAGE_W, MR_STAGE_H);
    if (!g_st || !g_ref) {
        printf("could not allocate the stages\n");
        return 2;
    }

    /* --- the skip is invisible ------------------------------------------- */

    /* room.png is 1280x720 and has no alpha channel, so it covers the stage
     * and the fill can go. If the fixture's art ever stops being full-stage
     * this claim turns vacuous, so the covering is asserted first, off the
     * surface rather than assumed from the filename. */
    {
        mr_surface *bg;

        mr_image_begin_frame();
        bg = mr_image_get("room");
        check("the fixture's background covers the stage",
              bg && bg->w == MR_STAGE_W && bg->h == MR_STAGE_H);
        check("and the alpha scan found it opaque", bg && !bg->has_alpha);
    }

    mr_scene_scene("room");

    poison(g_st);
    mr_image_begin_frame();
    mr_image_set_time(0);
    mr_scene_render(g_st, 0);

    /* Same moment, fill forced. */
    if (!force_fill("room", 1)) {
        check("could reach the background surface to deny the skip", 0);
        return 1;
    }
    poison(g_ref);
    mr_image_set_time(0);
    mr_scene_render(g_ref, 0);
    force_fill("room", 0);

    bad = differs(g_st, g_ref);
    printf("   whole stage: %ld of %ld pixels differ\n",
           bad, (long)MR_STAGE_W * MR_STAGE_H);
    check("skipping the fill draws what filling would", bad == 0);

    /* And the stage really is the background rather than black -- a composite
     * that drew nothing at all would also match itself, so "the two agree" is
     * only worth something alongside "something was drawn".
     *
     * Counted over the whole stage rather than sampled at a corner, which is
     * where the first version of this looked and failed: the fixture's
     * room.png is legitimately near-black at 4,4, so a pixel test there was
     * asking about the art and not about the engine. */
    {
        long unwritten = 0, nonblack = 0;
        int x, y;

        for (y = 0; y < MR_STAGE_H; y++) {
            for (x = 0; x < MR_STAGE_W; x++) {
                mr_u32 v = px(g_st, x, y);
                if (v == 0xFFFF00FFUL)
                    unwritten++;
                else if (v != MR_RGB(0, 0, 0))
                    nonblack++;
            }
        }

        printf("   %ld pixels never written, %ld not black\n",
               unwritten, nonblack);
        check("every pixel of the stage was written", unwritten == 0);
        check("and the result is the background, not an empty frame",
              nonblack > 0);
    }

    /* --- and the three cases where the fill has to stay ------------------ */

    /* No background: the whole stage is the fill, and poisoning it first is
     * what makes that a claim rather than a coincidence. */
    mr_scene_scene("");
    poison(g_st);
    mr_image_begin_frame();
    mr_image_set_time(0);
    mr_scene_render(g_st, 0);
    check("with no background the stage is cleared to black",
          px(g_st, 0, 0) == MR_RGB(0, 0, 0) &&
          px(g_st, MR_STAGE_W - 1, MR_STAGE_H - 1) == MR_RGB(0, 0, 0) &&
          px(g_st, MR_STAGE_W / 2, MR_STAGE_H / 2) == MR_RGB(0, 0, 0));

    /* A background smaller than the stage. `band` is 64x64, so wherever the
     * placement puts it there is far more stage than image.
     *
     * And deliberately an *opaque* 64x64 -- zoomband.png is RGBA with every
     * alpha at 255, which the scan in png.c clears. That is what makes this
     * claim isolate the size test: with a translucent image the has_alpha
     * half of the guard would catch it too, and dropping the size test would
     * stay green. It did stay green, before this was asserted. */
    {
        mr_surface *bg;

        mr_image_begin_frame();
        bg = mr_image_get("band");
        check("the fixture has a background smaller than the stage",
              bg && (bg->w < MR_STAGE_W || bg->h < MR_STAGE_H));
        check("and an opaque one, so only the size test can refuse it",
              bg && !bg->has_alpha);
    }

    mr_scene_scene("band");
    poison(g_st);
    mr_image_begin_frame();
    mr_image_set_time(0);
    mr_scene_render(g_st, 0);
    check("a background smaller than the stage still gets the fill",
          px(g_st, 0, 0) == MR_RGB(0, 0, 0) &&
          px(g_st, MR_STAGE_W - 1, MR_STAGE_H - 1) == MR_RGB(0, 0, 0));

    /* A full-stage background that is genuinely translucent: black is what it
     * composites over, so the fill is part of the picture and not waste. The
     * witness is that the result differs from the same blend over magenta --
     * which is what skipping the fill would have left underneath. */
    mr_scene_scene("room");
    force_fill("room", 1);
    {
        mr_surface *bg;
        mr_u32 over_black, over_magenta;

        mr_image_begin_frame();
        bg = mr_image_get("room");
        if (bg) {
            /* Half-transparent, so the destination contributes. */
            int i;
            for (i = 0; i < bg->h * bg->pitch; i++)
                bg->px[i] = (bg->px[i] & 0x00FFFFFFUL) | 0x80000000UL;
        }

        poison(g_st);
        mr_image_set_time(0);
        mr_scene_render(g_st, 0);
        over_black = px(g_st, 4, 4);

        check("a translucent background is composited over black, not over"
              " what was there",
              over_black != 0xFFFF00FFUL);

        /* The same blend with the fill denied, to show the claim above is
         * about the fill and not about the blend: blit over the poison by
         * hand and the answer has to be different. */
        poison(g_ref);
        mr_blit(g_ref, bg, 0, 0);
        over_magenta = px(g_ref, 4, 4);

        check("and that is a different pixel from the same blend over the"
              " previous frame",
              over_black != over_magenta);

        /* The cached surface is left mutilated on purpose: mr_image_flush
         * must not be called from inside a frame (image.h), and this is the
         * last claim in the process, so there is nothing left to mislead.
         * Anything added after this needs the flush moved, not ignored. */
    }

    /* --- the extent test, which is the half that nearly went untested ----
     *
     * bg_covers_clip tests the background's origin *and* its extent, and
     * patching the extent out left every claim above green. The reasoning
     * that looked like it explained that: a background is placed by
     * mr_place_resolve with has_place = 0, which is bottom centre (xalign
     * 0.5, yalign 1.0), so x = (1280 - w) / 2 and y = 720 - h -- and an
     * origin at or below zero would then imply an image at least stage-sized
     * on that axis, hence an extent that reaches.
     *
     * That is **wrong**, and asserting it is what showed it. The placement
     * truncates: for a background one pixel *narrower* than the stage,
     * x = (int)(0.5f * 1) = 0, the origin test passes, and column 1279 is
     * never covered by anything. Without the extent test the fill would be
     * skipped and that column would keep the previous frame -- a one-pixel
     * stale stripe down the right edge, which is exactly the kind of fault
     * that gets blamed on the presenter for a week.
     *
     * No fixture image is 1279 wide, so the width is poked onto the cached
     * surface instead: the allocation is unchanged and only the reported
     * width shrinks, which is all the placement and the blit read.
     */
    {
        static const int sizes[][2] = {
            {  64,   64 }, { 300,  600 }, { 840, 2140 },
            { 1280, 720 }, { 1280, 1460 }, { 1920, 1080 },
            { 1279, 719 }, { 1281, 721 }
        };
        mr_place none;
        int i, counterexample = 0;

        memset(&none, 0, sizeof(none));

        for (i = 0; i < (int)(sizeof(sizes) / sizeof(sizes[0])); i++) {
            int w = sizes[i][0], h = sizes[i][1], bx, by;

            mr_place_resolve(&none, 0, w, h, &bx, &by);

            if ((bx <= 0 && bx + w < MR_STAGE_W) ||
                (by <= 0 && by + h < MR_STAGE_H))
                counterexample = 1;
        }

        /* Stated as "there is one" rather than "there is none", because that
         * is what is true and because the engine's extent test is load-
         * bearing on the strength of it. If placement ever changes so that
         * no size can do this, this claim fails and says so -- at which point
         * the extent test becomes redundant, not before. */
        check("some background size passes the origin test and still leaves"
              " a gap", counterexample);

        /* And the gap really is filled. 1279 wide, opaque, origin 0. */
        {
            mr_surface *bg;
            int real_w = 0;

            mr_scene_scene("room");
            mr_image_begin_frame();
            bg = mr_image_get("room");
            if (bg) {
                real_w = bg->w;
                bg->w = MR_STAGE_W - 1;
                bg->has_alpha = 0;
            }

            poison(g_st);
            mr_image_set_time(0);
            mr_scene_render(g_st, 0);

            check("a background one pixel narrow still gets the fill",
                  px(g_st, MR_STAGE_W - 1, 10) != 0xFFFF00FFUL);

            if (bg)
                bg->w = real_w;
        }
    }

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}

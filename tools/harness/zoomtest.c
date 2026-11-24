/* Transform `xzoom` / `yzoom`.
 *
 *     show azami magical pout full behind keiko:
 *         xzoom 0.5 yzoom 0.5
 *         xoffset 370.0
 *         yoffset 300.0
 *
 * Ren'Py's `add_property("xzoom", float, 1.0)` (display/transform.py:1392),
 * implemented at accelerator.pyx:555-585. What that code says, and what this
 * checks:
 *
 *   - no clamp, so a factor above 1 enlarges;
 *   - the scaled size is what the placement then works from (`width *=
 *     xzoom`), so a zoom on an aligned image moves it as well as resizing it;
 *   - a negative factor mirrors -- the size takes the absolute value and the
 *     sign stays in the matrix, which is what the im.Flip deprecation note
 *     means by "set xzoom to a negative value";
 *   - the filtering is the texture's, and both GL filters are GL_LINEAR
 *     (gl2texture.pyx:298-301), so bilinear in both directions.
 *
 * Measured off the stage, as a bounding box of everything that differs from
 * the empty stage. That makes every assertion a statement about where the
 * pixels landed rather than about what the place struct holds, and it does
 * not care how the engine chooses to store a zoom.
 *
 * Runs on tools/harness/story, label zoom_show.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
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

/* The empty stage, captured at the first line of the label. */
static mr_u32 *g_ref;

/* The band the bounding box is taken over. The fixture parks everything
 * between y 100 and y 500, which is clear of the say window and of the
 * speaker's name -- the two things that change from line to line and would
 * otherwise be measured as part of the sprite. */
#define BAND_Y0 0
#define BAND_Y1 520

typedef struct { int x0, y0, x1, y1; } box;

static void render(void)
{
    mr_scene_render(g_st, T);
}

static mr_u32 pix(int x, int y)
{
    render();
    return g_st->px[(size_t)y * g_st->pitch + x];
}

/* The bounding box of every pixel that differs from the empty stage, within
 * the column range [cx0, cx1), in inclusive coordinates. x1 < x0 means
 * nothing is drawn there. */
static box bbox_in(int cx0, int cx1)
{
    box b;
    int x, y;

    b.x0 = b.y0 = 1 << 20;
    b.x1 = b.y1 = -1;

    render();
    for (y = BAND_Y0; y < BAND_Y1; y++) {
        const mr_u32 *row = g_st->px + (size_t)y * g_st->pitch;
        const mr_u32 *ref = g_ref + (size_t)y * MR_STAGE_W;

        for (x = cx0; x < cx1; x++) {
            if (row[x] == ref[x])
                continue;
            if (x < b.x0) b.x0 = x;
            if (x > b.x1) b.x1 = x;
            if (y < b.y0) b.y0 = y;
            if (y > b.y1) b.y1 = y;
        }
    }
    return b;
}

static box bbox(void) { return bbox_in(0, MR_STAGE_W); }

static int box_w(box b) { return b.x1 < b.x0 ? 0 : b.x1 - b.x0 + 1; }
static int box_h(box b) { return b.y1 < b.y0 ? 0 : b.y1 - b.y0 + 1; }

/* Within one pixel. A zoom rounds a fractional size to whole pixels and the
 * bilinear edge can carry a nearly-invisible extra column, neither of which
 * is the thing under test. */
static void check_near(const char *label, int got, int want)
{
    int ok = (got >= want - 1 && got <= want + 1);

    printf("%-52s %4d  want %4d  %s\n", label, got, want,
           ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

static void check_between(const char *label, int got, int lo, int hi)
{
    int ok = (got > lo && got < hi);

    printf("%-52s %4d  want %d..%d  %s\n", label, got, lo, hi,
           ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

static void check(const char *label, int ok)
{
    printf("%-52s       %s\n", label, ok ? "ok" : "** FAIL **");
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
    printf("never reached '%s'\n", want);
    fails++;
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

static void show_box(const char *label, box b)
{
    if (b.x1 < b.x0) {
        printf("  %-30s nothing drawn\n", label);
        return;
    }
    printf("  %-30s x %d..%d (%d)  y %d..%d (%d)\n", label,
           b.x0, b.x1, box_w(b), b.y0, b.y1, box_h(b));
}

/* Reach a line, let the frame settle, and assert the drawn size. The two
 * dimensions are separate assertions because a zoom that landed on the wrong
 * axis is the mistake worth naming. */
static box check_size(const char *line, const char *what, int w, int h)
{
    char lab[96];
    box b;

    if (!reach(line)) {
        b.x0 = b.y0 = 0;
        b.x1 = b.y1 = -1;
        return b;
    }
    settle(300);
    b = bbox();
    show_box(what, b);
    sprintf(lab, "%s: width", what);
    check_near(lab, box_w(b), w);
    sprintf(lab, "%s: height", what);
    check_near(lab, box_h(b), h);
    return b;
}

int main(int argc, char **argv)
{
    mr_script *s;
    box plain, half, tall, offset, twice, centred, mirror, big;
    box zbase, zprod;
    mr_u32 band_top, band_bot, q_tl, q_tr, q_bl, q_br;
    int y;

    setbuf(stdout, NULL);
    if (argc < 2) {
        fprintf(stderr, "usage: zoomtest <story-dir>\n");
        return 2;
    }

    mr_vfs_init(NULL, argv[1]);
    mr_image_init(argv[1]);
    mr_scene_init();
    s = mr_script_load(argv[1]);
    mr_interp_init(s, argv[1]);
    mr_ui_init(argv[1]);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);
    g_ref = (mr_u32 *)malloc((size_t)MR_STAGE_W * MR_STAGE_H * sizeof(mr_u32));
    if (!g_ref)
        return 1;
    mr_interp_start("zoom_show");

    reach("Empty stage.");
    settle(300);
    render();
    for (y = 0; y < MR_STAGE_H; y++)
        memcpy(g_ref + (size_t)y * MR_STAGE_W,
               g_st->px + (size_t)y * g_st->pitch,
               (size_t)MR_STAGE_W * sizeof(mr_u32));

    /* The unzoomed sprite, which every other measurement is relative to. The
     * fixture is 64x64 with its right half fully transparent, so the box is
     * the opaque left half: 32 wide, 64 tall, at the transform's offsets. */
    reach("Unzoomed.");
    settle(300);
    plain = bbox();
    show_box("fringe at zoomspot", plain);
    if (box_w(plain) == 0) {
        printf("fixture is useless: nothing is drawn\n");
        return 1;
    }
    check_near("unzoomed left edge is the xoffset", plain.x0, 400);
    check_near("unzoomed top edge is the yoffset", plain.y0, 100);

    reach("Half as wide.");
    settle(300);
    half = bbox();
    show_box("xzoom 0.5", half);
    check_near("xzoom 0.5 halves the width", box_w(half), box_w(plain) / 2);
    check_near("xzoom 0.5 leaves the height", box_h(half), box_h(plain));
    check_near("xzoom 0.5 leaves the left edge", half.x0, plain.x0);

    reach("Half as tall.");
    settle(300);
    tall = bbox();
    show_box("yzoom 0.5", tall);
    check_near("yzoom 0.5 halves the height", box_h(tall), box_h(plain) / 2);
    check_near("yzoom 0.5 leaves the width", box_w(tall), box_w(plain));

    /* The reported shape. The offsets are screen pixels: they are applied
     * after the resample and are not themselves scaled. */
    reach("Half size, offset.");
    settle(300);
    offset = bbox();
    show_box("both halved, xoffset 370 yoffset 300", offset);
    check_near("the xoffset is not scaled by the zoom", offset.x0, 370);
    check_near("the yoffset is not scaled by the zoom", offset.y0, 300);
    check_near("both axes halved: width", box_w(offset), box_w(plain) / 2);
    check_near("both axes halved: height", box_h(offset), box_h(plain) / 2);

    reach("Twice as wide.");
    settle(300);
    twice = bbox();
    show_box("xzoom 2.0", twice);
    check_near("xzoom 2.0 doubles the width", box_w(twice), box_w(plain) * 2);
    check_near("xzoom 2.0 leaves the height", box_h(twice), box_h(plain));

    /* A re-show that changes only the picture must not throw the zoom away:
     * it is transform state like any other (config.keep_running_transform /
     * take_state), and the same rule alphatest checks for alpha. */
    reach("Still twice as wide.");
    settle(300);
    show_box("after `show fringe other`", bbox());
    check_near("a re-show that says nothing keeps the zoom",
               box_w(bbox()), box_w(plain) * 2);

    /* The placement has to be resolved from the scaled size. A 64-wide image
     * at xalign 0.5 has its left edge at (1280-64)/2 = 608; halved, it is 32
     * wide and the edge is at 624. Measured on the *image*, so the opaque
     * half's left edge is the image's left edge. */
    reach("Centred and halved.");
    settle(300);
    centred = bbox();
    show_box("xalign 0.5 with xzoom 0.5", centred);
    check_near("an aligned image is placed at its scaled size",
               centred.x0, 624);

    /* Negative: the opaque half moves from the left of the image to the
     * right. The image occupies x 400..464 either way, so this is a box that
     * starts at 432 instead of 400 and is the same size. */
    reach("Mirrored.");
    settle(300);
    mirror = bbox();
    show_box("xzoom -1.0", mirror);
    check_near("xzoom -1.0 keeps the width", box_w(mirror), box_w(plain));
    check_near("xzoom -1.0 mirrors: the opaque half moves right",
               mirror.x0, plain.x0 + box_w(plain));

    /* The interpolation. Two separate claims on one frame.
     *
     * First, that there is any interpolation at all: a nearest-neighbour
     * resample of a hard alpha edge produces no partly transparent pixel, so
     * the column where the edge lands is either background or sprite and
     * nothing between.
     *
     * Second, that the resample weights each colour by its own alpha. The
     * fixture's transparent half is bright orange, so a resample that
     * interpolates straight colour drags red into the green edge; with
     * premultiplication the edge stays green and only its alpha falls. */
    reach("Doubled.");
    settle(300);
    big = bbox();
    show_box("xzoom 2.0 yzoom 2.0", big);
    check_near("both axes doubled: width", box_w(big), box_w(plain) * 2);
    check_near("both axes doubled: height", box_h(big), box_h(plain) * 2);
    {
        int mid = big.y0 + box_h(big) / 2;
        int x, mixed = 0, orange = 0;
        mr_u32 bg, sprite;

        render();
        bg = g_ref[(size_t)mid * MR_STAGE_W + big.x1 + 4];
        sprite = g_st->px[(size_t)mid * g_st->pitch + big.x0 + 4];

        printf("  edge row %d: bg (%d,%d,%d) sprite (%d,%d,%d)\n", mid,
               MR_R(bg), MR_G(bg), MR_B(bg),
               MR_R(sprite), MR_G(sprite), MR_B(sprite));
        printf("  across the edge:");
        for (x = big.x1 - 3; x <= big.x1 + 3; x++) {
            mr_u32 v = g_st->px[(size_t)mid * g_st->pitch + x];
            printf(" (%d,%d,%d)", MR_R(v), MR_G(v), MR_B(v));
        }
        printf("\n");

        for (x = big.x0; x <= big.x1 + 4 && x < MR_STAGE_W; x++) {
            mr_u32 v = g_st->px[(size_t)mid * g_st->pitch + x];

            if (v == sprite || v == g_ref[(size_t)mid * MR_STAGE_W + x])
                continue;
            mixed++;
            /* Green is the opaque half; the clear half is orange. Red
             * overtaking green on an edge pixel is the colour of the
             * invisible pixels bleeding through. */
            if (MR_R(v) >= MR_G(v))
                orange++;
        }
        check("the resampled edge is interpolated, not stepped", mixed > 0);
        check("no colour bleeds out of the transparent half", orange == 0);
    }

    /* The timed step, sampled while it runs. */
    reach("Shrinking.");
    settle(100);
    {
        int w0 = box_w(bbox());
        int i;

        check_between("a timed xzoom is part-way through", w0,
                      box_w(plain) / 2, box_w(plain) + 1);
        printf("  widths:");
        for (i = 0; i < 6; i++) {
            printf(" %d", box_w(bbox()));
            settle(100);
        }
        printf(" %d\n", box_w(bbox()));
        check_near("a timed xzoom arrives at its target",
                   box_w(bbox()), box_w(plain) / 2);
    }

    /* --- the combined `zoom` -------------------------------------------- */

    /* A third factor that multiplies into both axes (transform.py:1382,
     * accelerator.pyx:555-557), and separate state from the per-axis two.
     *
     * The sizes are all expressed against `zbase`, the same fixture with
     * every factor at 1.0, rather than against numbers written down here. */
    zbase = check_size("Zoom reset.", "every factor 1.0",
                       box_w(plain), box_h(plain));

    check_size("Zoom halved.", "zoom 0.5",
               box_w(zbase) / 2, box_h(zbase) / 2);
    check_size("Zoom doubled.", "zoom 2.0",
               box_w(zbase) * 2, box_h(zbase) * 2);
    check_size("Zoom still doubled.", "after `show fringe other`",
               box_w(zbase) * 2, box_h(zbase) * 2);

    /* The product. `zoom 2.0` is standing and this statement says only
     * `xzoom 0.5`, so x is 1.0 and y is still 2.0.
     *
     * This is the case that fails if `zoom` is folded into the per-axis
     * factors when it is parsed: the folded version has this statement
     * *replace* the 2.0 on x rather than multiply with it, giving 0.5 and
     * half the width instead of the full one. */
    zprod = check_size("Zoom times xzoom.", "zoom 2.0 then xzoom 0.5",
                       box_w(zbase), box_h(zbase) * 2);

    /* The same two numbers in the other order. */
    check_size("Only xzoom.", "xzoom 0.5 alone",
               box_w(zbase) / 2, box_h(zbase));
    {
        box b = check_size("Then zoom.", "xzoom 0.5 then zoom 2.0",
                           box_w(zbase), box_h(zbase) * 2);

        check("the two factors are order independent",
              box_w(b) == box_w(zprod) && box_h(b) == box_h(zprod));
    }

    reach("Zoom zero.");
    settle(300);
    show_box("zoom 0.0", bbox());
    check("zoom 0.0 draws nothing at all", box_w(bbox()) == 0);

    /* Aligned, so the placement is resolved from the scaled size -- the same
     * claim as the xzoom case above, reached through the other property. */
    reach("Zoom centred.");
    settle(300);
    {
        box b = bbox();

        show_box("xalign 0.5 with zoom 0.5", b);
        check_near("an aligned image is placed at its zoomed size",
                   b.x0, 624);
    }

    /* Timed, with an xzoom standing. The step interpolates `zoom` itself, so
     * x travels from 0.5 to 0.25 of the base and never through 0.75 -- which
     * is what it would do if the step wrote the product. */
    reach("Zoom shrinking.");
    settle(100);
    {
        box b = bbox();
        int i;

        show_box("mid-flight", b);
        check_between("a timed zoom is part-way through on x",
                      box_w(b), box_w(zbase) / 4 - 1, box_w(zbase) / 2 + 1);
        check_between("a timed zoom is part-way through on y",
                      box_h(b), box_h(zbase) / 2 - 1, box_h(zbase) + 1);
        printf("  widths:");
        for (i = 0; i < 6; i++) {
            printf(" %d", box_w(bbox()));
            settle(100);
        }
        printf(" %d\n", box_w(bbox()));
        b = bbox();
        check_near("a timed zoom arrives at zoom * xzoom on x",
                   box_w(b), box_w(zbase) / 4);
        check_near("a timed zoom arrives at zoom * yzoom on y",
                   box_h(b), box_h(zbase) / 2);
    }

    /* --- negative factors, as pixels ------------------------------------ */

    /* At |zoom| == 1 the resample is an exact copy -- every sampling fraction
     * is zero -- so these are equality checks on whole pixels rather than
     * anything approximate. The reference colours are read off the upright
     * image so the fixtures can be regenerated.
     *
     * `band` differs by row, so this says yzoom -1.0 flips vertically and not
     * horizontally: a horizontal flip of it would change nothing. */
    reach("Band upright.");
    settle(300);
    band_top = pix(420, 110);
    band_bot = pix(420, 150);
    printf("  band upright: top (%d,%d,%d) bottom (%d,%d,%d)\n",
           MR_R(band_top), MR_G(band_top), MR_B(band_top),
           MR_R(band_bot), MR_G(band_bot), MR_B(band_bot));
    check("the band fixture has two different halves", band_top != band_bot);

    reach("Band flipped.");
    settle(300);
    check("yzoom -1.0 puts the bottom half on top", pix(420, 110) == band_bot);
    check("yzoom -1.0 puts the top half at the bottom",
          pix(420, 150) == band_top);

    /* And the mirror on the other axis, on a fixture that differs by column
     * *and* by row, so that a flip on the wrong axis is a visible failure
     * rather than an invisible one. */
    reach("Quad upright.");
    settle(300);
    q_tl = pix(410, 110);
    q_tr = pix(450, 110);
    q_bl = pix(410, 150);
    q_br = pix(450, 150);
    printf("  quad upright: tl/tr (%d,%d,%d)/(%d,%d,%d)\n",
           MR_R(q_tl), MR_G(q_tl), MR_B(q_tl),
           MR_R(q_tr), MR_G(q_tr), MR_B(q_tr));
    check("the quad fixture has two different columns", q_tl != q_tr);

    reach("Quad mirrored.");
    settle(300);
    check("xzoom -1.0 swaps the top corners",
          pix(410, 110) == q_tr && pix(450, 110) == q_tl);
    check("xzoom -1.0 swaps the bottom corners",
          pix(410, 150) == q_br && pix(450, 150) == q_bl);

    /* Both signs at once. The quadrants come back to where they started,
     * which is the case that fails if either sign is dropped: one flip alone
     * leaves the diagonals swapped. */
    reach("Quad flipped twice.");
    settle(300);
    check("both axes negative restores the upright image",
          pix(410, 110) == q_tl && pix(450, 110) == q_tr &&
          pix(410, 150) == q_bl && pix(450, 150) == q_br);

    /* `zoom` carries a sign too, and the signs multiply:
     * `zoom -1.0 xzoom -1.0 yzoom 1.0` is +1 on x and -1 on y.
     *
     * Two fixtures, the same three numbers, because one picture cannot say
     * which axis moved: `band` differs by row and shows the y flip
     * happening, `fringe` differs by column and shows the x flip *not*
     * happening. An implementation where any negative mirrors both axes
     * passes the first and fails the second. */
    reach("Band two negatives.");
    settle(300);
    check("zoom -1.0 times yzoom 1.0 still flips y",
          pix(420, 110) == band_bot && pix(420, 150) == band_top);

    reach("Fringe two negatives.");
    settle(300);
    {
        box b = bbox();

        show_box("zoom -1.0 with xzoom -1.0", b);
        check_near("zoom -1.0 times xzoom -1.0 does not flip x",
                   b.x0, plain.x0);
        check_near("the two negatives leave the width alone",
                   box_w(b), box_w(plain));
    }

    /* --- the zoomed-copy cache ------------------------------------------ */

    /* Eight zoomed copies live in one frame, against six slots: the cache
     * runs out, every slot is pinned by the frame in progress, and the next
     * arrival takes one anyway. The claim is that nothing is served from the
     * wrong slot -- which would show up as a sprite drawn at another
     * sprite's size, or in another sprite's colours.
     *
     * zc5 and zc6 are the pair that pins the key: both resample to 96x96, so
     * a cache that keyed on the size alone would hand one of them the other's
     * pixels and the sizes would still look right.
     *
     * Measured twice, with an oversized copy shown in between -- see below --
     * so the second pass is after a round of eviction rather than being the
     * same six slots still warm. */
    {
        /* Two of the three numbers are derived rather than written down: the
         * fixtures are 64x64, `fringe`'s visible half is the left 32 columns,
         * and `band` is opaque to its edges. */
        static const float f[8] = { 0.25f, 0.5f, 0.75f, 1.25f,
                                    1.5f, 1.5f, 1.75f, 2.0f };
        int pass;

        for (pass = 0; pass < 2; pass++) {
            int i;

            if (pass == 0) {
                reach("Eight at once.");
            } else {
                /* The oversized one: 1280x1460 at 0.7 x 2.0 is 896x2920,
                 * about 10MB against the cache's 8MB budget. It is served and
                 * does not stay resident, the same deal mr_image_get gives an
                 * image larger than its own budget -- and taking it evicts
                 * the eight, which is the path being tested. 896 wide also
                 * leaves the right of the stage clear, so the x scale is
                 * visible rather than being clipped away. */
                reach("One huge one.");
                settle(100);
                check("an oversized zoom is still drawn, at its own width",
                      pix(880, 300) != g_ref[(size_t)300 * MR_STAGE_W + 880] &&
                      pix(910, 300) == g_ref[(size_t)300 * MR_STAGE_W + 910]);

                reach("Eight again.");
            }
            settle(300);

            printf("  pass %d:\n", pass);
            for (i = 0; i < 8; i++) {
                int x0 = 40 + i * 150;
                int x1 = (i == 7) ? MR_STAGE_W : x0 + 150;
                /* The even slots are `fringe`, whose visible half is 32 of
                 * its 64 columns; the odd ones are `band`, opaque throughout.
                 */
                int src_w = (i & 1) ? 64 : 32;
                box b = bbox_in(x0, x1);
                char label[64];

                printf("    zc%d (x%.2f)", i + 1, f[i]);
                show_box("", b);

                sprintf(label, "zc%d is %d wide", i + 1,
                        (int)(src_w * f[i] + 0.5f));
                check_near(label, box_w(b), (int)(src_w * f[i] + 0.5f));
                sprintf(label, "zc%d is %d tall", i + 1,
                        (int)(64 * f[i] + 0.5f));
                check_near(label, box_h(b), (int)(64 * f[i] + 0.5f));
                sprintf(label, "zc%d starts at its own offset", i + 1);
                check_near(label, b.x0, 50 + i * 150);
            }

            /* The same-size pair, by colour. zc5 is the green fixture and zc6
             * the magenta one; both are 96x96 zoomed. */
            {
                mr_u32 a = pix(660, 110), b = pix(810, 110);

                printf("    zc5 (%d,%d,%d)  zc6 (%d,%d,%d)\n",
                       MR_R(a), MR_G(a), MR_B(a), MR_R(b), MR_G(b), MR_B(b));
                check("two names at the same zoomed size keep their own "
                      "pixels", a != b);
            }
        }
    }

    free(g_ref);
    mr_surface_free(g_st);
    mr_scene_shutdown();
    mr_image_shutdown();
    mr_vfs_shutdown();
    mr_script_free(s);

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}

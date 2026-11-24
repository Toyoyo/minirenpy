/* im.Blur: our blur against Ren'Py's own, and the declarations that reach it.
 *
 * Reported from a second game's effects block:
 *
 *     image bg rebouchon table blurred = im.Blur("bg rebouchon table.png", 2.0)
 *     image bg karaoke blurred         = im.Blur("bg karaoke.png", 2.0)
 *     image bg street night blurred    = im.Blur("bg street night.png", 2.0)
 *
 * im.Blur is a *decode-time* manipulator, like the im.Flip the first game
 * used: it is part of what a name means, not something a statement does. So
 * there are two separable claims here, and they fail in different ways.
 *
 *   1. The pixels. mr_surface_blur must produce what Ren'Py produces. The
 *      oracle is Ren'Py's blur_filters/blur32_core/linblur32_core, copied out
 *      of its src/core.c into blurref.c and run side by side -- so this is
 *      byte-for-byte equality, not a tolerance. A reference I wrote myself
 *      from the Kovesi paper would only prove I can make the same mistake
 *      twice; the almost-Gaussian is three box blurs with widths from a
 *      formula, and getting the widths or the pass split wrong changes the
 *      result without changing the fact that it looks blurred.
 *
 *   2. The declaration. `im.Blur("f.png", 2.0)` has to parse, the radius has
 *      to survive to mr_image_define_manip, and the blur has to be applied to
 *      the decoded surface -- after a nested im.Flip, not before.
 *
 * The fixture for (2) is images/quad.png, four quadrants with a hard edge on
 * both axes. The existing story fixtures are flat or two flat bands, and
 * blurring a flat image returns the flat image: on those, every assertion
 * here would pass with the blur ripped out entirely. That is the same trap
 * that let the backdrop pan ship wrong twice, so the negative controls below
 * are run against the structured image on purpose.
 *
 * Runs on tools/harness/story.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mr.h"
#include "interp.h"
#include "vfs.h"
#include "script.h"
#include "image.h"
#include "scene.h"
#include "ui.h"
#include "blurref.h"

static mr_surface *g_st;
mr_surface *mr_stage(void) { return g_st; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); exit(1); }
void mr_log(const char *f, ...) { (void)f; }
void mr_quit(void) {}
void mr_invalidate(void) {}
void hostshim_wave_sink(const short *d, int n) { (void)d; (void)n; }

static int fails;

static void check(const char *label, int ok)
{
    printf("  %-60s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

/* --- fixtures ------------------------------------------------------------ */

/* Structure on both axes and in every channel, with hard edges rather than a
 * gradient: a box blur of a gradient is very nearly the gradient again, so a
 * wrong radius barely shows. The alpha channel is given its own pattern
 * because im.Blur blurs alpha too (core.c blurs all four bytes) and a blur
 * that quietly left alpha alone would otherwise pass. */
static mr_surface *make_src(int w, int h)
{
    mr_surface *s = mr_surface_new(w, h);
    int x, y;

    if (!s)
        return NULL;
    for (y = 0; y < h; y++) {
        mr_u32 *row = s->px + (size_t)y * s->pitch;
        for (x = 0; x < w; x++) {
            int r = (x < w / 2) ? 10 : 250;
            int g = (y < h / 3) ? 200 : 30;
            int b = ((x / 7) ^ (y / 5)) & 1 ? 240 : 15;
            int a = (x + y) < (w + h) / 2 ? 255 : 64;
            row[x] = MR_ARGB(a, r, g, b);
        }
    }
    s->has_alpha = 1;
    return s;
}

static mr_surface *clone(const mr_surface *src)
{
    mr_surface *s;
    int y;

    /* mr_image_get returns NULL for a name it cannot resolve, and a blurred
     * name that failed to register would arrive here as one. Saying so beats
     * a segfault that looks like a blur bug. */
    if (!src)
        mr_fatal("clone of a NULL surface -- an image name did not resolve");
    s = mr_surface_new(src->w, src->h);

    if (!s)
        return NULL;
    for (y = 0; y < src->h; y++)
        memcpy(s->px + (size_t)y * s->pitch, src->px + (size_t)y * src->pitch,
               (size_t)src->w * sizeof(mr_u32));
    s->has_alpha = src->has_alpha;
    return s;
}

static int same(const mr_surface *a, const mr_surface *b)
{
    int y;

    if (a->w != b->w || a->h != b->h)
        return 0;
    for (y = 0; y < a->h; y++) {
        if (memcmp(a->px + (size_t)y * a->pitch, b->px + (size_t)y * b->pitch,
                   (size_t)a->w * sizeof(mr_u32)))
            return 0;
    }
    return 1;
}

/* Where the two first disagree, for a failure that says something. */
static void report_diff(const mr_surface *a, const mr_surface *b)
{
    int x, y;

    for (y = 0; y < a->h; y++) {
        for (x = 0; x < a->w; x++) {
            mr_u32 p = a->px[(size_t)y * a->pitch + x];
            mr_u32 q = b->px[(size_t)y * b->pitch + x];
            if (p != q) {
                printf("     first difference at (%d,%d): "
                       "ours %02x%02x%02x%02x ref %02x%02x%02x%02x\n",
                       x, y, MR_A(p), MR_R(p), MR_G(p), MR_B(p),
                       MR_A(q), MR_R(q), MR_G(q), MR_B(q));
                return;
            }
        }
    }
}

/* Ren'Py's blur over a copy of the same pixels. core.c wants three surfaces
 * of equal size and leaves the result in the third. */
static mr_surface *ref_blur_copy(const mr_surface *src, float xrad, float yrad)
{
    mr_surface *s = clone(src);
    mr_surface *wrk = mr_surface_new(src->w, src->h);
    mr_surface *dst = mr_surface_new(src->w, src->h);
    refsurf rs, rw, rd;

    if (!s || !wrk || !dst)
        mr_fatal("out of memory");

    rs.w = s->w;   rs.h = s->h;   rs.pitch = s->pitch * 4;   rs.pixels = s->px;
    rw.w = wrk->w; rw.h = wrk->h; rw.pitch = wrk->pitch * 4; rw.pixels = wrk->px;
    rd.w = dst->w; rd.h = dst->h; rd.pitch = dst->pitch * 4; rd.pixels = dst->px;

    ref_blur32(&rs, &rw, &rd, xrad, yrad);

    mr_surface_free(s);
    mr_surface_free(wrk);
    return dst;
}

/* --- (1) the pixels ------------------------------------------------------ */

static void agrees_with_renpy(int w, int h, float xrad, float yrad)
{
    mr_surface *src = make_src(w, h);
    mr_surface *ours = clone(src);
    mr_surface *ref = ref_blur_copy(src, xrad, yrad);
    char label[80];
    int ok;

    mr_surface_blur(ours, xrad, yrad);
    ok = same(ours, ref);

    sprintf(label, "%dx%d, xrad %.1f yrad %.1f matches Ren'Py exactly",
            w, h, (double)xrad, (double)yrad);
    check(label, ok);
    if (!ok)
        report_diff(ours, ref);

    mr_surface_free(src);
    mr_surface_free(ours);
    mr_surface_free(ref);
}

/* The assertions above are equality against a reference that could in
 * principle agree with us by both doing nothing. These say the blur is not
 * nothing, and say it on the same kind of pixels. */
static void blur_actually_blurs(void)
{
    mr_surface *src = make_src(64, 48);
    mr_surface *s = clone(src);
    mr_surface *flat;
    int x, y, differs = 0;

    mr_surface_blur(s, 2.0f, 2.0f);
    for (y = 0; y < src->h; y++)
        for (x = 0; x < src->w; x++)
            if (s->px[(size_t)y * s->pitch + x] != src->px[(size_t)y * src->pitch + x])
                differs++;

    /* A 2.0 blur over an image with a hard edge every few pixels should move
     * most of it. "Some pixel changed" would be satisfied by a one-row bug. */
    check("a 2.0 blur changes most of a structured image",
          differs > src->w * src->h * 3 / 4);
    mr_surface_free(s);

    /* Radius zero is a copy, not a crash and not a darkening: the accumulator
     * divides by 2*0+1 and the edge-extension terms vanish. */
    s = clone(src);
    mr_surface_blur(s, 0.0f, 0.0f);
    check("radius 0 leaves the image alone", same(s, src));
    mr_surface_free(s);

    /* And the control that proves the previous assertions are not passing by
     * accident on flat input: on a flat image the blur is a no-op, which is
     * exactly why the story fixture needed quad.png. */
    flat = mr_surface_new(64, 48);
    mr_surface_fill(flat, MR_ARGB(255, 90, 90, 90));
    s = clone(flat);
    mr_surface_blur(s, 4.0f, 4.0f);
    check("a flat image blurs to itself (so a flat fixture proves nothing)",
          same(s, flat));
    mr_surface_free(s);
    mr_surface_free(flat);
    mr_surface_free(src);
}

/* The edges. Ren'Py extends the line by repeating its first and last pixel,
 * so the corner of an opaque image stays opaque and a bright edge does not
 * fade into black -- getting this wrong gives a dark or transparent frame
 * around every blurred background, which is both obvious and easy to ship. */
static void edges_are_extended(void)
{
    mr_surface *s = mr_surface_new(32, 32);
    mr_u32 p;

    mr_surface_fill(s, MR_ARGB(255, 255, 255, 255));
    mr_surface_blur(s, 3.0f, 3.0f);

    p = s->px[0];
    check("the corner of a white opaque image stays white after a blur",
          MR_R(p) == 255 && MR_G(p) == 255 && MR_B(p) == 255 && MR_A(p) == 255);
    mr_surface_free(s);
}

/* A sprite can be narrower than the kernel. Ren'Py would walk off the end of
 * such a line; we clamp the radius instead, which is a deliberate divergence
 * and the only one in this file. Nothing in a game hits it -- the radii come
 * from a formula on a full-size image -- but a 1px surface is not a reason to
 * fault on Win32s. */
static void tiny_surfaces_survive(void)
{
    int sizes[] = { 1, 2, 3, 5 };
    int i;

    for (i = 0; i < 4; i++) {
        mr_surface *s = make_src(sizes[i], sizes[i]);
        mr_u32 before = s->px[0];
        int ok;

        mr_surface_blur(s, 8.0f, 8.0f);
        /* Finite and in range is the whole claim; the pixels are whatever the
         * clamped radius gives. */
        ok = MR_A(s->px[0]) <= 255;
        (void)before;
        check(sizes[i] == 1 ? "a 1x1 surface blurs without faulting" :
              sizes[i] == 2 ? "a 2x2 surface blurs without faulting" :
              sizes[i] == 3 ? "a 3x3 surface blurs without faulting" :
                              "a 5x5 surface blurs without faulting", ok);
        mr_surface_free(s);
    }
}

/* has_alpha is load-bearing for the blit path, and a background is the thing
 * a game blurs. An opaque image must stay flagged opaque. */
static void opaque_stays_opaque(void)
{
    mr_surface *s = mr_surface_new(40, 40);

    mr_surface_fill(s, MR_ARGB(255, 20, 40, 60));
    s->has_alpha = 0;
    mr_surface_blur(s, 2.0f, 2.0f);
    check("a blurred opaque surface keeps the fast blit path", !s->has_alpha);
    mr_surface_free(s);
}

/* --- (2) the declaration ------------------------------------------------- */

static mr_surface *png_or_die(const char *path)
{
    mr_surface *s = mr_png_load(path);
    if (!s)
        mr_fatal("cannot load %s", path);
    return s;
}

static void declared_image_is_blurred(mr_script *s)
{
    mr_surface *raw;
    mr_surface *want;
    mr_surface *got;
    int i, nblur = 0;

    /* What the parser made of the three declarations, before any pixels: a
     * radius that never arrives and a blur that never runs look identical in
     * the output. */
    printf("\n-- what the parser made of each im.Blur --\n");
    for (i = 0; i < s->n; i++) {
        if (s->st[i].kind != MR_ST_IMAGE)
            continue;
        if (s->st[i].blur_x == 0.0f && s->st[i].blur_y == 0.0f)
            continue;
        nblur++;
        printf("   %-16s %-22s flip=%d xrad=%.1f yrad=%.1f\n",
               s->st[i].name, s->st[i].text, s->st[i].aux,
               (double)s->st[i].blur_x, (double)s->st[i].blur_y);
    }
    check("three im.Blur declarations parsed with a radius", nblur == 3);

    for (i = 0; i < s->n; i++) {
        mr_stmt *st = &s->st[i];
        if (st->kind != MR_ST_IMAGE)
            continue;
        if (!strcmp(st->name, "quad blurred"))
            check("one radius means a circular kernel (yrad = xrad)",
                  st->blur_x == 2.0f && st->blur_y == 2.0f && st->aux == 0);
        else if (!strcmp(st->name, "quad ellipse"))
            check("two radii are kept as two axes",
                  st->blur_x == 1.0f && st->blur_y == 6.0f);
        else if (!strcmp(st->name, "quad flipblur"))
            check("a nested im.Flip still flips, and the radius is found "
                  "past it", st->blur_x == 2.0f && st->aux == 1);
    }

    /* And the pixels the name actually resolves to. */
    printf("\n-- the pixels each name resolves to --\n");

    mr_image_begin_frame();
    raw = png_or_die("images/quad.png");

    want = ref_blur_copy(raw, 2.0f, 2.0f);
    got = clone(mr_image_get("quad blurred"));
    check("`quad blurred` is quad.png blurred by 2.0", same(got, want));
    if (!same(got, want))
        report_diff(got, want);
    mr_surface_free(want);
    mr_surface_free(got);

    /* The unblurred name is the control: same file, no manipulator. If the
     * radius were being applied to every image, or to none, exactly one of
     * these two assertions would still pass. */
    got = clone(mr_image_get("quad"));
    check("`quad` is not blurred", same(got, raw));
    mr_surface_free(got);

    want = ref_blur_copy(raw, 1.0f, 6.0f);
    got = clone(mr_image_get("quad ellipse"));
    check("`quad ellipse` uses 1.0 on x and 6.0 on y", same(got, want));
    if (!same(got, want))
        report_diff(got, want);
    mr_surface_free(want);
    mr_surface_free(got);

    /* The nested im.Flip. This asserts that both manipulators ran -- it
     * fails if either is dropped -- but *not* that they ran in Ren'Py's
     * order, and it cannot: a mirror and a box blur commute exactly. The box
     * widths are always odd (wl is forced odd and wu = wl + 2), so the kernel
     * is symmetric, and so is the edge extension; mirroring the input
     * mirrors the output. Swapping the two lines in image.c leaves this file
     * entirely green, which was checked rather than assumed. The order is
     * still written the way Ren'Py nests it, because the next manipulator
     * added may not be a symmetric one. */
    {
        mr_surface *flipped = clone(raw);
        int x, y;

        for (y = 0; y < flipped->h; y++) {
            mr_u32 *row = flipped->px + (size_t)y * flipped->pitch;
            for (x = 0; x < flipped->w / 2; x++) {
                mr_u32 t = row[x];
                row[x] = row[flipped->w - 1 - x];
                row[flipped->w - 1 - x] = t;
            }
        }
        want = ref_blur_copy(flipped, 2.0f, 2.0f);
        got = clone(mr_image_get("quad flipblur"));
        check("`quad flipblur` is both flipped and blurred", same(got, want));
        if (!same(got, want))
            report_diff(got, want);
        mr_surface_free(flipped);
        mr_surface_free(want);
        mr_surface_free(got);
    }

    mr_surface_free(raw);
}

int main(int argc, char **argv)
{
    mr_script *s;

    setbuf(stdout, NULL);
    if (argc < 2) {
        fprintf(stderr, "usage: blurtest <story-dir>\n");
        return 2;
    }

    printf("\n-- our blur against Ren'Py's, byte for byte --\n");
    /* 2.0 is the radius the game uses. The rest are chosen for what they do
     * to blur_filters: 0.5 and 1.0 give the smallest widths, 8.0 a wide
     * kernel, and the asymmetric pairs make an axis swap visible. Odd sizes
     * because an even-sized image hides an off-by-one in the pass that walks
     * the shorter axis. */
    agrees_with_renpy(64, 48, 2.0f, 2.0f);
    agrees_with_renpy(63, 47, 2.0f, 2.0f);
    agrees_with_renpy(64, 48, 0.5f, 0.5f);
    agrees_with_renpy(64, 48, 1.0f, 1.0f);
    agrees_with_renpy(64, 48, 8.0f, 8.0f);
    agrees_with_renpy(64, 48, 1.0f, 6.0f);
    agrees_with_renpy(64, 48, 6.0f, 1.0f);
    agrees_with_renpy(17, 101, 3.0f, 3.0f);
    /* Full stage size, which is what a game actually blurs. */
    agrees_with_renpy(MR_STAGE_W, MR_STAGE_H, 2.0f, 2.0f);

    /* Not an assertion -- a number for the performance budget. A blurred
     * background costs this on top of its PNG decode, once per cache miss,
     * inside whichever phase first asks for the image. The host figure is
     * from a -O0 build and the Pentium III will be several times worse, so
     * what this is for is noticing if it ever grows. */
    {
        mr_surface *s = make_src(MR_STAGE_W, MR_STAGE_H);
        clock_t t0 = clock();

        mr_surface_blur(s, 2.0f, 2.0f);
        printf("   a 1280x720 blur at radius 2.0: %ldms (host, -O0)\n",
               (long)((clock() - t0) * 1000 / CLOCKS_PER_SEC));
        mr_surface_free(s);
    }

    printf("\n-- and it is not a no-op, on pixels that could show it --\n");
    blur_actually_blurs();
    edges_are_extended();
    opaque_stays_opaque();
    tiny_surfaces_survive();

    printf("\n-- the declarations --\n");
    mr_vfs_init(NULL, argv[1]);
    mr_image_init(argv[1]);
    mr_scene_init();
    s = mr_script_load(argv[1]);
    mr_interp_init(s, argv[1]);
    mr_ui_init(argv[1]);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);
    mr_interp_start("start");

    declared_image_is_blurred(s);

    printf("\n%s\n", fails ? "FAILURES" : "all ok");
    return fails ? 1 : 0;
}

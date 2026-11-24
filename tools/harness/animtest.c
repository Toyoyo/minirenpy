/* Animated images: `image <name>:` with a block of frames.
 *
 * Reported from a second game's declarations:
 *
 *     image fireworks animated:
 *         "images/fireworks0.png" with dissolve
 *         pause 2.2
 *         ...
 *         repeat
 *
 * In Ren'Py this is an ATL block whose statements are children rather than
 * interpolations (atl.py:1303 RawChild, :1333 Child). Two readings of that
 * source shape everything here, and both are the kind of thing that produces
 * a plausible-looking wrong animation:
 *
 *   - A frame's duration is the `pause` that *follows* it. A Child swaps the
 *     displayable and returns "next" immediately; the pause after it is what
 *     holds it on screen. Attaching each pause to the frame before it reads
 *     backwards from the source text and is the obvious thing to get wrong.
 *   - `with dissolve` on a frame line is a cross-fade *inside* the image,
 *     from the frame leaving to the frame arriving (Child.execute wraps the
 *     new child in the transition against the old). It is not a scene
 *     transition; nothing else on the stage takes part.
 *
 * The engine puts this in the image registry rather than on the display list
 * entry, because an animated image is part of what a *name* means -- the same
 * place im.Flip and im.Blur live. So there are three separable claims, and
 * they fail differently:
 *
 *   1. The parse. Frame count, which pause belongs to which frame, `repeat`,
 *      and the per-frame transition and its duration.
 *   2. The resolution. mr_image_get on an animated name returns the frame the
 *      clock says, wrapping or holding at the end.
 *   3. The blend. A cross-fade between two frames has to interpolate alpha,
 *      premultiplied, or every edge that is opaque in one frame and clear in
 *      the other gets a dark fringe.
 *
 * Fixtures: images/anim{0,1,2}.png, 64x64 RGBA, each a left half and a right
 * half. The left halves are opaque and differ in colour, so "which frame is
 * showing" is one pixel read. The right halves differ in *alpha*, which is
 * the only way (3) is observable at all -- two opaque frames blend
 * identically whether the alpha handling is right or not, so an all-opaque
 * fixture would make every assertion below pass with the premultiply ripped
 * out. That is the same unobservability trap that let the backdrop pan and
 * then im.Blur ship against fixtures that could not fail.
 *
 * The frame durations are deliberately unequal (100/200/400ms). With equal
 * ones, an implementation that divided the elapsed time by the frame count
 * instead of walking the frames would land on the right frame every time.
 *
 * Runs on tools/harness/story.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mr.h"
#include "interp.h"
#include "vfs.h"
#include "script.h"
#include "image.h"
#include "scene.h"
#include "ui.h"

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
    printf("  %-62s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

/* --- fixture colours ----------------------------------------------------- */

/* What mkfixtureimg.py wrote, restated here so a check reads as an equality
 * against a named constant rather than against a magic number. If the
 * fixtures are regenerated with different colours these are what to change. */
#define A0_LEFT   MR_ARGB(255, 0xff, 0x00, 0x00)
#define A0_RIGHT  MR_ARGB(0,   0x00, 0x00, 0x00)
#define A1_LEFT   MR_ARGB(255, 0x00, 0x00, 0xff)
#define A1_RIGHT  MR_ARGB(128, 0x00, 0x00, 0xff)
#define A2_LEFT   MR_ARGB(255, 0x00, 0xff, 0x00)
#define A2_RIGHT  MR_ARGB(64,  0x00, 0xff, 0x00)

#define LEFT_X   8
#define RIGHT_X  48

static mr_u32 px(const mr_surface *s, int x, int y)
{
    if (!s)
        mr_fatal("pixel read from a NULL surface -- a name did not resolve");
    return s->px[(size_t)y * s->pitch + x];
}

/* The pixels of whatever `name` resolves to at `now`. Each call opens a frame
 * so the registry's pinning behaves as it does in the real loop. */
static mr_u32 at(const char *name, long now, int x)
{
    mr_image_begin_frame();
    mr_image_set_time(now);
    return px(mr_image_get(name), x, 32);
}

static const char *cname(mr_u32 c)
{
    static char buf[4][32];
    static int  n;
    char *b = buf[n++ & 3];

    sprintf(b, "a%02x %02x%02x%02x", MR_A(c), MR_R(c), MR_G(c), MR_B(c));
    return b;
}

static void check_px(const char *label, mr_u32 got, mr_u32 want)
{
    int ok = (got == want);

    printf("  %-62s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok) {
        printf("       got %s, want %s\n", cname(got), cname(want));
        fails++;
    }
}

/* --- (1) the parse ------------------------------------------------------- */

static const mr_anim *anim_of(mr_script *s, const char *name)
{
    int i;

    for (i = 0; i < s->n; i++) {
        if (s->st[i].kind == MR_ST_IMAGE && !strcmp(s->st[i].name, name))
            return s->st[i].anim;
    }
    return NULL;
}

static void parsed_the_block(mr_script *s)
{
    const mr_anim *a;
    int i, nanim = 0;

    printf("\n-- what the parser made of each animation block --\n");
    for (i = 0; i < s->n; i++) {
        const mr_anim *q = s->st[i].anim;
        int f;

        if (s->st[i].kind != MR_ST_IMAGE || !q)
            continue;
        nanim++;
        printf("   %-12s %d frames, repeat=%d, total=%ldms\n",
               s->st[i].name, q->n, q->repeat, q->total_ms);
        for (f = 0; f < q->n; f++)
            printf("       %-24s %4ldms  fade %ldms\n",
                   q->v[f].file, q->v[f].ms, q->v[f].trans_ms);
    }
    /* By name, not by count. This used to assert `nanim == 3`, which made it
     * a claim about the whole shared fixture rather than about the three
     * blocks this file is concerned with: adding an animated background for
     * bandtest took it red without anything being wrong, which is the failure
     * mode NOTES.md describes for tests pinned to a fixture's totals. The
     * useful question is whether the parser turned each of *these* blocks
     * into an animation, and nanim is printed above for context. */
    check("the three animation blocks this file uses parsed",
          anim_of(s, "anim cut") && anim_of(s, "anim fade") &&
          anim_of(s, "anim once"));
    check("and the parser made animations, not nothing", nanim >= 3);

    a = anim_of(s, "anim cut");
    check("`anim cut` exists and was not parsed as an = form", a != NULL);
    if (!a)
        return;

    check("three frames", a->n == 3);
    check("frame files resolved against images/",
          !strcmp(a->v[0].file, "images/anim0.png") &&
          !strcmp(a->v[1].file, "images/anim1.png") &&
          !strcmp(a->v[2].file, "images/anim2.png"));

    /* The reading that matters: each pause holds the frame *above* it. Were
     * the pauses attached to the following frame instead, the durations would
     * come out 0/100/200 with the last 400 dropped -- so this one assertion
     * is what distinguishes the two readings. */
    check("each pause is the duration of the frame it follows",
          a->v[0].ms == 100 && a->v[1].ms == 200 && a->v[2].ms == 400);
    check("the durations are unequal, so a divide-by-count reader fails",
          a->v[0].ms != a->v[1].ms && a->v[1].ms != a->v[2].ms);
    check("total duration is the sum", a->total_ms == 700);
    check("`repeat` was seen", a->repeat == 1);
    check("no `with` means no cross-fade",
          a->v[0].trans_ms == 0 && a->v[1].trans_ms == 0 &&
          a->v[2].trans_ms == 0);

    a = anim_of(s, "anim fade");
    check("`anim fade` exists", a != NULL);
    if (a) {
        check("a per-frame `with dissolve` became a cross-fade on every frame",
              a->v[0].trans_ms > 0 && a->v[1].trans_ms > 0 &&
              a->v[2].trans_ms > 0);
        /* The parse records what the script said: the stock dissolve is
         * 500ms, and every frame here is shorter than that. The clamp that
         * makes such a fade usable belongs to the registry, not here -- the
         * parsed form should say what was written. It is asserted where it
         * happens, by behaviour, in fades_between_frames. */
        check("the stock dissolve's own 500ms survives the parse unaltered",
              a->v[0].trans_ms == 500 && a->v[1].trans_ms == 500 &&
              a->v[2].trans_ms == 500);
        check("the frames are otherwise the same as the cut form",
              a->n == 3 && a->total_ms == 700 && a->repeat == 1);
    }

    a = anim_of(s, "anim once");
    check("`anim once` exists", a != NULL);
    if (a) {
        check("no `repeat` means it does not loop", a->repeat == 0);
        check("two frames, 100 and 200",
              a->n == 2 && a->v[0].ms == 100 && a->v[1].ms == 200);
    }
}

/* --- (2) which frame the clock selects ----------------------------------- */

static void steps_through_frames(void)
{
    printf("\n-- which frame `anim cut` resolves to --\n");

    /* Frame boundaries are half-open: [0,100) [100,300) [300,700). */
    check_px("t=0 is the first frame",     at("anim cut", 0, LEFT_X), A0_LEFT);
    check_px("t=99 is still the first",    at("anim cut", 99, LEFT_X), A0_LEFT);
    check_px("t=100 is the second",        at("anim cut", 100, LEFT_X), A1_LEFT);
    check_px("t=299 is still the second",  at("anim cut", 299, LEFT_X), A1_LEFT);
    check_px("t=300 is the third",         at("anim cut", 300, LEFT_X), A2_LEFT);
    check_px("t=699 is still the third",   at("anim cut", 699, LEFT_X), A2_LEFT);

    /* A reader that divided elapsed time by the frame count would put t=100
     * in frame 0 (100 / (700/3) = 0) and t=300 in frame 1. Both are checked
     * above, so this is a note rather than an assertion -- but it is why the
     * fixture's durations are unequal. */
    check_px("t=700 wraps to the first",   at("anim cut", 700, LEFT_X), A0_LEFT);
    /* 850 % 700 = 150, which is inside [100,300): the second frame, one
     * cycle on. */
    check_px("t=850 is the second frame of the second cycle",
             at("anim cut", 850, LEFT_X), A1_LEFT);
    check_px("t=1000 is 300 into the cycle, so the third frame",
             at("anim cut", 1000, LEFT_X), A2_LEFT);
    check_px("a time far out still wraps correctly",
             at("anim cut", 700L * 1000L + 350L, LEFT_X), A2_LEFT);

    /* Not an animation, so the clock must not move it. */
    check_px("a still image ignores the clock (t=0)",
             at("quad", 0, 0), at("quad", 999999, 0));

    printf("\n-- a one-shot holds its last frame --\n");
    check_px("t=0 is the first frame",   at("anim once", 0, LEFT_X), A0_LEFT);
    check_px("t=150 is the second",      at("anim once", 150, LEFT_X), A1_LEFT);
    check_px("t=300 is past the end and holds the second",
             at("anim once", 300, LEFT_X), A1_LEFT);
    check_px("and still holds it much later",
             at("anim once", 60000, LEFT_X), A1_LEFT);

    printf("\n-- and the frames really are distinguishable --\n");
    /* Without this the whole section above could be reading one unchanging
     * image. Three left-half colours, all different. */
    check("the three frames differ from each other where they are read",
          A0_LEFT != A1_LEFT && A1_LEFT != A2_LEFT && A0_LEFT != A2_LEFT);
    check("mr_image_is_anim tells the animated names from the still ones",
          mr_image_is_anim("anim cut") && mr_image_is_anim("anim fade") &&
          mr_image_is_anim("anim once") && !mr_image_is_anim("quad") &&
          !mr_image_is_anim("room"));
}

/* --- (3) the cross-fade -------------------------------------------------- */

/* An independent reference for the blend, in floating point and written from
 * the definition rather than from the integer code it is checking: mix the
 * premultiplied values, then divide back out.
 *
 * Deliberately not a transcription of mr_lerp_surface_a. A reference derived
 * from the implementation only proves the implementation agrees with itself;
 * the thing worth catching is a premultiply that is scaled wrong, which looks
 * entirely correct until you compare against arithmetic done another way.
 * Compared with a tolerance of 1 because the two round differently -- the
 * exact values are pinned separately, by hand, below. */
static mr_u32 ref_blend(mr_u32 av, mr_u32 bv, int t)
{
    double w = (double)t / 255.0, iw = 1.0 - w;
    double aa = (double)MR_A(av), ba = (double)MR_A(bv);
    double oa = aa * iw + ba * w;
    double r, g, b;

    if (oa <= 0.0)
        return MR_ARGB(0, 0, 0, 0);

    r = ((double)MR_R(av) * aa * iw + (double)MR_R(bv) * ba * w) / oa;
    g = ((double)MR_G(av) * aa * iw + (double)MR_G(bv) * ba * w) / oa;
    b = ((double)MR_B(av) * aa * iw + (double)MR_B(bv) * ba * w) / oa;

    return MR_ARGB((int)(oa + 0.5), (int)(r + 0.5), (int)(g + 0.5),
                   (int)(b + 0.5));
}

/* What a *straight* mix -- RGB interpolated independently of alpha -- would
 * give. This is the wrong answer, and it is here so the test can say it is
 * reaching a pixel where right and wrong differ.
 *
 * Rounded the same way the implementation rounds, so that where the two
 * disagree it is because of the premultiply and not because of a half-bit. */
static mr_u32 straight_blend(mr_u32 av, mr_u32 bv, int t)
{
    unsigned it = 255u - (unsigned)t;

    return MR_ARGB(((unsigned)MR_A(av) * it + (unsigned)MR_A(bv) * (unsigned)t + 127u) / 255u,
                   ((unsigned)MR_R(av) * it + (unsigned)MR_R(bv) * (unsigned)t + 127u) / 255u,
                   ((unsigned)MR_G(av) * it + (unsigned)MR_G(bv) * (unsigned)t + 127u) / 255u,
                   ((unsigned)MR_B(av) * it + (unsigned)MR_B(bv) * (unsigned)t + 127u) / 255u);
}

static int near_eq(mr_u32 a, mr_u32 b)
{
    int d[4], i;

    d[0] = MR_A(a) - MR_A(b);
    d[1] = MR_R(a) - MR_R(b);
    d[2] = MR_G(a) - MR_G(b);
    d[3] = MR_B(a) - MR_B(b);
    for (i = 0; i < 4; i++) {
        if (d[i] < -1 || d[i] > 1)
            return 0;
    }
    return 1;
}

static void blend_is_premultiplied(void)
{
    mr_surface *a, *b, *out;
    int t, bad = 0, differs = 0;

    printf("\n-- mr_lerp_surface_a against arithmetic done another way --\n");

    a = mr_surface_new(4, 1);
    b = mr_surface_new(4, 1);
    out = mr_surface_new(4, 1);

    /* Column 0: both opaque, different colours -- the case a straight mix
     * also gets right. Column 1: clear to opaque, the fringe case. Column 2:
     * two partial alphas. Column 3: identical pixels, the early-out. */
    a->px[0] = MR_ARGB(255, 255, 0, 0);   b->px[0] = MR_ARGB(255, 0, 0, 255);
    a->px[1] = MR_ARGB(0, 0, 0, 0);       b->px[1] = MR_ARGB(128, 0, 0, 255);
    a->px[2] = MR_ARGB(64, 10, 200, 30);  b->px[2] = MR_ARGB(192, 240, 15, 250);
    a->px[3] = MR_ARGB(200, 7, 8, 9);     b->px[3] = MR_ARGB(200, 7, 8, 9);

    for (t = 1; t < 255; t++) {
        int x;

        mr_lerp_surface_a(out, a, b, t);
        for (x = 0; x < 4; x++) {
            mr_u32 want = ref_blend(a->px[x], b->px[x], t);

            if (!near_eq(out->px[x], want)) {
                if (!bad)
                    printf("       t=%d x=%d: got %s, want %s\n",
                           t, x, cname(out->px[x]), cname(want));
                bad++;
            }
            /* Count where the premultiplied and straight answers part
             * company, so a pass cannot be a pass on pixels that agree. */
            if (want != straight_blend(a->px[x], b->px[x], t))
                differs++;
        }
    }
    check("every blend matches the independent reference", bad == 0);
    check("and on pixels where a straight mix would differ", differs > 200);

    /* Two values worked out by hand, exactly, so the tolerance above is not
     * carrying the whole check. a=(0,0,0,0) over b=(128,0,0,255) at t=127:
     *     den = 0*128 + 128*127 = 16256
     *     A'  = 16256/255 = 63.75, rounded -> 64
     *     B'  = (0 + 255*128*127) / 16256 = 255
     * The blue survives at full strength because the transparent pixel's RGB
     * carries no weight. A straight mix gives B' = 127 -- a blue that has
     * been mixed halfway to black, which is the dark fringe. */
    mr_lerp_surface_a(out, a, b, 127);
    check_px("clear-to-opaque keeps the arriving colour undimmed",
             out->px[1], MR_ARGB(64, 0, 0, 255));
    check_px("which is not what a straight mix gives",
             straight_blend(a->px[1], b->px[1], 127), MR_ARGB(64, 0, 0, 127));

    /* Equal alphas: the premultiply cancels and this is an ordinary mix. */
    mr_lerp_surface_a(out, a, b, 127);
    check_px("two opaque pixels mix normally and stay opaque",
             out->px[0], MR_ARGB(255, 128, 0, 127));

    /* The endpoints, which are short-circuited to copies. */
    mr_lerp_surface_a(out, a, b, 0);
    check_px("t=0 is the outgoing frame", out->px[1], a->px[1]);
    mr_lerp_surface_a(out, a, b, 255);
    check_px("t=255 is the arriving frame", out->px[1], b->px[1]);

    /* The opaque-result version must still force alpha, or the transitions
     * that rely on it change behaviour. */
    mr_lerp_surface(out, a, b, 127);
    check("mr_lerp_surface is unchanged and still returns opaque pixels",
          MR_A(out->px[1]) == 255 && MR_A(out->px[2]) == 255);

    mr_surface_free(a);
    mr_surface_free(b);
    mr_surface_free(out);
}

static void fades_between_frames(void)
{
    mr_u32 got, want;

    printf("\n-- `anim fade` cross-fades frame to frame --\n");

    /* Frame 1 occupies [100,300) and fades in across all 200ms of it, from
     * frame 0. At t=200 it is 100ms in: t = 100*255/200 = 127. */
    got = at("anim fade", 200, LEFT_X);
    want = ref_blend(A0_LEFT, A1_LEFT, 127);
    check_px("mid-frame, the opaque halves are a blend of two frames",
             got, want);
    check("and that blend is neither endpoint",
          got != A0_LEFT && got != A1_LEFT);

    got = at("anim fade", 200, RIGHT_X);
    want = ref_blend(A0_RIGHT, A1_RIGHT, 127);
    check_px("mid-frame, the alpha-differing halves blend premultiplied",
             got, want);
    check("which a straight mix would get wrong here",
          want != straight_blend(A0_RIGHT, A1_RIGHT, 127));

    /* At the instant a frame starts, the fade has not progressed: t=0, which
     * is the frame being left. At the last instant before the next one it is
     * as good as arrived. */
    check_px("at the start of a frame the fade shows the previous one",
             at("anim fade", 100, LEFT_X), A0_LEFT);

    got = at("anim fade", 299, LEFT_X);
    want = ref_blend(A0_LEFT, A1_LEFT, (int)(199L * 255L / 200L));
    check_px("at the end of a frame the fade has all but arrived", got, want);

    /* Frame 2 is [300,700) fading from frame 1 over 400ms. At t=500 it is
     * 200ms in: t = 200*255/400 = 127. A fade that used a fixed duration, or
     * the previous frame's, would land somewhere else. */
    got = at("anim fade", 500, LEFT_X);
    want = ref_blend(A1_LEFT, A2_LEFT, 127);
    check_px("each frame's fade runs over its own duration", got, want);

    /* has_alpha on the blended surface, which is not a pixel value and so is
     * not covered by anything above. mr_blit takes an opaque fast path when
     * it is 0, and mr_surface_new leaves it 0 -- so a cross-fade of RGBA art
     * would composite as an opaque rectangle over whatever is behind it,
     * which is exactly what interpolating alpha was for. Reading the pixels
     * of the returned surface cannot see that; only asking the surface can. */
    mr_image_begin_frame();
    mr_image_set_time(200);
    {
        mr_surface *s = mr_image_get("anim fade");
        mr_surface *f = mr_image_get("images/anim0.png");

        check("the frames themselves are flagged as having alpha",
              f && f->has_alpha);
        check("and so is the surface a cross-fade returns",
              s && s->has_alpha);
    }

    /* has_alpha comes from the *pixels*, not from the PNG header.
     *
     * png.c reads the colour type first -- which only says an alpha channel
     * is present -- and then scans, because this game's art is exported as
     * RGBA whether or not anything in the channel differs from 255. Every
     * such asset otherwise made mr_blit run its per-pixel loop to find
     * `a == 255` and assign, 43 against 27 ns/px, which on a 1280x720
     * background is 14.7ms a blit. The mechanism and the proof that flipping
     * the flag changes no pixel are in blendtest; this is the half that needs
     * a real file.
     *
     * Three witnesses, because two would not separate the question from the
     * answer: an RGBA file that is opaque throughout must come out 0, an RGBA
     * file with real transparency must stay 1 -- or the scan is simply
     * clearing the flag for everything -- and an RGB file must still be 0,
     * which it was before any of this. The fixture's alpha content was
     * checked rather than assumed: zoomband.png is 64x64 RGBA with every
     * alpha 255, person.png is 300x600 RGBA reaching alpha 0. */
    mr_image_begin_frame();
    {
        mr_surface *op  = mr_image_get("images/zoomband.png");
        mr_surface *tr  = mr_image_get("images/person.png");
        mr_surface *rgb = mr_image_get("images/room.png");

        check("an RGBA image that is opaque throughout loads as opaque",
              op && !op->has_alpha);
        check("one with real transparency keeps its alpha flag",
              tr && tr->has_alpha);
        check("and an RGB image is opaque as it always was",
              rgb && !rgb->has_alpha);
    }

    /* The clamp, asserted where it actually happens. The script asked for the
     * stock 500ms dissolve on a 100ms frame; the registry has to shorten it
     * to 100ms or the fade never completes. At t=99 a clamped fade is 99/100
     * of the way in -- all but arrived at frame 0. Unclamped it would be
     * 99/500, barely a fifth, and the animation would never fully show any
     * frame. Frame 0's fade comes from frame 2, the previous one in a loop. */
    got = at("anim fade", 99, LEFT_X);
    check_px("a fade longer than its frame is clamped to fill it",
             got, ref_blend(A2_LEFT, A0_LEFT, (int)(99L * 255L / 100L)));
    check("so it has nearly arrived, not barely started",
          !near_eq(got, ref_blend(A2_LEFT, A0_LEFT, (int)(99L * 255L / 500L))));

    /* The cut form shares the frames and the timing and differs only in the
     * transition, so it is the control: if the fade were being applied to
     * every animation, or to none, exactly one of these would still pass. */
    printf("\n-- and the cut form is not fading --\n");
    check_px("`anim cut` mid-frame is a frame, not a blend",
             at("anim cut", 200, LEFT_X), A1_LEFT);
    check("the two forms disagree mid-frame, so the fade is doing something",
          at("anim cut", 200, LEFT_X) != at("anim fade", 200, LEFT_X));
}

/* --- and has to stop drawing when nothing is changing -------------------- */

/* Draw the animation at `now`, so the registry records what it handed out. */
static void draw_at(const char *name, long now)
{
    mr_image_begin_frame();
    mr_image_set_time(now);
    (void)mr_image_get(name);
}

/* Ask whether it would look different at `now`, without drawing. */
static int changed_at(const char *name, long now)
{
    mr_image_set_time(now);
    return mr_image_anim_changed(name);
}

/* The saving this exists for: an animation holds a frame for a long time and
 * resolves to the identical surface throughout, so the frame loop must not be
 * told to composite the whole stage again for it.
 *
 * Both halves matter and they fail in opposite, equally quiet ways. Reporting
 * changed too readily gives the saving back while looking perfectly correct;
 * reporting it too rarely freezes the animation. So every check below has its
 * opposite nearby. */
static void only_asks_for_frames_it_needs(void)
{
    int i, redraws;

    printf("\n-- an animation only asks for a redraw when it will differ --\n");

    /* On a name nothing has drawn yet. The fixtures above have all been
     * resolved by earlier sections, so asking about one of those would be
     * asking whether it changed since it was last drawn -- a different
     * question, and it would answer no. */
    {
        mr_anim fresh;

        memset(&fresh, 0, sizeof(fresh));
        fresh.n = 2;
        fresh.repeat = 1;
        strcpy(fresh.v[0].file, "images/anim0.png");
        fresh.v[0].ms = 100;
        strcpy(fresh.v[1].file, "images/anim2.png");
        fresh.v[1].ms = 100;
        mr_image_define_anim("neverdrawn", &fresh);

        mr_image_set_time(0);
        check("before anything is drawn, it reports changed",
              mr_image_anim_changed("neverdrawn") == 1);
        draw_at("neverdrawn", 0);
        check("and stops once it has been", !changed_at("neverdrawn", 0));
    }

    /* `anim cut` frame 0 runs [0,100) with no cross-fade: one draw, then
     * nothing until the boundary. */
    draw_at("anim cut", 0);
    check("just drawn, it does not want drawing again",
          !changed_at("anim cut", 0));
    check("nor part way through the same frame",
          !changed_at("anim cut", 50) && !changed_at("anim cut", 99));
    check("but it does at the next frame's first instant",
          changed_at("anim cut", 100));

    draw_at("anim cut", 100);
    check("and then settles again",
          !changed_at("anim cut", 150) && !changed_at("anim cut", 299));
    check("until the frame after that",
          changed_at("anim cut", 300));

    /* A cross-fade is the opposite case: it genuinely differs every frame,
     * and must keep asking. `anim fade`'s frame 1 fades across [100,300). */
    draw_at("anim fade", 150);
    check("mid-cross-fade it wants every frame",
          changed_at("anim fade", 160) && changed_at("anim fade", 180));

    /* A still image never asks, and an unknown name must not either -- the
     * scene asks about every name on the display list. */
    check("a still image never asks", !mr_image_anim_changed("quad"));
    check("and an unknown name is not an error",
          !mr_image_anim_changed("no such image at all"));

    /* The headline number, measured rather than asserted in the abstract:
     * walk one full 700ms cycle of each at 33ms and count the redraws a
     * frame loop would actually perform. The cut form changes three times a
     * cycle; the fade form is fading for its whole cycle here, because every
     * frame's stock 500ms dissolve was clamped to fill it, so it is the
     * worst case and should ask for nearly everything. */
    {
        long t;

        redraws = 0;
        for (t = 0; t < 700; t += 33) {
            if (changed_at("anim cut", t)) {
                redraws++;
                draw_at("anim cut", t);
            }
        }
        printf("   `anim cut`:  %2d redraws over a 700ms cycle of 22 frames\n",
               redraws);
        check("the cut form asks for a handful, not for every frame",
              redraws <= 4);
        check("and for at least one per frame change", redraws >= 3);

        redraws = 0;
        for (t = 0; t < 700; t += 33) {
            if (changed_at("anim fade", t)) {
                redraws++;
                draw_at("anim fade", t);
            }
        }
        printf("   `anim fade`: %2d redraws over the same cycle (all-fading, "
               "so the worst case)\n", redraws);
        check("the fade form asks for essentially every frame", redraws >= 20);
    }

    /* And the saving is real rather than an artefact of the counting: the cut
     * form has to ask for strictly fewer than the fade form. */
    (void)i;
}

/* --- the frame loop has to keep drawing --------------------------------- */

static void drives_redraw(void)
{
    mr_place pl;

    printf("\n-- an animation asks the frame loop for frames --\n");

    memset(&pl, 0, sizeof(pl));

    /* mr_scene_animating takes a timestamp but reads the registry's clock
     * for the animation half, because main.c sets that once per frame before
     * anything resolves an image. Here the two are set to match by hand. */

    mr_scene_clear();
    mr_scene_show("quad", &pl, 0);
    mr_scene_commit();
    mr_image_set_time(0);
    check("a still image on the scene does not ask for redraws",
          !mr_scene_animating(0));

    mr_scene_clear();
    mr_scene_show("anim cut", &pl, 0);
    mr_scene_commit();
    draw_at("anim cut", 0);
    mr_image_set_time(0);
    check("an animated image, just drawn, does not ask either",
          !mr_scene_animating(0));
    mr_image_set_time(50);
    check("nor while it is still holding the same frame",
          !mr_scene_animating(50));
    mr_image_set_time(100);
    check("but it does as soon as its frame changes",
          mr_scene_animating(100));

    /* The background is resolved by name too, and is the commonest place a
     * game puts an animation. Covered separately because it is a different
     * field of the display list and an early version checked only the
     * shown-image array. */
    mr_scene_clear();
    mr_scene_scene("anim cut");
    mr_scene_commit();
    draw_at("anim cut", 0);
    mr_image_set_time(100);
    check("so does an animated `scene` background", mr_scene_animating(100));
    draw_at("anim cut", 100);
    mr_image_set_time(150);
    check("and it too goes quiet once that frame is drawn",
          !mr_scene_animating(150));

    mr_scene_clear();
    mr_scene_scene("quad");
    mr_scene_commit();
    mr_image_set_time(0);
    check("and a still background never asks", !mr_scene_animating(0));
}

/* --- redefinition -------------------------------------------------------- */

/* Ren'Py keeps the last definition of a name. The registry has to overwrite
 * rather than merge in both directions, and the half that is easy to forget
 * is an animated name redefined as a still one: the anim has to be dropped or
 * the name goes on animating off a definition that no longer says so. */
static void redefinition_overwrites(void)
{
    mr_anim a;

    printf("\n-- a redefined name takes the new definition --\n");

    memset(&a, 0, sizeof(a));
    a.n = 2;
    a.repeat = 1;
    strcpy(a.v[0].file, "images/anim0.png");
    a.v[0].ms = 100;
    strcpy(a.v[1].file, "images/anim2.png");
    a.v[1].ms = 100;

    mr_image_define_anim("redef", &a);
    check("defined as an animation, it is one", mr_image_is_anim("redef"));
    check_px("and resolves to its frames", at("redef", 150, LEFT_X), A2_LEFT);

    mr_image_define("redef", "images/anim1.png", 0);
    check("redefined as a still image, it stops being animated",
          !mr_image_is_anim("redef"));
    check_px("and resolves to the file, whatever the clock says",
             at("redef", 150, LEFT_X), A1_LEFT);

    mr_image_define_anim("redef", &a);
    check("and back again", mr_image_is_anim("redef"));
    check_px("with the clock in charge once more",
             at("redef", 150, LEFT_X), A2_LEFT);

    /* An animation with no duration at all would be a division by zero in
     * the frame lookup. It has to show its first frame and not fault. */
    memset(&a, 0, sizeof(a));
    a.n = 2;
    strcpy(a.v[0].file, "images/anim0.png");
    strcpy(a.v[1].file, "images/anim2.png");
    mr_image_define_anim("nodur", &a);
    check_px("an animation whose frames have no duration shows its first",
             at("nodur", 12345, LEFT_X), A0_LEFT);
}

int main(int argc, char **argv)
{
    mr_script *s;

    setbuf(stdout, NULL);
    if (argc < 2) {
        fprintf(stderr, "usage: animtest <story-dir>\n");
        return 2;
    }

    mr_vfs_init(NULL, argv[1]);
    mr_image_init(argv[1]);
    mr_scene_init();
    s = mr_script_load(argv[1]);
    mr_interp_init(s, argv[1]);
    mr_ui_init(argv[1]);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);
    mr_interp_start("start");

    parsed_the_block(s);
    steps_through_frames();
    blend_is_premultiplied();
    fades_between_frames();
    only_asks_for_frames_it_needs();
    drives_redraw();
    redefinition_overwrites();

    printf("\n%s\n", fails ? "FAILURES" : "all ok");
    return fails ? 1 : 0;
}

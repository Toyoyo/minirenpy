/* The compositor profile's buckets (mr.h, gfx.c).
 *
 * MR_PROF's `scene` was one number -- 175.76ms of a 304.86ms frame on the
 * Pentium III -- and a decision about SIMD or about dirty rectangles needs it
 * split by what the inner loop costs: an opaque row copy runs at whatever the
 * memory does and an alpha blend is three multiply-add-divides per pixel. The
 * split is only worth having if the attribution is right, and "right" here
 * means two separable claims:
 *
 *   - the *pixels* land in the bucket matching the loop that ran, and count
 *     the clipped rectangle rather than the nominal one;
 *   - the *milliseconds* land in the same bucket and are the elapsed time.
 *
 * The second claim is normally untestable on this host: the real clock is
 * 1ms-coarse, which is most of a small blit, and `NOTES.md` is explicit that
 * the shims lie about time. That is exactly why mr_gfx_profile_enable takes a
 * clock rather than calling mr_plat_ticks -- this harness passes one whose
 * every reading it chooses, so the ms attribution is checked arithmetically
 * and the claim does not belong to the target. Nothing here measures the
 * speed of anything, and no claim in it depends on real time.
 *
 * What is deliberately *not* covered: the engine-side wiring in main.c, which
 * no harness links (CLAUDE.md). That the `scene` and `ui` buckets are read
 * between the two phases is asserted by inspection of main.c and by the log
 * from a real run, not by this file.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "mr.h"

static int g_fail;

static void check(const char *what, int ok)
{
    printf("  %s %s\n", ok ? "pass" : "FAIL", what);
    if (!ok)
        g_fail = 1;
}

void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); g_fail = 1; }
void mr_log(const char *f, ...) { (void)f; }

/* A clock that advances by a controlled amount per *reading*. gfx.c reads it
 * twice per accounted call -- once before the work and once after -- so a
 * step of `g_step` makes every call cost exactly g_step milliseconds, and a
 * bucket's ms is then a known multiple of its call count. That is what turns
 * "the time went somewhere plausible" into an equality. */
static unsigned int g_now;
static unsigned int g_step = 1;

static unsigned int fake_clock(void)
{
    unsigned int was = g_now;
    g_now += g_step;
    return was;
}

/* A surface with a known alpha state. `alpha` 0 marks it opaque, which is the
 * flag mr_blit branches on to choose memcpy over blending -- so this is what
 * decides which bucket a blit lands in. */
static mr_surface *mk(int w, int h, int alpha, mr_u32 fill)
{
    mr_surface *s = mr_surface_new(w, h);

    if (!s)
        return NULL;
    mr_surface_fill(s, fill);
    s->has_alpha = alpha;
    return s;
}

int main(void)
{
    mr_surface *dst, *opaque, *translucent;
    mr_surface *zoomed;
    mr_gfx_prof g;

    setbuf(stdout, NULL);

    dst = mr_surface_new(200, 100);
    if (!dst) {
        printf("  could not allocate the destination\n");
        return 1;
    }

    /* Built before the profile is enabled, so the fills inside mk() are not
     * accounted and each claim below starts from a clean slate. */
    opaque      = mk(50, 40, 0, 0xFF203040UL);
    translucent = mk(50, 40, 1, 0x80203040UL);
    if (!opaque || !translucent) {
        printf("  could not allocate the sources\n");
        return 1;
    }

    /* --- off by default ---------------------------------------------------
     *
     * The engine pays for this on every frame it draws, so "off" has to mean
     * off: a bucket that filled up without being asked would be a cost with
     * no reader. */
    mr_blit(dst, opaque, 0, 0);
    mr_gfx_profile(&g);
    check("nothing is accounted until a clock is handed over",
          g.copy_n == 0 && g.blend_n == 0 && g.fill_n == 0 &&
          g.lerp_n == 0 && g.stretch_n == 0);

    mr_gfx_profile_enable(fake_clock);

    /* --- the split that the whole thing exists for ------------------------
     *
     * Same function, same size, same destination. The only difference is the
     * source's has_alpha, and it has to be the difference between the two
     * buckets -- because on the target it is the difference between a memcpy
     * and three divides per pixel. */
    mr_gfx_profile(&g);                 /* discard anything pending */
    mr_blit(dst, opaque, 0, 0);
    mr_gfx_profile(&g);
    check("an opaque blit is charged to copy and not to blend",
          g.copy_n == 1 && g.copy_px == 50L * 40 && g.blend_n == 0);

    mr_blit(dst, translucent, 0, 0);
    mr_gfx_profile(&g);
    check("a blit with alpha is charged to blend and not to copy",
          g.blend_n == 1 && g.blend_px == 50L * 40 && g.copy_n == 0);

    /* --- clipped pixels, not nominal ones ---------------------------------
     *
     * The ms divided by the px is a per-pixel cost, and it is only a cost if
     * the px is the work the loop was actually offered. A blit hanging half
     * off the destination does half the work; charging it for the whole
     * rectangle would halve the apparent per-pixel cost of every sprite near
     * an edge. */
    mr_blit(dst, translucent, 175, 0);  /* 25 of 50 columns land */
    mr_gfx_profile(&g);
    check("a blit clipped by the destination is charged the clipped area",
          g.blend_n == 1 && g.blend_px == 25L * 40);

    /* And the same question for the clip stack, which is the one the dialogue
     * band uses -- the partial-frame path in main.c pushes a rect and the
     * cost of a typing frame is what it leaves inside it. */
    mr_clip_push(0, 0, 10, 40);
    mr_blit(dst, translucent, 0, 0);
    mr_clip_pop();
    mr_gfx_profile(&g);
    check("and the clip stack narrows the charge the same way",
          g.blend_n == 1 && g.blend_px == 10L * 40);

    /* A blit entirely outside is no work at all, and must not arrive as a
     * zero-pixel call: an inflated n is what makes the ms look like
     * instrumentation overhead when it is not. */
    mr_blit(dst, translucent, 900, 900);
    mr_gfx_profile(&g);
    check("a fully clipped blit is not charged at all", g.blend_n == 0);

    /* --- the remaining buckets -------------------------------------------- */
    mr_surface_fill(dst, 0xFF000000UL);
    mr_gfx_profile(&g);
    check("a fill is write-only work and has its own bucket",
          g.fill_n == 1 && g.fill_px == 200L * 100 && g.copy_n == 0);

    mr_surface_copy(dst, dst);
    mr_gfx_profile(&g);
    check("a surface copy is charged to copy",
          g.copy_n == 1 && g.copy_px == 200L * 100);

    mr_lerp_surface(dst, dst, dst, 128);
    mr_gfx_profile(&g);
    check("a transition lerp is charged to lerp, not to blend",
          g.lerp_n == 1 && g.lerp_px == 200L * 100 && g.blend_n == 0);

    mr_fade_to(dst, 0xFF000000UL, 128);
    mr_gfx_profile(&g);
    check("and so is a fade, which is the same shape of arithmetic",
          g.lerp_n == 1 && g.lerp_px == 200L * 100);

    mr_blit_stretch(dst, translucent, 0, 0, 50, 40, 0, 0, 100, 80);
    mr_gfx_profile(&g);
    check("a stretch blit is charged to stretch at its destination size",
          g.stretch_n == 1 && g.stretch_px == 100L * 80);

    zoomed = mr_surface_zoom(translucent, 100, 80, 0, 0);
    mr_gfx_profile(&g);
    check("and so is a zoom resample",
          zoomed != NULL && g.stretch_n == 1 && g.stretch_px == 100L * 80);
    if (zoomed)
        mr_surface_free(zoomed);

    /* --- no double counting ----------------------------------------------
     *
     * mr_blit_alpha at full opacity delegates to mr_blit, and mr_fade_to at
     * full delegates to mr_surface_fill. Charging in both places would count
     * the same milliseconds twice, which on the target would silently inflate
     * whichever bucket a dissolve happens to end on. */
    mr_blit_alpha(dst, opaque, 0, 0, 255);
    mr_gfx_profile(&g);
    check("a fully opaque mr_blit_alpha is charged once, as a copy",
          g.copy_n == 1 && g.blend_n == 0);

    mr_blit_alpha(dst, translucent, 0, 0, 128);
    mr_gfx_profile(&g);
    check("and a partial one is a blend, charged once",
          g.blend_n == 1 && g.copy_n == 0);

    mr_fade_to(dst, 0xFF000000UL, 255);
    mr_gfx_profile(&g);
    check("a complete fade is charged once, as the fill it performs",
          g.fill_n == 1 && g.lerp_n == 0);

    /* --- the milliseconds ------------------------------------------------
     *
     * The claim the real clock cannot support. Two readings per accounted
     * call at a known step means the elapsed total is an exact multiple, so
     * this checks that the time is charged to the bucket that did the work
     * and that it is the difference of the two readings rather than, say, a
     * single reading or a count of calls. */
    g_step = 7;
    mr_blit(dst, translucent, 0, 0);
    mr_blit(dst, translucent, 0, 0);
    mr_blit(dst, opaque, 0, 0);
    mr_gfx_profile(&g);
    check("elapsed time is charged to the bucket that did the work",
          g.blend_n == 2 && g.blend_ms == 14 &&
          g.copy_n == 1 && g.copy_ms == 7);

    /* A tick counter wraps -- timeGetTime does, every 49 days -- and the
     * unsigned subtraction in gfx.c is what survives it. A blit straddling
     * the wrap must not charge four billion milliseconds to a bucket and
     * swamp the report. */
    g_now = 0xFFFFFFFCUL;
    g_step = 7;
    mr_blit(dst, translucent, 0, 0);
    mr_gfx_profile(&g);
    check("and a reading across a counter wrap stays 7ms, not 4 billion",
          g.blend_n == 1 && g.blend_ms == 7);

    /* --- reading zeroes -------------------------------------------------
     *
     * main.c reads between the scene and the ui phase and relies on the
     * second read returning only what happened since the first. If a read
     * left the accumulators alone, every bucket would be a running total and
     * the ui line would contain the scene's work. */
    g_step = 1;
    g_now = 0;
    mr_blit(dst, translucent, 0, 0);
    mr_gfx_profile(&g);
    mr_gfx_profile(&g);
    check("a read zeroes the accumulators, so the next phase starts clean",
          g.blend_n == 0 && g.blend_px == 0 && g.blend_ms == 0);

    /* --- off again ------------------------------------------------------- */
    mr_gfx_profile_enable(NULL);
    mr_blit(dst, translucent, 0, 0);
    mr_gfx_profile(&g);
    check("and handing back a NULL clock stops the accounting",
          g.blend_n == 0);

    mr_surface_free(translucent);
    mr_surface_free(opaque);
    mr_surface_free(dst);

    printf("%s\n", g_fail ? "** FAIL **" : "ok");
    return g_fail ? 1 : 0;
}

/* bandcache.c - see bandcache.h for what this is for and where the seam is. */

#include <string.h>

#include "bandcache.h"

static mr_surface *g_strip;
static int         g_y, g_h;
static int         g_valid;

/* Rows between the stage and the strip. Not mr_blit: the strip has no alpha
 * and wants none of the blitter's clipping, and on a band frame the clip
 * stack is already set to exactly this rectangle, so going through mr_blit
 * would be a second, redundant clip against the same numbers -- with the
 * accounting then charged to the `copy` bucket as if it were compositing.
 * memcpy per row says what this is. */
static void rows(mr_u32 *dst, int dpitch, const mr_u32 *src, int spitch,
                 int w, int h)
{
    int y;

    for (y = 0; y < h; y++)
        memcpy(dst + (long)y * dpitch, src + (long)y * spitch,
               (size_t)w * sizeof(mr_u32));
}

/* The one geometry test both entry points need: a rectangle of the full stage
 * width that lies on the surface. */
static int fits(const mr_surface *stage, int y, int h)
{
    return stage && stage->px && h > 0 && y >= 0 && y + h <= stage->h;
}

int mr_band_cache_try(mr_surface *stage, int y, int h)
{
    if (!g_valid || !g_strip)
        return 0;
    if (y != g_y || h != g_h)
        return 0;
    if (!fits(stage, y, h))
        return 0;

    /* The strip is as wide as the stage it was taken from. A stage of a
     * different width is a different stage; refuse rather than copy part of
     * a row, which would leave the right-hand side stale. */
    if (stage->w != g_strip->w)
        return 0;

    rows(stage->px + (long)y * stage->pitch, stage->pitch,
         g_strip->px, g_strip->pitch, g_strip->w, h);
    return 1;
}

void mr_band_cache_offer(const mr_surface *stage, int y, int h, int keep)
{
    /* Invalidate first and unconditionally. Every path out of here that is
     * not a completed store has to leave the cache unusable, and there are
     * four of them; setting the flag at the end is the shape where adding a
     * fifth quietly keeps a stale strip. */
    g_valid = 0;

    if (!keep || !fits(stage, y, h))
        return;

    if (g_strip && (g_strip->w != stage->w || g_strip->h != h)) {
        mr_surface_free(g_strip);
        g_strip = NULL;
    }

    if (!g_strip) {
        g_strip = mr_surface_new(stage->w, h);
        if (!g_strip) {
            /* Not fatal and not worth retrying every frame -- but worth
             * saying once, because the symptom otherwise is "the band path
             * is slower than it was measured to be" with nothing to see. */
            mr_log("bandcache: no room for a %dx%d strip; band frames will"
                   " composite in full", stage->w, h);
            return;
        }
        g_strip->has_alpha = 0;
    }

    rows(g_strip->px, g_strip->pitch,
         stage->px + (long)y * stage->pitch, stage->pitch, stage->w, h);

    g_y = y;
    g_h = h;
    g_valid = 1;
}

void mr_band_cache_drop(void)
{
    if (g_strip)
        mr_surface_free(g_strip);
    g_strip = NULL;
    g_valid = 0;
}

long mr_band_cache_bytes(void)
{
    if (!g_strip)
        return 0;
    return (long)g_strip->pitch * g_strip->h * (long)sizeof(mr_u32);
}

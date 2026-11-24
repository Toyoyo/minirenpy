/* blurref.c - Ren'Py's own blur, as the oracle for ours.
 *
 * This is blur_filters, blur32_core and linblur32_core copied out of
 * renpy-8.5.3-source/src/core.c (lines 552-760) with two mechanical changes
 * and no others:
 *
 *   - the PyObject and SDL_Surface pointers are replaced by the `refsurf`
 *     struct below, so it builds without Python or SDL;
 *   - Py_BEGIN_ALLOW_THREADS is dropped.
 *
 * The arithmetic, the loop bounds, the edge handling and the integer
 * truncation are untouched, which is the entire point: blurtest runs this and
 * mr_surface_blur over the same pixels and demands they agree byte for byte.
 * An oracle I wrote myself from the same paper would only prove I can make
 * the same mistake twice.
 *
 * Ren'Py is MIT licensed; see renpy-8.5.3-source/LICENSE.txt. Copyright
 * 2004-2025 Tom Rothamel and contributors. This file is build-time test
 * scaffolding and is not compiled into the engine.
 *
 * Note on channel order: core.c walks pixels as four bytes and treats every
 * byte identically, and so does ours once it has unpacked them. Whether byte
 * 0 is alpha or blue therefore cannot change the output, which is why the two
 * can be compared as raw bytes despite disagreeing about what the channels
 * are called.
 */

#include <math.h>
#include "blurref.h"

void ref_blur_filters(float sigma, int n, int *wl, int *wu, int *m)
{
    *wl = (int) floor(sqrt(12 * sigma * sigma / n + 1));
    if (*wl % 2 == 0) (*wl)--;
    *wu = *wl + 2;
    *m = (int) round(
        (12 * sigma * sigma - n * *wl * *wl - 4 * n * *wl - 3 * n)
        / (-4 * *wl - 4)
    );
}

void ref_linblur32(const refsurf *src, refsurf *dst, int radius, int vertical)
{
    int c, r;

    int rows, cols;
    int incr, skip;

    unsigned char *srcpixels;
    unsigned char *dstpixels;

    unsigned char *dstp;

    int divisor;

    srcpixels = (unsigned char *) src->pixels;
    dstpixels = (unsigned char *) dst->pixels;

    if (vertical) {
        rows = dst->w;
        skip = 4;
        incr = dst->pitch - 4;
        cols = dst->h;
    } else {
        rows = dst->h;
        skip = dst->pitch;
        incr = 0;
        cols = dst->w;
    }

    divisor = radius * 2 + 1;

    for (r = 0; r < rows; r++) {
        unsigned char lr, lg, lb, la;
        unsigned char rr, rg, rb, ra;
        unsigned char *leader;
        unsigned char *trailer;
        int sumr, sumg, sumb, suma;
        int end;

        leader = srcpixels + r * skip;
        trailer = leader;
        dstp = dstpixels + r * skip;

        lr = *leader;
        lg = *(leader + 1);
        lb = *(leader + 2);
        la = *(leader + 3);

        sumr = lr * radius;
        sumg = lg * radius;
        sumb = lb * radius;
        suma = la * radius;

        for (c = 0; c < radius; c++) {
            sumr += *leader++;
            sumg += *leader++;
            sumb += *leader++;
            suma += *leader++;
            leader += incr;
        }

        /* left side of the kernel is off of the screen. */
        for (c = 0; c < radius; c++) {
            sumr += *leader++;
            sumg += *leader++;
            sumb += *leader++;
            suma += *leader++;
            leader += incr;

            *dstp++ = sumr / divisor;
            *dstp++ = sumg / divisor;
            *dstp++ = sumb / divisor;
            *dstp++ = suma / divisor;
            dstp += incr;

            sumr -= lr;
            sumg -= lg;
            sumb -= lb;
            suma -= la;
        }

        end = cols - radius - 1;

        /* The kernel is fully on the screen. */
        for (; c < end; c++) {
            sumr += *leader++;
            sumg += *leader++;
            sumb += *leader++;
            suma += *leader++;
            leader += incr;

            *dstp++ = sumr / divisor;
            *dstp++ = sumg / divisor;
            *dstp++ = sumb / divisor;
            *dstp++ = suma / divisor;
            dstp += incr;

            sumr -= *trailer++;
            sumg -= *trailer++;
            sumb -= *trailer++;
            suma -= *trailer++;
            trailer += incr;
        }

        rr = *leader++;
        rg = *leader++;
        rb = *leader++;
        ra = *leader++;

        /* The kernel is off the right side of the screen. */
        for (; c < cols; c++) {
            sumr += rr;
            sumg += rg;
            sumb += rb;
            suma += ra;

            *dstp++ = sumr / divisor;
            *dstp++ = sumg / divisor;
            *dstp++ = sumb / divisor;
            *dstp++ = suma / divisor;
            dstp += incr;

            sumr -= *trailer++;
            sumg -= *trailer++;
            sumb -= *trailer++;
            suma -= *trailer++;
            trailer += incr;
        }
    }
}

void ref_blur32(const refsurf *src, refsurf *wrk, refsurf *dst,
                float xrad, float yrad)
{
    int n = 3; /* number of passes, no more than six */

    int xl, xu, xm;
    int yl, yu, ym;
    int i;

    ref_blur_filters(xrad, n, &xl, &xu, &xm);

    if (xrad != yrad) {
        ref_blur_filters(yrad, n, &yl, &yu, &ym);
    } else {
        yl = xl; yu = xu; ym = xm;
    }

    for (i = 0; i < n; i++) {
        int xr = i < xm ? xl : xu;
        int yr;
        ref_linblur32(src, wrk, xr, 0);
        yr = i < ym ? yl : yu;
        ref_linblur32(wrk, dst, yr, 1);
        src = dst;
    }
}

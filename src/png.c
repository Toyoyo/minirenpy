/* png.c - PNG decoding onto a 32bpp surface via libpng.
 *
 * Every transformation is pushed into libpng so that the rows it hands back
 * are already in our pixel layout: bytes B,G,R,A, which read as an
 * 0xAARRGGBB DWORD on x86. That keeps the decode to a single pass with no
 * fixup loop over three and a half million pixels.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#include "png.h"
#include "image.h"
#include "vfs.h"
#include "plat.h"

static void png_err(png_structp png, png_const_charp msg)
{
    mr_log("libpng error: %s", msg);
    longjmp(png_jmpbuf(png), 1);
}

static void png_warn(png_structp png, png_const_charp msg)
{
    (void)png;
    mr_log("libpng warning: %s", msg);
}

/* libpng pulls bytes through this rather than using stdio directly, so a PNG
 * can live inside the archive. */
static void png_vfs_read(png_structp png, png_bytep out, png_size_t want)
{
    mr_file *f = (mr_file *)png_get_io_ptr(png);

    if (!f || mr_vfs_read(f, out, (long)want) != (long)want)
        png_error(png, "short read");
}

/* Every sixteenth row. Often enough that the pointer moves smoothly -- a
 * 720-row background comes up for air 45 times -- and rarely enough that
 * what it costs is lost in the decode. The mask rather than a modulo
 * because this runs per row on a Pentium III.
 *
 * Interlaced images call this once per row per Adam7 pass, so they service
 * more often than they need to. Harmless, and not worth a counter. */
static void row_done(png_structp png, png_uint_32 row, int pass)
{
    (void)png;
    (void)pass;
    if ((row & 15) == 0)
        mr_plat_service();
}

mr_surface *mr_png_load(const char *path)
{
    mr_file *fp;
    png_structp png;
    png_infop info;
    png_uint_32 w, h;
    int depth, ctype, i;
    mr_surface *s;
    png_bytep *rows;

    fp = mr_vfs_open(path);
    if (!fp) {
        mr_log("png: cannot open %s", path);
        return NULL;
    }

    png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL,
                                 png_err, png_warn);
    if (!png) {
        mr_vfs_close(fp);
        return NULL;
    }

    /* The decode is the longest thing the engine does between two frames --
     * a couple of hundred milliseconds for a background on the target -- and
     * png_read_image below is a single call, so there is no natural place
     * inside it to come up for air. libpng's row-status callback is that
     * place. See mr_plat_service: on DOS it keeps the mouse pointer moving,
     * and the engine draws that pointer itself. */
    png_set_read_status_fn(png, row_done);

    info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, NULL, NULL);
        mr_vfs_close(fp);
        return NULL;
    }

    s = NULL;
    rows = NULL;

    /* libpng reports errors by longjmp'ing back here. */
    if (setjmp(png_jmpbuf(png))) {
        free(rows);
        mr_surface_free(s);
        png_destroy_read_struct(&png, &info, NULL);
        mr_vfs_close(fp);
        return NULL;
    }

    png_set_read_fn(png, fp, png_vfs_read);
    png_read_info(png, info);
    png_get_IHDR(png, info, &w, &h, &depth, &ctype, NULL, NULL, NULL);

    /* Normalise everything to 8-bit RGB with an alpha channel. */
    if (ctype == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);
    if (ctype == PNG_COLOR_TYPE_GRAY && depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (ctype == PNG_COLOR_TYPE_GRAY || ctype == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    if (depth == 16)
        png_set_strip_16(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png);
    else if (!(ctype & PNG_COLOR_MASK_ALPHA))
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);

    /* R,G,B,A -> B,G,R,A, giving us 0xAARRGGBB DWORDs directly. */
    png_set_bgr(png);

    /* At least one of the game's images is interlaced. png_read_image needs
     * this to combine the seven Adam7 passes; without it the decode silently
     * produces a partial image. */
    png_set_interlace_handling(png);

    png_read_update_info(png, info);

    if (png_get_rowbytes(png, info) != (png_size_t)w * 4) {
        mr_log("png: %s did not normalise to 32bpp (rowbytes %lu, w %lu)",
               path, (unsigned long)png_get_rowbytes(png, info),
               (unsigned long)w);
        longjmp(png_jmpbuf(png), 1);
    }

    s = mr_surface_new((int)w, (int)h);
    if (!s) {
        mr_log("png: out of memory for %s (%lux%lu)", path,
               (unsigned long)w, (unsigned long)h);
        longjmp(png_jmpbuf(png), 1);
    }

    rows = (png_bytep *)malloc((size_t)h * sizeof(png_bytep));
    if (!rows) {
        mr_log("png: out of memory for the row table of %s", path);
        longjmp(png_jmpbuf(png), 1);
    }
    for (i = 0; i < (int)h; i++)
        rows[i] = (png_bytep)(s->px + (size_t)i * s->pitch);

    png_read_image(png, rows);
    png_read_end(png, NULL);

    /* Record whether alpha is actually used, so mr_blit can take the memcpy
     * path for the many fully opaque backgrounds. This has to be read before
     * the png structs are destroyed. */
    s->has_alpha = ((ctype & PNG_COLOR_MASK_ALPHA) ||
                    png_get_valid(png, info, PNG_INFO_tRNS)) ? 1 : 0;

    /* ...and then ask the pixels, because the header only says an alpha
     * channel is *present*. 70 of this game's assets carry one that is 255
     * from corner to corner -- all 22 full-stage backgrounds among them,
     * since the art is exported as RGBA whether or not it needs to be -- and
     * each of those made mr_blit run its per-pixel loop to discover `a == 255`
     * and assign, instead of a memcpy per row. Measured at 43 against
     * 27 ns/px, which is 14.7ms on every blit of a 1280x720 background.
     *
     * The scan is one streaming pass, ~34ms for a full-stage image at the
     * ~113MB/s this machine reads at, against 14.7ms saved on *every* blit of
     * it: it pays for itself in 2.3 frames and the image is blitted on every
     * full redraw, every still frame and twice per transition frame. A
     * translucent sprite exits after its first row.
     *
     * Deliberately not logged. The evidence that it is working is in
     * `MR_PROF=1`: pixels move out of the scene's `blend` bucket and into
     * `copy`, which is the figure the change is about, and a line per image
     * load would bury it. */
    if (s->has_alpha && mr_surface_opaque(s))
        s->has_alpha = 0;

    free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    mr_vfs_close(fp);

    return s;
}

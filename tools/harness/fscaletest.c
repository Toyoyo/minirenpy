/* im.FactorScale.
 *
 *     image magic_portal2 = im.FactorScale("magic_portal.png", 1.0)
 *     image azami small   = im.FactorScale("azami figures.png", 0.46)
 *
 * im.py:1292-1355. The size is `int(w * factor)` -- truncated, not rounded --
 * the second factor defaults to the first, and `bilinear` chooses between
 * bilinear and nearest. Deprecated in 7.4.0 in favour of the zoom transform
 * properties, but the games were written before that and still use it.
 *
 * Until this existed the declaration was logged as an unsupported image
 * definition and *dropped*, so the name did not exist at all. Azami has 76 of
 * them and 74 are a factor of exactly 1.0 -- "Duplications with dummy
 * FactorScale", a way of getting several distinct image names for one file so
 * that five vampires can be on screen at once. Those 74 shows drew nothing.
 *
 * Two halves. The registry half reads the decoded surfaces directly, because
 * a size is exactly what is in question and a surface's w/h says it without
 * any placement arithmetic in the way. The stage half shows one and measures
 * it, which is what says the scaled pixels are the ones that get drawn.
 *
 * Runs on tools/harness/story, label fscale_show.
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

static void check(const char *label, int ok)
{
    printf("%-54s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

/* The decoded size of a name, which is the whole claim for most of these. */
static void check_size(const char *name, int w, int h)
{
    mr_surface *s;
    char label[96];
    int ok;

    mr_image_begin_frame();
    s = mr_image_get(name);
    sprintf(label, "'%s' decodes to %dx%d", name, w, h);
    ok = s && s->w == w && s->h == h;
    printf("%-54s %s", label, ok ? "ok" : "** FAIL **");
    if (s)
        printf("   (got %dx%d)\n", s->w, s->h);
    else
        printf("   (no surface)\n");
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

/* The bounding box of what differs from the empty stage, as zoomtest does it
 * and for the same reason: it is immune to the fixture background's two
 * shades and to anything a screen draws. */
static mr_u32 *g_ref;

typedef struct { int x0, y0, x1, y1; } box;

static box bbox(void)
{
    box b;
    int x, y;

    b.x0 = b.y0 = 1 << 20;
    b.x1 = b.y1 = -1;

    mr_scene_render(g_st, T);
    for (y = 0; y < 520; y++) {
        const mr_u32 *row = g_st->px + (size_t)y * g_st->pitch;
        const mr_u32 *ref = g_ref + (size_t)y * MR_STAGE_W;

        for (x = 0; x < MR_STAGE_W; x++) {
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

int main(int argc, char **argv)
{
    mr_script *s;
    int y;

    setbuf(stdout, NULL);
    if (argc < 2) {
        fprintf(stderr, "usage: fscaletest <story-dir>\n");
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

    /* --- the registry ---------------------------------------------------- */

    /* After mr_interp_start, not after mr_interp_init: the pass that hands
     * every `image` statement to the registry is in start (interp.c:1990 and
     * on), because a declaration may sit anywhere in any file and all of them
     * have to exist before the first line of story runs. Asking before that
     * finds an empty table, which looks exactly like a dropped declaration --
     * it is how this test first "failed". */
    mr_interp_start("fscale_show");

    /* First that the names exist at all. This is the Azami bug by itself: a
     * dropped declaration is not a wrongly sized image, it is no image, and
     * every `show` of it draws nothing. */
    check("'fs half' is a declared name", mr_image_exists("fs half"));
    check("'fs wide' is a declared name", mr_image_exists("fs wide"));
    check("'fs dup' is a declared name", mr_image_exists("fs dup"));
    check("'fs bare' is a declared name", mr_image_exists("fs bare"));
    check("'fs nearest' is a declared name", mr_image_exists("fs nearest"));
    check("'fs trunc' is a declared name", mr_image_exists("fs trunc"));
    check("'fs flip' is a declared name", mr_image_exists("fs flip"));

    /* And that a factor of zero is refused rather than clamped to one pixel,
     * which would hide a typo as a sprite nobody can see. */
    check("'fs zero' is refused", !mr_image_exists("fs zero"));

    /* The sizes. zoomband.png is 64x64 and fringe.png is 64x64. */
    check_size("fs half", 32, 32);          /* one factor, applied to both */
    check_size("fs wide", 128, 64);         /* two factors, x only */
    check_size("fs dup", 64, 64);           /* the 1.0 alias */
    check_size("fs bare", 32, 32);          /* no images/ prefix */
    check_size("fs nearest", 32, 32);       /* bilinear=False, ignored */
    check_size("fs flip", 32, 32);          /* nested im.Flip */

    /* 64 * 0.51 is 32.64: truncated it is 32, rounded it would be 33. The one
     * assertion that says which. */
    check_size("fs trunc", 32, 32);

    /* The nested flip has to survive the scaling. fringe.png is opaque on its
     * left half, so flipped it is opaque on the right -- read off the decoded
     * surface rather than the stage so that no placement is involved. */
    {
        mr_surface *f;

        mr_image_begin_frame();
        f = mr_image_get("fs flip");
        if (!f || f->w < 4) {
            check("the nested im.Flip is still a flip", 0);
        } else {
            mr_u32 left = f->px[(size_t)(f->h / 2) * f->pitch + 2];
            mr_u32 right = f->px[(size_t)(f->h / 2) * f->pitch + f->w - 3];

            printf("  fs flip: left alpha %d, right alpha %d\n",
                   MR_A(left), MR_A(right));
            check("the nested im.Flip is still a flip",
                  MR_A(left) == 0 && MR_A(right) == 255);
        }
    }

    /* A name with no manipulator at all must be untouched by any of this. */
    check_size("band", 64, 64);

    /* --- the stage ------------------------------------------------------- */

    /* One of them actually shown, which is what says the scaled surface is
     * the one that reaches the stage rather than merely existing in the
     * registry. zoomspot is xoffset 400 / yoffset 100. */
    reach("Empty stage.");
    settle(300);
    mr_scene_render(g_st, T);
    for (y = 0; y < MR_STAGE_H; y++)
        memcpy(g_ref + (size_t)y * MR_STAGE_W,
               g_st->px + (size_t)y * g_st->pitch,
               (size_t)MR_STAGE_W * sizeof(mr_u32));

    reach("Scaled by a half.");
    settle(300);
    {
        box b = bbox();

        printf("  shown: x %d..%d  y %d..%d\n", b.x0, b.x1, b.y0, b.y1);
        check("a scaled image is drawn at its scaled size",
              b.x0 == 400 && b.x1 == 431 && b.y0 == 100 && b.y1 == 131);
    }

    /* And the flipped one, whose visible half is on the right of its box: the
     * box starts 16 pixels into the 32-wide image. */
    reach("Scaled and flipped.");
    settle(300);
    {
        box b = bbox();

        printf("  shown: x %d..%d  y %d..%d\n", b.x0, b.x1, b.y0, b.y1);
        check("a scaled flip is drawn flipped",
              b.x0 == 416 && b.x1 == 431);
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

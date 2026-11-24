/* Transform `alpha`.
 *
 *     show credits_names at left:
 *         alpha 0.5
 *
 * Ren'Py's `add_property("alpha", float, 1.0)` (display/transform.py:1351):
 * a 0..1 multiplier on the image's own alpha, applied to the composited
 * result. Azami writes it 182 times in its script alone, always as a ghost --
 * `show X at <pos> with dissolve:` + `alpha 0.3` -- and until now the
 * property was parsed into nothing and every one of them drew opaque.
 *
 * Measured off the stage rather than off the place struct. A field says what
 * the engine intends; only the pixel says whether the blitter was asked.
 *
 * Runs on tools/harness/story, label alpha_show.
 */
#include <stdarg.h>
#include <stdio.h>
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

/* The fixture sprite is 300x600 of flat (230,210,200), parked by `here940` at
 * x 940..1240, y 120..720. (1150,300) is inside it and in the flat part of
 * dark.png -- the background has two shades and the darker one covers the
 * middle of the stage. */
#define PX_X 1150
#define PX_Y 300

static int px(int x, int y)
{
    mr_scene_render(g_st, T);
    return (int)g_st->px[y * g_st->pitch + x];
}

static int chan(int v, int shift) { return (v >> shift) & 255; }

static void show_px(const char *label, int v)
{
    printf("  %-44s (%3d,%3d,%3d)\n", label,
           chan(v, 16), chan(v, 8), chan(v, 0));
}

static void check(const char *label, int got, int want)
{
    int ok = (got == want);
    printf("%-50s (%3d,%3d,%3d) want (%3d,%3d,%3d)  %s\n", label,
           chan(got, 16), chan(got, 8), chan(got, 0),
           chan(want, 16), chan(want, 8), chan(want, 0),
           ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

/* Strictly between the two, on every channel. The exact number depends on
 * mr_blit_alpha's rounding, and pinning it would make this a test of the
 * blitter rather than of whether the blitter was reached. */
static void check_mixed(const char *label, int got, int lo, int hi)
{
    int i, ok = 1;

    for (i = 0; i <= 16; i += 8) {
        int g = chan(got, i), a = chan(lo, i), b = chan(hi, i);
        int t = a < b ? a : b, u = a < b ? b : a;
        if (g <= t || g >= u)
            ok = 0;
    }
    printf("%-50s (%3d,%3d,%3d) want strictly between      %s\n", label,
           chan(got, 16), chan(got, 8), chan(got, 0),
           ok ? "ok" : "** FAIL **");
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

int main(int argc, char **argv)
{
    mr_script *s;
    int opaque, bg;

    setbuf(stdout, NULL);
    if (argc < 2) {
        fprintf(stderr, "usage: alphatest <story-dir>\n");
        return 2;
    }

    mr_vfs_init(NULL, argv[1]);
    mr_image_init(argv[1]);
    mr_scene_init();
    s = mr_script_load(argv[1]);
    mr_interp_init(s, argv[1]);
    mr_ui_init(argv[1]);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);
    mr_interp_start("alpha_show");

    /* The two reference colours, read off the stage rather than written down:
     * the fixture images can be regenerated and this test should follow. */
    reach("Opaque.");
    settle(300);
    opaque = px(PX_X, PX_Y);
    bg = px(40, PX_Y);
    show_px("sprite, no alpha given", opaque);
    show_px("background beside it", bg);
    if (opaque == bg) {
        printf("fixture is useless: the sprite and the background match\n");
        return 1;
    }

    reach("Half transparent.");
    settle(300);
    check_mixed("alpha 0.5 blends with the background",
                px(PX_X, PX_Y), bg, opaque);

    /* A different value, written `.3`. Two things at once: that a leading
     * dot parses (strtod takes it, and it is how Azami writes it), and that
     * the blend is computed from the number rather than picked from a few
     * known cases -- it has to land nearer the background than 0.5 did. */
    {
        int half = px(PX_X, PX_Y);     /* still the 0.5 frame */
        int third;

        reach("Nearly gone.");
        settle(300);
        third = px(PX_X, PX_Y);
        check_mixed("alpha .3 blends too", third, bg, opaque);
        printf("%-50s %s\n", "alpha .3 is nearer the background than 0.5",
               (chan(third, 16) < chan(half, 16)) ? "ok" : "** FAIL **");
        if (chan(third, 16) >= chan(half, 16))
            fails++;
    }

    /* The ends of the range. Both are short-circuits in mr_blit_alpha, so
     * they exercise different code from the blend above. */
    reach("Invisible.");
    settle(300);
    check("alpha 0.0 draws nothing at all", px(PX_X, PX_Y), bg);

    reach("Opaque again.");
    settle(300);
    check("alpha 1.0 is the opaque sprite", px(PX_X, PX_Y), opaque);

    reach("Half transparent again.");
    settle(300);
    check_mixed("half transparent before the re-show",
                px(PX_X, PX_Y), bg, opaque);

    /* `show person other` says nothing about alpha. Ren'Py carries transform
     * state across a re-show that only changes the picture
     * (config.keep_running_transform / take_state), and alpha is state. */
    reach("Still half transparent.");
    settle(300);
    check_mixed("a re-show that says nothing keeps the alpha",
                px(PX_X, PX_Y), bg, opaque);

    /* The timed fade. Sampled in flight, because the endpoints alone cannot
     * tell a fade from a jump -- see NOTES.md on the move that arrived
     * without travelling. */
    reach("Fading out.");
    settle(100);
    check_mixed("linear alpha is part-way through",
                px(PX_X, PX_Y), bg, opaque);
    {
        int i;
        printf("  fade:");
        for (i = 0; i < 6; i++) {
            printf(" %d", chan(px(PX_X, PX_Y), 16));
            settle(100);
        }
        printf(" %d\n", chan(px(PX_X, PX_Y), 16));
    }
    check("linear alpha reaches zero", px(PX_X, PX_Y), bg);

    mr_surface_free(g_st);
    mr_scene_shutdown();
    mr_image_shutdown();
    mr_vfs_shutdown();
    mr_script_free(s);

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}

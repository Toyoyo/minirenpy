/* A second font face on part of the interface.
 *
 * A game may name more than one font. Hinata does:
 *
 *     define gui.text_font           = "DejaVuSans.ttf"
 *     define gui.interface_text_font = "Pecita-0owz.otf"
 *
 * and style.c's expand_text has always resolved that into the `font` property
 * of every interface style -- the value was computed and then dropped on the
 * floor, because the renderer's font provider was keyed on point size alone.
 * Everything came out in DejaVu.
 *
 * What is tested here is the whole path, from the define to the pixels:
 *
 *   - mkfont.py finds the face in gui.rpy and bakes it (fonts.lst `face`
 *     lines, font-<slug><size>.dat)
 *   - ui.c loads those atlases and matches one by the filename the script
 *     named
 *   - slrender.c reads the `font` property and asks for that face
 *   - font.c falls back to the default atlas for a glyph the named face does
 *     not have
 *
 * The method throughout is to render the same word at the same size through
 * different routes and compare the pixels. Identical text in two faces must
 * come out *different*; the same face reached two ways must come out the
 * same. A broken provider makes every row identical, which is what this would
 * have reported before any of it existed.
 *
 * Run against tools/harness/faces, whose gui.rpy names DejaVuSerif as its
 * interface face. Rebake it with
 *
 *     python3 tools/mkfont.py tools/harness/faces tools/harness/faces/fonts
 *
 * if the fixture's charset or sizes change.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mr.h"
#include "expr.h"
#include "font.h"
#include "interp.h"
#include "vfs.h"
#include "script.h"
#include "image.h"
#include "scene.h"
#include "slrender.h"
#include "ui.h"

static mr_surface *g_st;
mr_surface *mr_stage(void) { return g_st; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); }
void mr_log(const char *f, ...) { (void)f; }
void mr_quit(void) {}
void mr_invalidate(void) {}
static void sink(const short *d, int n) { (void)d; (void)n; }
void (*hostshim_wave_sink)(const short *, int) = sink;

static int fails;

static int screen_is_up(const char *name)
{
    int i, n = mr_scene_screen_count();

    for (i = 0; i < n; i++) {
        const char *s = mr_scene_screen_name(i);
        if (s && !strcmp(s, name))
            return 1;
    }
    return 0;
}

static void check(const char *label, int ok)
{
    printf("%-56s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

/* --- reading the rendered rows ------------------------------------------- */

/* The screen puts one text at each of these y positions; see the fixture's
 * script.rpy. A band is 40 tall, which is more than a 26px line needs. */
#define ROW_H        40
#define ROW_EXPLICIT   0
#define ROW_STYLED    40
#define ROW_INHERITED 80
#define ROW_DEFAULT  120
#define ROW_STAR_FACE   160
#define ROW_STAR_DEFAULT 200

/* How many pixels in one row band are not background. A row that drew nothing
 * has none, which is a different failure from a row that drew the wrong
 * shapes and has to be told apart from it. */
static long row_ink(int top)
{
    int x, y;
    long n = 0;
    mr_u32 bg = g_st->px[0];

    for (y = top; y < top + ROW_H && y < MR_STAGE_H; y++) {
        for (x = 0; x < MR_STAGE_W; x++) {
            if (g_st->px[y * g_st->pitch + x] != bg)
                n++;
        }
    }
    return n;
}

/* Pixels where two row bands differ. */
static long rows_differ(int a, int b)
{
    int x, y;
    long n = 0;

    for (y = 0; y < ROW_H; y++) {
        if (a + y >= MR_STAGE_H || b + y >= MR_STAGE_H)
            break;
        for (x = 0; x < MR_STAGE_W; x++) {
            if (g_st->px[(a + y) * g_st->pitch + x] !=
                g_st->px[(b + y) * g_st->pitch + x])
                n++;
        }
    }
    return n;
}

static void dump_ppm(const mr_surface *s, const char *path)
{
    FILE *f = fopen(path, "wb");
    int x, y;

    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", s->w, s->h);
    for (y = 0; y < s->h; y++) {
        for (x = 0; x < s->w; x++) {
            mr_u32 v = s->px[y * s->pitch + x];
            fputc((int)MR_R(v), f);
            fputc((int)MR_G(v), f);
            fputc((int)MR_B(v), f);
        }
    }
    fclose(f);
}

/* --- what mkfont.py produced -------------------------------------------- */

/* The manifest is the contract between the baker and ui.c, and it is plain
 * text, so the test can read it rather than infer it from behaviour. */
static void manifest_lists_the_face(void)
{
    char *text;
    long len;
    int sizes = 0, faces = 0;
    const char *p;

    printf("\n-- fonts.lst --\n");

    text = mr_vfs_read_all("fonts/fonts.lst", &len);
    if (!text) {
        check("fonts/fonts.lst is there (rebake the fixture?)", 0);
        return;
    }

    for (p = text; *p; ) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        if (*p >= '0' && *p <= '9')
            sizes++;
        else if (!strncmp(p, "face ", 5))
            faces++;
        while (*p && *p != '\n')
            p++;
    }

    printf("   %d default sizes, %d face lines\n", sizes, faces);
    check("the manifest lists the default face's sizes", sizes > 0);
    check("and a face line per baked size of the named face", faces > 0);
    /* The phone variant block in the fixture's gui.rpy sets 34 and 36. They
     * are in the older `if renpy.variant("small"):` spelling, which the size
     * scrape did not recognise -- Hinata baked four atlases nothing read,
     * about 1.1MB, and loaded them resident at startup. */
    check("and nothing from the per-device variant block (4 sizes, not 6)",
          sizes == 4);
    free(text);
}

/* --- the faces are distinct --------------------------------------------- */

/* Both atlases exist and are genuinely different fonts. If the fixture were
 * baked with one face twice, every pixel comparison below would pass with the
 * provider doing nothing at all. */
static void the_two_faces_differ(void)
{
    mr_font *dflt = mr_ui_font(26);
    mr_font *face = mr_ui_font_named("DejaVuSerif.ttf", 26);
    int wd, wf;

    printf("\n-- the fixture has two distinct faces --\n");

    check("the default face is loaded", dflt != NULL);
    check("the named face is loaded", face != NULL);
    if (!dflt || !face)
        return;

    check("and they are not the same atlas", dflt != face);

    wd = mr_font_text_width(dflt, "Hamburgefonstiv");
    wf = mr_font_text_width(face, "Hamburgefonstiv");
    printf("   same word at 26px: sans %dpx, serif %dpx\n", wd, wf);
    check("the same word measures differently in each", wd != wf);

    /* A name no face has must not resolve to one that exists. */
    check("an unknown face name gives the default",
          mr_ui_font_named("NoSuchFont.ttf", 26) == dflt);
    check("and so does a NULL one", mr_ui_font_named(NULL, 26) == dflt);

    /* Case: the pak lowercases its names, gui.rpy need not. */
    check("the match is case-insensitive",
          mr_ui_font_named("dejavuserif.ttf", 26) == face);

    /* Nearest-size, as for the default face: the fixture bakes 14/22/26/30,
     * so 27 has to land on 26 rather than on nothing. */
    check("a size between baked ones picks the nearest in the face",
          mr_ui_font_named("DejaVuSerif.ttf", 27) == face);
}

/* The glyph a named face does not have comes from the default atlas.
 *
 * DejaVuSerif has no U+2605 BLACK STAR; DejaVuSans does. mkfont.py leaves it
 * out of the serif atlas (it reports "2 uncovered" baking the fixture) and
 * ui.c points the serif atlas at the sans one of the same size. Without that
 * chain the star would measure zero and draw nothing -- which is exactly what
 * Pecita would have done to every kanji in Hinata's interface. */
static void an_uncovered_glyph_falls_back(void)
{
    mr_font *dflt = mr_ui_font(26);
    mr_font *face = mr_ui_font_named("DejaVuSerif.ttf", 26);
    int wd, wf, hd, hf;

    printf("\n-- a glyph the named face lacks --\n");
    if (!dflt || !face)
        return;

    wd = mr_font_text_width(dflt, "\xe2\x98\x85");       /* U+2605 */
    wf = mr_font_text_width(face, "\xe2\x98\x85");
    printf("   U+2605 advance: default %dpx, named face %dpx\n", wd, wf);

    check("the default face has the star", wd > 0);
    check("and the named face finds it too, through its fallback", wf > 0);
    check("with the same advance, because it is the same glyph", wd == wf);

    /* A letter both faces have must *not* come from the fallback, or the
     * chain would be defeating the point of having a second face at all. */
    hd = mr_font_text_width(dflt, "H");
    hf = mr_font_text_width(face, "H");
    printf("   'H' advance:    default %dpx, named face %dpx\n", hd, hf);
    check("a letter the named face has stays in the named face", hd != hf);
}

/* --- the pixels --------------------------------------------------------- */

static void the_screen_uses_the_faces(void)
{
    long ink_explicit, ink_styled, ink_inherited, ink_default;
    long ink_star_face, ink_star_default;

    printf("\n-- the rendered screen --\n");

    ink_explicit  = row_ink(ROW_EXPLICIT);
    ink_styled    = row_ink(ROW_STYLED);
    ink_inherited = row_ink(ROW_INHERITED);
    ink_default   = row_ink(ROW_DEFAULT);
    ink_star_face    = row_ink(ROW_STAR_FACE);
    ink_star_default = row_ink(ROW_STAR_DEFAULT);

    printf("   ink: explicit %ld, styled %ld, inherited %ld, default %ld\n",
           ink_explicit, ink_styled, ink_inherited, ink_default);
    printf("   ink: star via face %ld, star via default %ld\n",
           ink_star_face, ink_star_default);

    /* Nothing below means anything if the rows did not draw. */
    check("every row drew something",
          ink_explicit > 0 && ink_styled > 0 && ink_inherited > 0 &&
          ink_default > 0 && ink_star_face > 0 && ink_star_default > 0);

    /* The point of the whole change: a `font` on the node selects a face. */
    check("`font` on the node changes the pixels",
          rows_differ(ROW_EXPLICIT, ROW_DEFAULT) > 0);

    /* The route Hinata actually takes: gui.interface_text_font, through
     * style.c's expand_text into the style's `font` property. */
    check("gui.interface_text_font reaches the style",
          rows_differ(ROW_STYLED, ROW_DEFAULT) > 0);
    check("and gives the same face as naming it on the node",
          rows_differ(ROW_STYLED, ROW_EXPLICIT) == 0);

    /* A style with no font of its own, parented to one that has: this is
     * Hinata's quick_button_text, and it is why a named face is baked at
     * every size rather than only at its own kind's size. */
    check("a style inherits its parent's face",
          rows_differ(ROW_INHERITED, ROW_EXPLICIT) == 0);
    check("which is not merely the default face",
          rows_differ(ROW_INHERITED, ROW_DEFAULT) > 0);

    /* And the fallback, in pixels rather than in advances. */
    check("an uncovered glyph draws through the fallback",
          rows_differ(ROW_STAR_FACE, ROW_STAR_DEFAULT) == 0);
}

int main(int argc, char **argv)
{
    mr_script *s;
    long t = 0;
    int i;

    setbuf(stdout, NULL);
    if (argc < 2) {
        fprintf(stderr, "usage: facetest <game-dir>\n");
        return 2;
    }

    mr_vfs_init(NULL, argv[1]);
    mr_image_init(argv[1]);
    mr_scene_init();
    s = mr_script_load(argv[1]);
    mr_interp_init(s, argv[1]);
    mr_ui_init(argv[1]);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);

    manifest_lists_the_face();

    /* After the init phase, so the gui.* defines are live: mr_ui_init runs
     * before the script is parsed and can only see Ren'Py's defaults. */
    mr_ui_sync_fonts();
    mr_ui_sync_geometry();

    the_two_faces_differ();
    an_uncovered_glyph_falls_back();

    /* Walk until `show screen faces` has run. Instant text, so a line does
     * not have to be waited out. */
    mr_interp_start("start");
    for (i = 0; i < 40; i++) {
        mr_interp_update(t);
        t += 33;
        mr_interp_advance();
        if (screen_is_up("faces"))
            break;
    }
    check("the fixture's screen is up", screen_is_up("faces"));

    mr_clip_reset();
    mr_interp_render(g_st, t);
    the_screen_uses_the_faces();

    dump_ppm(g_st, "/tmp/facetest.ppm");

    printf("\n%s (%d failure%s); stage in /tmp/facetest.ppm\n",
           fails ? "FAILED" : "all ok", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}

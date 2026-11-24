/* ui.c - the dialogue box and menus.
 *
 * Geometry is read from the game's own gui.* defines at startup, by
 * mr_ui_sync_geometry and mr_ui_sync_fonts -- not transcribed here. Both
 * games happen to share these values, which is why this file carried them as
 * constants for so long without anything going wrong:
 *
 *   gui.textbox_height  185      gui.dialogue_xpos   268
 *   gui.textbox_yalign  1.0      gui.dialogue_ypos    50
 *   gui.name_xpos       240      gui.dialogue_width  744
 *   gui.text_size        22      gui.name_text_size   30
 *   gui.text_color  #ffffff
 *
 * They survive only as the fallbacks used when a game defines nothing.
 *
 * `style window` uses background Image(), not Frame(), so gui/textbox.png is
 * placed at its natural 800x185 rather than stretched across the stage; the
 * namebox really is a Frame and is nine-sliced.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mr.h"
#include "ui.h"
#include "font.h"
#include "image.h"
#include "interp.h"
#include "expr.h"
#include "scene.h"
#include "style.h"
#include "slrender.h"
#include "vfs.h"       /* fonts/fonts.lst */
#include "lex.h"       /* mr_read_string, for style background paths */
#include "bandcache.h" /* dropped when the window's geometry is re-read */

/* The say window's geometry, read from the game's own gui.* defines.
 *
 * These were hardcoded to the two games' shared values, with a comment at the
 * top of this file claiming they came from gui.rpy -- they had been
 * transcribed by hand, which is not the same thing. Both games happen to
 * agree on all seven, so nothing ever went wrong; a third game with a
 * different gui.textbox_height would have had its dialogue drawn in the right
 * place and *repainted* in the wrong one, because mr_ui_dialogue_rect derives
 * the partial-redraw band from the same numbers. That shows as stale pixels
 * along the top of the box while a line types itself out, and only then.
 *
 * Exactly the same bug was already found and fixed one field over, for
 * gui.text_size and gui.name_text_size (see mr_ui_sync_fonts) -- the
 * geometry simply got left behind.
 *
 * The values below are the defaults, used when a game does not define the
 * variable at all. They are Ren'Py's own defaults from its gui template,
 * which is where both games' numbers come from. */
#define UI_TEXTBOX_H     185
#define UI_TEXTBOX_YALIGN 1.0
#define UI_DIALOGUE_X    268
#define UI_DIALOGUE_Y     50
#define UI_DIALOGUE_W    744
#define UI_NAME_X        240
#define UI_NAME_Y          0
#define UI_NAME_PAD        5      /* gui.namebox_borders = Borders(5,5,5,5) */

/* Resolved once by mr_ui_sync_geometry, after the init phase has run the
 * `define`s. Initialised to the defaults so that anything reading them before
 * then -- or a harness that never calls it -- behaves exactly as the
 * hardcoded version did. */
static struct {
    int   textbox_h;
    float textbox_yalign;
    int   dialogue_x, dialogue_y, dialogue_w;
    int   name_x, name_y;
    int   pad_l, pad_t, pad_r, pad_b;
} g_geom = {
    UI_TEXTBOX_H, (float)UI_TEXTBOX_YALIGN,
    UI_DIALOGUE_X, UI_DIALOGUE_Y, UI_DIALOGUE_W,
    UI_NAME_X, UI_NAME_Y,
    UI_NAME_PAD, UI_NAME_PAD, UI_NAME_PAD, UI_NAME_PAD
};

/* Top edge of the say window. gui.textbox_yalign is a *fraction*, 1.0 meaning
 * the bottom of the stage, so this is the alignment arithmetic rather than an
 * assumption that the box sits at the bottom -- which is what
 * `MR_STAGE_H - UI_TEXTBOX_H` was. */
static int textbox_top(void)
{
    return (int)((float)(MR_STAGE_H - g_geom.textbox_h) *
                 g_geom.textbox_yalign);
}

static void draw_screen(mr_surface *dst, const char *name, int interactive);

/* The baked text sizes, read from fonts/fonts.lst, which tools/mkfont.py
 * writes with the sizes it baked -- and it takes those from the game's own
 * `gui.*_text_size` defines. So the set follows the script and no list here
 * has to be kept in step with one there.
 *
 * A style can name any size; mr_ui_font picks the nearest baked one, which is
 * why a wrong list was never an error and so never got noticed. The fallback
 * below is what this file used to carry, for a game whose fonts were baked
 * before the manifest existed. */
#define UI_MAX_FONTS 24
static int         g_font_sizes[UI_MAX_FONTS];
static int         g_nfonts;
static mr_font    *g_fonts[UI_MAX_FONTS];

/* Named faces: a game may put a second font on part of its interface, as
 * Hinata's `define gui.interface_text_font = "Pecita-0owz.otf"` does.
 * style.c's expand_text already resolves that into the `font` property of
 * every interface style, so what is needed here is the atlases and a way to
 * match on the filename the script named.
 *
 * `file` is that filename verbatim, which is why nothing here has to know how
 * mkfont.py spelled the slug in the path. Two is enough for both games; a
 * third face logs and is ignored rather than growing a malloc'd table for
 * something no GUI template does. */
#define UI_MAX_FACES 3
typedef struct {
    char     file[64];
    int      nsizes;
    int      sizes[UI_MAX_FONTS];
    mr_font *fonts[UI_MAX_FONTS];
} ui_face;
static ui_face g_faces[UI_MAX_FACES];
static int     g_nfaces;

/* Index of the face `name` refers to, or -1 for the default face. Matched
 * case-insensitively on the filename, because the pak lowercases its names
 * and gui.rpy need not. */
static int face_index(const char *name)
{
    int i;

    if (!name || !*name)
        return -1;
    for (i = 0; i < g_nfaces; i++) {
        const char *a = g_faces[i].file, *b = name;
        while (*a && *b) {
            int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
            int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
            if (ca != cb)
                break;
            a++;
            b++;
        }
        if (!*a && !*b)
            return i;
    }
    return -1;
}

/* One `face <slug> <size> <file>` line. The slug is skipped: it only exists
 * so the atlas can have a filename, and the engine derives the path from it
 * the same way mkfont.py built it. */
static void add_face_size(const char *line)
{
    char slug[32], file[64];
    int size = 0, i;
    const char *p = line;

    while (*p == ' ' || *p == '\t')
        p++;
    i = 0;
    while (*p && *p != ' ' && *p != '\t' && i < (int)sizeof(slug) - 1)
        slug[i++] = *p++;
    slug[i] = '\0';

    while (*p == ' ' || *p == '\t')
        p++;
    size = atoi(p);
    while (*p && *p != ' ' && *p != '\t')
        p++;
    while (*p == ' ' || *p == '\t')
        p++;
    i = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' &&
           i < (int)sizeof(file) - 1)
        file[i++] = *p++;
    file[i] = '\0';

    if (!slug[0] || !file[0] || size <= 0)
        return;

    i = face_index(file);
    if (i < 0) {
        if (g_nfaces >= UI_MAX_FACES) {
            mr_log("ui: more than %d named faces; ignoring %s",
                   UI_MAX_FACES, file);
            return;
        }
        i = g_nfaces++;
        strncpy(g_faces[i].file, file, sizeof(g_faces[i].file) - 1);
        g_faces[i].file[sizeof(g_faces[i].file) - 1] = '\0';
        g_faces[i].nsizes = 0;
    }
    if (g_faces[i].nsizes < UI_MAX_FONTS) {
        int k = g_faces[i].nsizes++;
        char path[320];

        g_faces[i].sizes[k] = size;
        sprintf(path, "fonts/font-%s%d.dat", slug, size);
        g_faces[i].fonts[k] = mr_font_load(path);
    }
}

static void load_font_sizes(void)
{
    static const int fallback[] = { 14, 16, 20, 22, 24, 30, 36, 50 };
    char *text;
    long len;
    int i;

    g_nfonts = 0;
    g_nfaces = 0;

    text = mr_vfs_read_all("fonts/fonts.lst", &len);
    if (text) {
        const char *p = text;

        while (*p) {
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
                p++;
            if (*p >= '0' && *p <= '9') {
                int size = atoi(p);
                if (size > 0 && g_nfonts < UI_MAX_FONTS)
                    g_font_sizes[g_nfonts++] = size;
            } else if (!strncmp(p, "face ", 5)) {
                add_face_size(p + 5);
            }
            /* Anything else on the line -- the comment header, chiefly -- is
             * skipped wholesale rather than parsed. An engine built before
             * faces existed took this branch for them too, which is why the
             * manifest is still readable by one. */
            while (*p && *p != '\n')
                p++;
        }
        free(text);
    }

    if (g_nfonts == 0) {
        mr_log("ui: no fonts/fonts.lst; falling back to the built-in sizes");
        for (i = 0; i < (int)(sizeof(fallback) / sizeof(fallback[0])); i++)
            g_font_sizes[g_nfonts++] = fallback[i];
    }
}

static mr_font    *g_body;
static mr_font    *g_name;
static mr_surface *g_textbox;
static mr_surface *g_namebox;

/* Where the textbox art is placed inside the window.
 *
 * `style window` gives it yalign 1.0, so it sits on the window's bottom edge
 * rather than filling it -- the art keeps its natural size and is merely
 * placed. With the usual gui.textbox_height that is a no-op, because the two
 * are the same height.
 *
 * A window *shorter* than its art is the interesting case: the art would then
 * be placed above the window's top and so outside the partial-redraw band,
 * which is the one thing that must never happen -- the band is what gets
 * repainted while a line types, and anything drawn outside it is left stale
 * on screen. No shipped game is configured that way, but the band is derived
 * from a script-supplied number and an invariant that holds only for
 * well-configured input is not an invariant. So the band is widened to cover
 * the art (see mr_ui_dialogue_rect) rather than the art being clipped: the
 * art is what the player is meant to see, and a larger band costs a little
 * speed where a clipped box would be a visible defect. */
static int textbox_art_top(int top)
{
    if (!g_textbox)
        return top;
    return top + (g_geom.textbox_h - g_textbox->h);
}

/* The say window's art. Ren'Py takes both from style properties in the
 * script --
 *
 *     style window:  background Image("gui/textbox.png", ...)
 *     style namebox: background Frame("gui/namebox.png", ...)
 *
 * -- so they are configurable, and these are only the conventional paths the
 * standard GUI uses. The screen layer already reads `background` off a style
 * (slrender.c, draw_background); the say window does not go through it,
 * because it is drawn natively rather than as a screen, so it has to ask for
 * itself. */
#define UI_TEXTBOX_DEFAULT "gui/textbox.png"
#define UI_NAMEBOX_DEFAULT "gui/namebox.png"

static char g_textbox_path[MR_PATH_MAX];
static char g_namebox_path[MR_PATH_MAX];

static void load_box_art(const char *textbox, const char *namebox)
{
    if (strcmp(g_textbox_path, textbox)) {
        mr_surface_free(g_textbox);
        g_textbox = mr_png_load(textbox);
        strncpy(g_textbox_path, textbox, sizeof(g_textbox_path) - 1);
        g_textbox_path[sizeof(g_textbox_path) - 1] = '\0';
    }
    if (strcmp(g_namebox_path, namebox)) {
        mr_surface_free(g_namebox);
        g_namebox = mr_png_load(namebox);
        strncpy(g_namebox_path, namebox, sizeof(g_namebox_path) - 1);
        g_namebox_path[sizeof(g_namebox_path) - 1] = '\0';
    }
}

/* The first filename in a style's `background`, or `dflt` if the style does
 * not set one. The property is held as its source text, so
 * `Image("gui/textbox.png", xalign=0.5)` and a bare `"gui/textbox.png"` both
 * reduce to the first quoted string in it -- which is also the right answer
 * for a Frame given a list, whose first entry is the preferred image. */
static int style_bg_path(const char *style, const char *dflt,
                         char *out, int outsz)
{
    const char *e = mr_style_get(style, "background");
    const char *q;

    out[0] = '\0';
    if (e) {
        for (q = e; *q; q++) {
            if (*q == '"' || *q == '\'') {
                if (mr_read_string(q, out, outsz) && out[0])
                    return 1;
                break;
            }
        }
    }
    strncpy(out, dflt, outsz - 1);
    out[outsz - 1] = '\0';
    return 0;
}

int mr_ui_init(const char *game_dir)
{
    char path[320];

    (void)game_dir;   /* the VFS resolves names */

    load_font_sizes();
    {
        int i, k;
        for (i = 0; i < g_nfonts; i++) {
            sprintf(path, "fonts/font%d.dat", g_font_sizes[i]);
            g_fonts[i] = mr_font_load(path);
        }

        /* Now that the default atlases exist, give every named face one to
         * fall back to at its own size. mkfont.py bakes a named face with
         * only the block it covers, so without this a kanji or a Thai
         * codepoint in an interface label would simply not draw. */
        for (i = 0; i < g_nfaces; i++) {
            for (k = 0; k < g_faces[i].nsizes; k++) {
                mr_font_set_fallback(g_faces[i].fonts[k],
                                     mr_ui_font(g_faces[i].sizes[k]));
            }
            mr_log("ui: face '%s', %d size%s", g_faces[i].file,
                   g_faces[i].nsizes, g_faces[i].nsizes == 1 ? "" : "s");
        }
    }

    /* Provisional: the real sizes are gui.text_size and gui.name_text_size,
     * and the script has not been parsed yet -- mr_ui_init runs before
     * mr_script_load. mr_ui_sync_fonts re-resolves them once the init phase
     * has run. These defaults are Ren'Py's own (gui.rpy's template values),
     * and the nearest-match means they land somewhere sensible regardless. */
    g_body = mr_ui_font(22);
    g_name = mr_ui_font(30);

    if (!g_body || !g_name) {
        mr_log("ui: fonts missing; run tools/mkfont.py to build them");
        return 0;
    }

    mr_scene_set_screen_drawer(draw_screen);
    mr_sl_init();
    mr_sl_set_font_provider(mr_ui_font_named);
    mr_sl_set_screen_lookup(mr_interp_find_screen);

    /* The box art is optional: without it the text still reads against a
     * plain darkened strip.
     *
     * Ren'Py's defaults, which is all that can be known here -- the script
     * that may name something else has not been parsed yet. mr_ui_sync_fonts
     * reloads them afterwards if it did. */
    load_box_art(UI_TEXTBOX_DEFAULT, UI_NAMEBOX_DEFAULT);

    return 1;
}

/* Take the dialogue and name sizes from the script, once the init phase has
 * evaluated the `define`s. Called from main.c after mr_interp_start, because
 * mr_ui_init runs before the script is even parsed.
 *
 * These were hardcoded 22 and 30 -- this game's values. Any other game with a
 * different gui.text_size got the nearest atlas to 22 regardless of what it
 * asked for. ui.c:342 already read gui.text_size this way for choice buttons,
 * so the dialogue box was the odd one out. */
/* A gui.* value that may be written as an int or a float. gui.textbox_yalign
 * is 1.0 in both games, but 1 is just as valid and mr_var_get_int would read
 * a float as whatever its integer field happens to hold. */
static float gui_float(const char *name, float dflt)
{
    mr_value v;

    if (!mr_var_get(name, &v))
        return dflt;
    if (v.type == MR_V_FLOAT)
        return (float)v.f;
    if (v.type == MR_V_INT)
        return (float)v.i;
    return dflt;
}

/* A gui.* value that is a string, copied out. 0 and an empty buffer if it is
 * unset or not a string. The mr_value is static for the same reason as
 * slrender.c's get_str: it is over a kilobyte. */
static int gui_str(const char *name, char *out, int outsz)
{
    static mr_value v;

    out[0] = '\0';
    if (!mr_var_get(name, &v) || v.type != MR_V_STR || !v.s[0])
        return 0;
    strncpy(out, v.s, (size_t)outsz - 1);
    out[outsz - 1] = '\0';
    return 1;
}

/* gui.namebox_borders, which `Borders(l, t, r, b)` leaves in the store as the
 * flattened string "l t r b" (interp.c:1507). Left at the defaults if it is
 * absent or unparseable -- a partly-read border is worse than the default,
 * because it would be asymmetric in a way no script asked for. */
static void read_borders(const char *name)
{
    mr_value v;
    int l, t, r, b;

    if (!mr_var_get(name, &v) || v.type != MR_V_STR)
        return;
    if (sscanf(v.s, "%d %d %d %d", &l, &t, &r, &b) != 4)
        return;
    if (l < 0 || t < 0 || r < 0 || b < 0)
        return;

    g_geom.pad_l = l;
    g_geom.pad_t = t;
    g_geom.pad_r = r;
    g_geom.pad_b = b;
}

/* The say window's geometry from the script, for the same reason and at the
 * same moment as the fonts below. See the note on g_geom. */
void mr_ui_sync_geometry(void)
{
    g_geom.textbox_h = (int)mr_var_get_int("gui.textbox_height",
                                           UI_TEXTBOX_H);
    g_geom.textbox_yalign = gui_float("gui.textbox_yalign",
                                      (float)UI_TEXTBOX_YALIGN);
    g_geom.dialogue_x = (int)mr_var_get_int("gui.dialogue_xpos",
                                            UI_DIALOGUE_X);
    g_geom.dialogue_y = (int)mr_var_get_int("gui.dialogue_ypos",
                                            UI_DIALOGUE_Y);
    g_geom.dialogue_w = (int)mr_var_get_int("gui.dialogue_width",
                                            UI_DIALOGUE_W);
    g_geom.name_x = (int)mr_var_get_int("gui.name_xpos", UI_NAME_X);
    g_geom.name_y = (int)mr_var_get_int("gui.name_ypos", UI_NAME_Y);
    read_borders("gui.namebox_borders");

    /* A textbox taller than the stage, or a negative one, would put the band
     * outside the surface. Clamped rather than trusted: these come from a
     * script, and mr_ui_dialogue_rect's result is used to clip a blit. */
    if (g_geom.textbox_h < 1)
        g_geom.textbox_h = 1;
    if (g_geom.textbox_h > MR_STAGE_H)
        g_geom.textbox_h = MR_STAGE_H;
    if (g_geom.textbox_yalign < 0.0f)
        g_geom.textbox_yalign = 0.0f;
    if (g_geom.textbox_yalign > 1.0f)
        g_geom.textbox_yalign = 1.0f;

    /* Any strip in hand was composited against the old numbers. The cache
     * keys on the rectangle, so a height change would miss on its own -- but
     * the window can also move or change art at the same position, and that
     * is invisible to it. Dropped here rather than at the caller so it holds
     * for whoever re-reads the geometry, which during a language or game
     * switch is not only startup. */
    mr_band_cache_drop();

    mr_log("ui: say window %dx%d at y=%d (gui.textbox_height %d, yalign "
           "%d.%02d), text at +%d,+%d w=%d, name at +%d,+%d",
           MR_STAGE_W, g_geom.textbox_h, textbox_top(),
           g_geom.textbox_h,
           (int)g_geom.textbox_yalign,
           (int)(g_geom.textbox_yalign * 100.0f) % 100,
           g_geom.dialogue_x, g_geom.dialogue_y, g_geom.dialogue_w,
           g_geom.name_x, g_geom.name_y);
}

void mr_ui_sync_fonts(void)
{
    int body = (int)mr_var_get_int("gui.text_size", 22);
    int name = (int)mr_var_get_int("gui.name_text_size", 30);
    char bface[64], nface[64];
    mr_font *fb, *fn;

    /* The faces as well as the sizes. Miwa names DejaVu for both, which is
     * the default face, so these resolve to exactly what mr_ui_font gave
     * before. A game that names something else for its dialogue gets it. */
    gui_str("gui.text_font", bface, (int)sizeof(bface));
    gui_str("gui.name_text_font", nface, (int)sizeof(nface));

    fb = mr_ui_font_named(bface[0] ? bface : NULL, body);
    fn = mr_ui_font_named(nface[0] ? nface : NULL, name);

    if (fb)
        g_body = fb;
    if (fn)
        g_name = fn;

    mr_log("ui: dialogue at gui.text_size %d (%s), name at %d (%s), from %d "
           "baked size%s", body, bface[0] ? bface : "default face",
           name, nface[0] ? nface : "default face",
           g_nfonts, g_nfonts == 1 ? "" : "s");

    /* And the say window's art, which the script's `style window` and
     * `style namebox` may point elsewhere. Only reloaded when the path
     * actually differs, so the usual case costs one strcmp each. */
    {
        char tb[128], nb[128];
        int tb_styled = style_bg_path("window", UI_TEXTBOX_DEFAULT,
                                      tb, sizeof(tb));
        int nb_styled = style_bg_path("namebox", UI_NAMEBOX_DEFAULT,
                                      nb, sizeof(nb));

        /* Logged either way, and saying *where* each came from: a script path
         * that happens to equal the default is indistinguishable in the
         * result from a style lookup that silently found nothing, and only
         * one of those is working. */
        mr_log("ui: textbox '%s' (%s), namebox '%s' (%s)",
               tb, tb_styled ? "style window" : "built-in default",
               nb, nb_styled ? "style namebox" : "built-in default");

        load_box_art(tb, nb);
    }
}

/* Nearest baked size out of a (sizes, fonts) pair. */
static mr_font *nearest(const int *sizes, mr_font *const *fonts, int n,
                        int size)
{
    int i, best = -1, bestd = 9999;

    for (i = 0; i < n; i++) {
        int d;
        if (!fonts[i])
            continue;
        d = sizes[i] - size;
        if (d < 0)
            d = -d;
        if (d < bestd) {
            bestd = d;
            best = i;
        }
    }
    return best >= 0 ? fonts[best] : NULL;
}

/* Nearest baked size, so a style asking for 28 gets 30 rather than nothing. */
mr_font *mr_ui_font(int size)
{
    return nearest(g_font_sizes, g_fonts, g_nfonts, size);
}

mr_font *mr_ui_font_named(const char *name, int size)
{
    int fi = face_index(name);
    mr_font *f;

    if (fi < 0)
        return mr_ui_font(size);

    f = nearest(g_faces[fi].sizes, g_faces[fi].fonts,
                g_faces[fi].nsizes, size);

    /* A face the manifest names but whose atlas failed to load is not an
     * error worth refusing to draw over: the text appears in the default
     * face, which is what it looked like before the game named a second one. */
    return f ? f : mr_ui_font(size);
}

void mr_ui_shutdown(void)
{
    int i;

    /* The faces first: each one's fallback points into g_fonts, and
     * mr_font_free deliberately does not follow it. */
    for (i = 0; i < g_nfaces; i++) {
        int k;
        for (k = 0; k < g_faces[i].nsizes; k++) {
            mr_font_free(g_faces[i].fonts[k]);
            g_faces[i].fonts[k] = NULL;
        }
        g_faces[i].nsizes = 0;
    }
    g_nfaces = 0;

    for (i = 0; i < g_nfonts; i++) {
        mr_font_free(g_fonts[i]);
        g_fonts[i] = NULL;
    }
    g_body = g_name = NULL;
    mr_band_cache_drop();
    mr_surface_free(g_textbox);
    mr_surface_free(g_namebox);
    g_textbox = g_namebox = NULL;
    g_textbox_path[0] = g_namebox_path[0] = '\0';
}

/* Point-sampled stretch of a source rectangle into a destination rectangle.
 *
 * `sx0 + x * sw / dw` used to sit in the inner loop, one `idiv` per
 * destination pixel: a variable divisor, so nothing a compiler could reduce,
 * and 36-40 non-pipelined cycles each on a Pentium III. The nine-slice frames
 * this draws are 48 calls a frame for the quick menu alone (1,440 over thirty
 * frames in MR_PROF), so it showed up in the `stretch` bucket at 98 ns/px.
 * Hoisted into a table built once per call, exactly as gfx.c's
 * mr_blit_stretch now does -- the arithmetic below is the expression that was
 * in the loop, not scale.c's centre-sampled one, so the pixels are unchanged.
 *
 * Its own table rather than a shared one: this is not mr_blit_stretch with
 * different arguments. It clips against the destination's own bounds and
 * ignores the clip stack, which is what the nine-slice needs and is a
 * difference worth keeping visible rather than unifying away.
 *
 * Static, not on the stack -- see NOTES.md on the working buffers that caused
 * the page fault.
 *
 * **Only the divide is touched.** The per-pixel destination-bounds test below
 * is hoistable into the loop bounds and was hoisted, and then put back: no
 * harness pins the pixels this function produces -- it is reached through
 * blit_frame for the namebox, and bandtest's comparison is of two stages that
 * would share any sampling bug -- so the only change made here is the one the
 * measurement asked for and the disassembly can confirm. Two compares a pixel
 * is not worth an unverifiable change to the loop bounds. */
static int g_ui_sxmap[MR_STAGE_W];

static void blit_rect_stretched(mr_surface *dst, const mr_surface *src,
                                int sx0, int sy0, int sw, int sh,
                                int dx, int dy, int dw, int dh)
{
    int y, x, sy, sx;
    const int *xmap = NULL;
    const mr_u32 *srow;
    mr_u32 *drow;
    mr_u32 s;
    unsigned a, ia;

    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return;

    if (dw <= MR_STAGE_W) {
        for (x = 0; x < dw; x++)
            g_ui_sxmap[x] = sx0 + x * sw / dw;
        xmap = g_ui_sxmap;
    }

    for (y = 0; y < dh; y++) {
        if (dy + y < 0 || dy + y >= dst->h)
            continue;
        sy = sy0 + y * sh / dh;
        srow = src->px + (size_t)sy * src->pitch;
        drow = dst->px + (size_t)(dy + y) * dst->pitch;

        for (x = 0; x < dw; x++) {
            if (dx + x < 0 || dx + x >= dst->w)
                continue;
            sx = xmap ? xmap[x] : sx0 + x * sw / dw;
            s = srow[sx];
            a = MR_A(s);
            if (!a)
                continue;
            if (a == 255) {
                drow[dx + x] = s;
                continue;
            }
            ia = 255u - a;
            {
                mr_u32 d = drow[dx + x];
                drow[dx + x] = MR_ARGB(255,
                    MR_DIV255((unsigned)MR_R(s) * a + (unsigned)MR_R(d) * ia),
                    MR_DIV255((unsigned)MR_G(s) * a + (unsigned)MR_G(d) * ia),
                    MR_DIV255((unsigned)MR_B(s) * a + (unsigned)MR_B(d) * ia));
            }
        }
    }
}

/* Ren'Py's Frame(): a nine-slice. The four corners keep their natural size,
 * the edges stretch along one axis and the middle stretches along both, so a
 * border of `b` pixels stays `b` pixels however far the frame is resized.
 * Stretching the whole image instead would squash the namebox's 5px border to
 * under two pixels for a short name. */
static void blit_frame(mr_surface *dst, const mr_surface *src,
                       int dx, int dy, int dw, int dh, int b)
{
    int sw = src->w, sh = src->h;
    int mw, mh;          /* middle span in the source */
    int dmw, dmh;        /* middle span in the destination */

    if (b * 2 >= sw || b * 2 >= sh || dw < b * 2 || dh < b * 2) {
        blit_rect_stretched(dst, src, 0, 0, sw, sh, dx, dy, dw, dh);
        return;
    }

    mw = sw - b * 2;
    mh = sh - b * 2;
    dmw = dw - b * 2;
    dmh = dh - b * 2;

    /* corners */
    blit_rect_stretched(dst, src, 0,        0,        b, b, dx,           dy,           b, b);
    blit_rect_stretched(dst, src, sw - b,   0,        b, b, dx + dw - b,  dy,           b, b);
    blit_rect_stretched(dst, src, 0,        sh - b,   b, b, dx,           dy + dh - b,  b, b);
    blit_rect_stretched(dst, src, sw - b,   sh - b,   b, b, dx + dw - b,  dy + dh - b,  b, b);

    /* edges */
    blit_rect_stretched(dst, src, b,      0,      mw, b,  dx + b,      dy,          dmw, b);
    blit_rect_stretched(dst, src, b,      sh - b, mw, b,  dx + b,      dy + dh - b, dmw, b);
    blit_rect_stretched(dst, src, 0,      b,      b,  mh, dx,          dy + b,      b,   dmh);
    blit_rect_stretched(dst, src, sw - b, b,      b,  mh, dx + dw - b, dy + b,      b,   dmh);

    /* middle */
    blit_rect_stretched(dst, src, b, b, mw, mh, dx + b, dy + b, dmw, dmh);
}

/* Fallback strip for when gui/textbox.png is missing. */
static void darken_strip(mr_surface *s, int top, int height)
{
    int y, x;
    mr_u32 *row, v;

    for (y = top; y < top + height && y < s->h; y++) {
        if (y < 0)
            continue;
        row = s->px + (size_t)y * s->pitch;
        for (x = 0; x < s->w; x++) {
            v = row[x];
            row[x] = MR_ARGB(255,
                (unsigned)MR_R(v) * 2 / 5,
                (unsigned)MR_G(v) * 2 / 5,
                (unsigned)MR_B(v) * 2 / 5);
        }
    }
}

/* Render one screen. Installed on the scene layer so transitions cover it. */
static int g_hover = -1;

void mr_ui_set_hover(int id)
{
    if (g_hover != id) {
        g_hover = id;
        mr_invalidate();
    }
}
int  mr_ui_hover(void)       { return g_hover; }

static void draw_screen(mr_surface *dst, const char *name, int interactive)
{
    const sl_screen *sc = mr_interp_find_screen(name);

    if (!sc || !g_body)
        return;
    mr_sl_render(dst, sc, interactive, g_hover);
}

/* The dialogue window.
 *
 * The window is a thing in its own right, not a property of there being a
 * line to read: `window show` puts it up, `window hide` takes it down, and a
 * line of dialogue shows it for as long as the line is up even when it is
 * otherwise hidden. So the box is painted whenever the window is visible, and
 * the name and the text go inside it when there is a line.
 *
 * It is painted after the scene, every frame, transition or not. A `with
 * dissolve` between two lines leaves the window exactly where it was, which
 * is what Ren'Py does: the effect belongs to the scene underneath.
 *
 * Split in two at the box art, because that is the seam the band cache is
 * taken at: the art is the same pixels for every frame of a line, and
 * blending 148,000 of them to add one glyph was most of what a typing frame
 * cost. See bandcache.h. The halves are called back to back by mr_ui_draw, so
 * nothing about the paint order changed. */
static void draw_dialogue_panel(mr_surface *stage)
{
    int top;

    if (!mr_interp_window_visible())
        return;

    top = textbox_top();

    /* `style window` sets the background to an Image, not a Frame, so the art
     * keeps its natural 800x185 and is merely placed: xalign 0.5 centres it
     * across the full-width window, yalign 1.0 sits it on the bottom. Only
     * the text inside spans the wider area. */
    if (g_textbox)
        mr_blit(stage, g_textbox,
                (MR_STAGE_W - g_textbox->w) / 2,
                textbox_art_top(top));
    else
        darken_strip(stage, top, g_geom.textbox_h);
}

/* The namebox, the speaker's name and the line itself: everything inside the
 * window that a typing frame actually changes. The namebox plate is here and
 * not in the panel because it is sized to the name, so it moves with the
 * speaker. */
static void draw_dialogue_front(mr_surface *stage)
{
    const char *who = NULL, *what = NULL;
    mr_u32 color = MR_RGB(255, 255, 255);
    int shown = -1;
    int have_line;
    int top;

    if (!mr_interp_window_visible())
        return;

    have_line = mr_interp_dialogue(&who, &what, &color, &shown);

    top = textbox_top();

    if (who && who[0]) {
        int nx = g_geom.name_x;
        int ny = top + g_geom.name_y;
        int tw = mr_font_text_width(g_name, who);
        int th = mr_font_line_height(g_name);

        /* gui.namebox_width is None and gui.name_xalign is 0.0, so the plate
         * shrinks to the name plus gui.namebox_borders, left aligned. This
         * one really is a Frame, so it is nine-sliced rather than stretched. */
        if (g_namebox)
            blit_frame(stage, g_namebox, nx, ny,
                       tw + g_geom.pad_l + g_geom.pad_r,
                       th + g_geom.pad_t + g_geom.pad_b,
                       g_geom.pad_l);

        /* Every character in this game specifies
         * who_outlines=[(1, "#000000", 0, 0)]. */
        mr_font_draw_outlined(stage, g_name, nx + g_geom.pad_l,
                              ny + g_geom.pad_t, who,
                              color, MR_RGB(0, 0, 0));
    }

    /* The line is laid out whole and revealed a character at a time, so it
     * never reflows as it arrives. An empty window has no line at all. */
    if (have_line) {
        mr_font_draw_revealed(stage, g_body,
                              g_geom.dialogue_x, top + g_geom.dialogue_y,
                              g_geom.dialogue_w, what,
                              MR_RGB(255, 255, 255), 0, 0, shown);

        /* Only for the real stage. A harness that renders a second,
         * reference copy of the same moment to another surface is not
         * painting anything the player sees, and must not be able to
         * satisfy the "the finished line has been drawn" test on the live
         * stage's behalf. */
        if (stage == mr_stage())
            mr_interp_say_painted(shown < 0);
    }
}

/* --- the choice menu -----------------------------------------------------
 *
 * Ren'Py shows `screen choice(items)` for a `menu` statement. This draws it
 * natively, as the dialogue window is drawn natively, because the items are a
 * list of objects with a `.caption` and an `.action` and the screen language
 * here has no way to be handed one.
 *
 * What it does not do is invent a look. Every number comes from the game's
 * own gui.* variables and choice_* styles, through the same style table and
 * the same background drawing the screen language uses, so this game's menu
 * comes out as its gui.rpy describes and another game's comes out as its own
 * does. With this game's values -- 790 wide, 22 spacing, the vbox centred on
 * y=270 -- a seven-option menu starts at y=78, which is where Ren'Py puts it.
 */
#define UI_CHOICE_MAX 16

static int menu_font_size(void)
{
    /* gui.choice_button_text_size, which gui.rpy defaults to gui.text_size. */
    long n = mr_var_get_int("gui.choice_button_text_size",
                            mr_var_get_int("gui.text_size", 22));
    return (int)n;
}

/* The menu's face and size together. gui.choice_button_text_font is
 * gui.text_font in both games' templates, so this is the default face today;
 * reading it is what keeps that a fact about the script rather than about
 * this function. */
static mr_font *menu_font(void)
{
    char face[64];

    if (!gui_str("gui.choice_button_text_font", face, (int)sizeof(face)))
        gui_str("gui.text_font", face, (int)sizeof(face));

    return mr_ui_font_named(face[0] ? face : NULL, menu_font_size());
}

/* A style property as a number, for the vbox placement. */
static double style_num(const char *style, const char *prop, double dflt)
{
    const char *e = mr_style_get(style, prop);
    mr_value v;

    if (!e || !*e)
        return dflt;
    if (!mr_expr_eval(e, &v))
        return dflt;
    if (v.type == MR_V_INT)
        return (double)v.i;
    if (v.type == MR_V_FLOAT)
        return v.f;
    return dflt;
}

/* Rectangle of choice `i` of `count`. Returns 0 if there is no menu. */
static int menu_rect(int i, int count, int *rx, int *ry, int *rw, int *rh)
{
    mr_font *f = menu_font();
    int w, h, spacing, total, top;

    if (!f || count <= 0)
        return 0;

    w = (int)mr_var_get_int("gui.choice_button_width", 790);
    spacing = (int)mr_var_get_int("gui.choice_spacing", 22);

    /* Borders(100, 5, 100, 5): the 5s are the vertical padding. */
    h = mr_font_line_height(f) + 10;

    total = count * h + (count - 1) * spacing;

    /* `style choice_vbox: ypos 270, yanchor 0.5`. */
    top = (int)(style_num("choice_vbox", "ypos", MR_STAGE_H / 2.0) -
                style_num("choice_vbox", "yanchor", 0.5) * total);

    *rx = (int)(style_num("choice_vbox", "xalign", 0.5) *
                (MR_STAGE_W - w));
    *ry = top + i * (h + spacing);
    *rw = w;
    *rh = h;
    return 1;
}

static int g_menu_hover = -1;

int mr_ui_menu_hit(int sx, int sy)
{
    const char *texts[UI_CHOICE_MAX];
    int count = 0;
    int i, x, y, w, h;

    if (!mr_interp_menu(&count, texts, UI_CHOICE_MAX) || count <= 0)
        return -1;
    if (count > UI_CHOICE_MAX)
        count = UI_CHOICE_MAX;

    for (i = 0; i < count; i++) {
        if (!menu_rect(i, count, &x, &y, &w, &h))
            return -1;
        if (sx >= x && sx < x + w && sy >= y && sy < y + h) {
            /* An insensitive row is not focusable in Ren'Py either, so it
             * neither lights up nor takes the click -- and the pointer does
             * not fall through to whatever is behind it. */
            if (!mr_interp_menu_sensitive(i))
                return -1;
            return i;
        }
    }
    return -1;
}

void mr_ui_set_menu_hover(int i)
{
    if (g_menu_hover != i) {
        g_menu_hover = i;
        mr_invalidate();
    }
}

static void draw_menu(mr_surface *stage)
{
    const char *texts[UI_CHOICE_MAX];
    int count = 0;
    int i;

    if (!mr_interp_menu(&count, texts, UI_CHOICE_MAX) || count <= 0)
        return;
    if (count > UI_CHOICE_MAX)
        count = UI_CHOICE_MAX;

    for (i = 0; i < count; i++) {
        /* The trailing underscore is part of the state: the properties are
         * stored as `hover_background`, and the style table's search
         * concatenates without adding one. */
        const char *state = !mr_interp_menu_sensitive(i) ? "insensitive_" :
                            (i == g_menu_hover) ? "hover_" : "idle_";
        mr_font *f = menu_font();
        int x, y, w, h, tw;
        mr_u32 col;

        if (!menu_rect(i, count, &x, &y, &w, &h))
            return;

        mr_sl_draw_style_background(stage, "choice_button", state,
                                    x, y, w, h);

        col = mr_sl_style_color("choice_button_text", state, "color",
                                MR_RGB(255, 255, 255));

        /* gui.choice_button_text_xalign is 0.5 in every gui.rpy that ships
         * with Ren'Py, and this game does not change it. */
        tw = mr_font_text_width(f, texts[i]);
        mr_font_draw(stage, f, x + (w - tw) / 2,
                     y + (h - mr_font_line_height(f)) / 2,
                     texts[i], col);
    }
}

/* Notify(): a line near the top of the stage for a few seconds. Ren'Py has a
 * `notify` screen for it; here it is painted with the dialogue rather than
 * made a screen, so it stays out of the display list -- and therefore out of
 * saves, out of transition snapshots, and out of the count that decides
 * whether a partial frame is safe. A transient message is not part of the
 * scene. */
static void draw_notify(mr_surface *stage)
{
    const char *msg = mr_interp_notify_text();
    int w;

    if (!msg || !*msg || !g_body)
        return;

    w = mr_font_text_width(g_body, msg);
    mr_font_draw(stage, g_body, (MR_STAGE_W - w) / 2, 24, msg,
                 MR_RGB(255, 255, 255));
}

void mr_ui_draw_panel(mr_surface *stage)
{
    if (!g_body)
        return;
    draw_dialogue_panel(stage);
}

void mr_ui_draw_front(mr_surface *stage)
{
    if (!g_body)
        return;
    draw_dialogue_front(stage);
    draw_menu(stage);
    draw_notify(stage);
}

void mr_ui_draw(mr_surface *stage)
{
    mr_ui_draw_panel(stage);
    mr_ui_draw_front(stage);
}

/* Everything draw_dialogue paints is inside the textbox strip: the box art
 * sits on the bottom edge at its natural height, the namebox starts at the
 * top of that strip, and the line is laid out inside it. The strip is the
 * full width because the text spans wider than the 800px box art.
 *
 * A menu is drawn up the middle of the stage, well outside the strip, so a
 * frame with one up cannot be a partial one. */
int mr_ui_dialogue_rect(int *x, int *y, int *w, int *h)
{
    int count = 0;
    const char *texts[16];

    if (!g_body)
        return 0;
    if (!mr_interp_window_visible())
        return 0;

    /* A choice menu is drawn up the middle of the stage, outside this strip,
     * and this used to refuse a partial frame outright because of it. But a
     * menu is *static* once drawn, and everything that changes it already
     * forces a full frame by another route: it appears on a frame reached
     * through ST_RUN, hovering a row calls mr_invalidate, and answering it
     * leaves the state. The one thing still moving with a menu up is the
     * caption a menu may open with, which types itself out inside this strip
     * like any other line.
     *
     * Refusing meant that caption never animated at all. ST_MENU asks for no
     * frames of its own, so the text sat frozen at however many characters
     * the frame that drew the menu had revealed, and only completed when the
     * player moved the mouse or pressed a key -- which is what it looked
     * like from play: the caption appearing on the click rather than with the
     * menu.
     *
     * Safe because the band path clips rather than skips: it runs the whole
     * of mr_interp_render and mr_ui_draw, draw_menu included, so a row that
     * reaches into the strip is repainted there and not left stale. */
    (void)count;
    (void)texts;

    {
        int top = textbox_top();
        int art = textbox_art_top(top);
        int bot = top + g_geom.textbox_h;

        /* The union of the window and its art, so that the rect really does
         * contain everything draw_dialogue paints -- which is the assumption
         * the whole partial-redraw path rests on. Clamped to the stage
         * because the caller clips a blit with it. */
        if (art < top)
            top = art;
        if (top < 0)
            top = 0;
        if (bot > MR_STAGE_H)
            bot = MR_STAGE_H;
        if (bot <= top)
            return 0;

        *x = 0;
        *y = top;
        *w = MR_STAGE_W;
        *h = bot - top;
    }
    return 1;
}

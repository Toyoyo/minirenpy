/* The inline text tags {b}, {i} and {u}.
 *
 * These used to be parsed and thrown away. Now {b} and {i} pick one of four
 * baked variants of each glyph and {u} draws a rule, so what is worth pinning
 * down is not "does it draw something" but that the right thing changed:
 *
 *   - a styled run is a different shape from the same text unstyled, and the
 *     four variants are four distinct shapes rather than three aliases of the
 *     regular face;
 *   - the counters are per flag, so the closes may come out of order --
 *     {i}{b}{u}a{/i}{/b}{/u} is the case that asked for this;
 *   - a style opened before a wrap is still in force after it, which is the
 *     part the old per-line drawing could not have done;
 *   - the typewriter reveal does not change which characters are styled, only
 *     how many are painted;
 *   - and everything the old skip_tag got right stays right: {{ is a literal
 *     brace, an unknown tag costs no width, a stray close is ignored.
 *
 * Shapes are compared by hashing the pixels the draw actually touched, which
 * catches a variant quietly falling back to the regular glyph -- the failure
 * this is really guarding, since a missing face looks like working code.
 *
 * Takes the game directory (it loads fonts/font*.dat through the VFS).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "mr.h"
#include "vfs.h"
#include "font.h"

static mr_surface *g_stage;

void mr_log(const char *fmt, ...)
{
    va_list ap;
    if (!getenv("MR_VERBOSE"))
        return;
    va_start(ap, fmt);
    printf("    log: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}
void mr_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("FATAL: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    exit(1);
}
void mr_quit(void) { }
void mr_invalidate(void) { }
mr_surface *mr_stage(void) { return g_stage; }

static int fails;

static void check(const char *what, int ok)
{
    printf("%-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        fails++;
}

#define W 640
#define H 120

static mr_surface *g_canvas;
static mr_font    *g_font;

/* Hash whatever is on the canvas now. */
static unsigned long hash_canvas(void)
{
    unsigned long h = 5381;
    int x, y;

    for (y = 0; y < H; y++) {
        mr_u32 *row = g_canvas->px + (size_t)y * g_canvas->pitch;
        for (x = 0; x < W; x++)
            h = h * 33u + (row[x] & 0xFFu);
    }
    return h;
}

/* Draw onto a cleared canvas and hash what came out. */
static unsigned long shape(const char *text)
{
    unsigned long h = 5381;
    int x, y;

    mr_surface_fill(g_canvas, MR_ARGB(255, 0, 0, 0));
    mr_font_draw(g_canvas, g_font, 20, 20, text, MR_RGB(255, 255, 255));

    for (y = 0; y < H; y++) {
        mr_u32 *row = g_canvas->px + (size_t)y * g_canvas->pitch;
        for (x = 0; x < W; x++)
            h = h * 33u + (row[x] & 0xFFu);
    }
    return h;
}

/* Lowest row with any ink, so an underline can be told from a descender. */
static int lowest_ink(const char *text)
{
    int x, y, low = -1;

    mr_surface_fill(g_canvas, MR_ARGB(255, 0, 0, 0));
    mr_font_draw(g_canvas, g_font, 20, 20, text, MR_RGB(255, 255, 255));

    for (y = 0; y < H; y++) {
        mr_u32 *row = g_canvas->px + (size_t)y * g_canvas->pitch;
        for (x = 0; x < W; x++)
            if ((row[x] & 0xFFu) > 40) {
                low = y;
                break;
            }
    }
    return low;
}

static int width(const char *text)
{
    return mr_font_text_width(g_font, text);
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "game";

    if (!mr_vfs_init(NULL, dir)) {
        printf("vfs init failed\n");
        return 1;
    }
    g_stage = mr_surface_new(MR_STAGE_W, MR_STAGE_H);
    g_canvas = mr_surface_new(W, H);

    g_font = mr_font_load("fonts/font22.dat");
    if (!g_font) {
        printf("fonts/font22.dat did not load -- run `make fonts` first "
               "(the styled atlas is format v2; a v1 file is rejected)\n");
        return 1;
    }

    printf("-- the four variants are four different faces --\n");
    {
        unsigned long r = shape("Wahrheit");
        unsigned long b = shape("{b}Wahrheit{/b}");
        unsigned long i = shape("{i}Wahrheit{/i}");
        unsigned long bi = shape("{b}{i}Wahrheit{/i}{/b}");

        check("bold is not the regular face", b != r);
        check("italic is not the regular face", i != r);
        check("bold italic is neither", bi != b && bi != i && bi != r);
        check("  and bold is not italic", b != i);
        check("bold is wider than regular", width("{b}Wahrheit{/b}") >
                                            width("Wahrheit"));
    }

    printf("\n-- tags are counted, not stacked --\n");
    {
        check("closes may come out of order",
              shape("{i}{b}{u}a{/i}{/b}{/u}") ==
              shape("{i}{b}{u}a{/u}{/b}{/i}"));
        check("a style really ends at its close",
              shape("{b}a{/b}b") != shape("{b}ab{/b}"));
        check("a stray close does not flip the rest",
              shape("{/i}abc") == shape("abc"));
        check("nesting the same tag twice needs two closes",
              shape("{b}{b}a{/b}b{/b}") == shape("{b}ab{/b}"));
    }

    printf("\n-- the underline --\n");
    {
        int plain = lowest_ink("axe");
        int under = lowest_ink("{u}axe{/u}");

        check("an underline puts ink below the glyphs", under > plain);
        check("  and it ends with the tag",
              lowest_ink("{u}a{/u}xe") == lowest_ink("{u}a{/u}"));
        check("  while costing no width",
              width("{u}axe{/u}") == width("axe"));
    }

    printf("\n-- what the old tag skipping already got right --\n");
    {
        /* Note a lone "{" is a tag that never closes, and so has no width:
         * comparing against it would pass on a renderer that dropped both. */
        check("{{ is a literal brace, not a tag",
              width("{{") > 0 && shape("a{{b") != shape("ab"));
        check("an unknown tag costs nothing",
              width("a{w=0.5}b") == width("ab"));
        check("  and neither does a style tag",
              width("{i}{/i}") == 0);
    }

    printf("\n-- a style survives a wrap --\n");
    {
        /* Wide enough for two words, so the phrase wraps in the middle of
         * the bold run. If the style were per line, the second line would
         * come back regular. */
        const char *bold_across =
            "{b}alpha bravo charlie delta{/b}";
        const char *bold_each =
            "{b}alpha bravo{/b} {b}charlie delta{/b}";
        const char *bold_first =
            "{b}alpha bravo{/b} charlie delta";
        int wrap = width("alpha bravo ") + 4;
        unsigned long across, each, first;

        mr_surface_fill(g_canvas, MR_ARGB(255, 0, 0, 0));
        mr_font_draw_wrapped(g_canvas, g_font, 10, 10, wrap, bold_across,
                             MR_RGB(255, 255, 255), 0, 0);
        across = 5381;
        {
            int x, y;
            for (y = 0; y < H; y++) {
                mr_u32 *row = g_canvas->px + (size_t)y * g_canvas->pitch;
                for (x = 0; x < W; x++)
                    across = across * 33u + (row[x] & 0xFFu);
            }
        }

        mr_surface_fill(g_canvas, MR_ARGB(255, 0, 0, 0));
        mr_font_draw_wrapped(g_canvas, g_font, 10, 10, wrap, bold_each,
                             MR_RGB(255, 255, 255), 0, 0);
        each = 5381;
        {
            int x, y;
            for (y = 0; y < H; y++) {
                mr_u32 *row = g_canvas->px + (size_t)y * g_canvas->pitch;
                for (x = 0; x < W; x++)
                    each = each * 33u + (row[x] & 0xFFu);
            }
        }

        mr_surface_fill(g_canvas, MR_ARGB(255, 0, 0, 0));
        mr_font_draw_wrapped(g_canvas, g_font, 10, 10, wrap, bold_first,
                             MR_RGB(255, 255, 255), 0, 0);
        first = 5381;
        {
            int x, y;
            for (y = 0; y < H; y++) {
                mr_u32 *row = g_canvas->px + (size_t)y * g_canvas->pitch;
                for (x = 0; x < W; x++)
                    first = first * 33u + (row[x] & 0xFFu);
            }
        }

        check("the second line is still bold", across == each);
        check("  and it would not be if the tag had closed", across != first);
        /* Bold is wider than the width this was measured at, so it may take
         * three lines rather than two; what matters is that it wrapped. */
        check("the text did wrap", mr_font_draw_wrapped(NULL, g_font, 10, 10,
                                       wrap, bold_across, 0, 0, 0) >= 2);
    }

    printf("\n-- the reveal styles the same characters --\n");
    {
        /* Revealing n characters must draw exactly what the text cut to n
         * characters draws -- same glyphs, same styles, same places.
         *
         * A pixel-prefix comparison against the settled line would be the
         * obvious test and is wrong: an italic glyph leans out past its own
         * advance, so the settled line has ink to the left of where the
         * partial one stops. The cuts are spelled out instead, including one
         * inside the italic run and one at the closing tag.
         */
        static const struct { int n; const char *cut; } cases[] = {
            { 2,  "un" },
            { 3,  "un " },
            { 4,  "un {i}s" },
            { 7,  "un {i}souf" },
            { 10, "un {i}souffle" },
            { 12, "un {i}souffle{/i} d" },
            { 25, "un {i}souffle{/i} dans le vent" }
        };
        const char *text = "un {i}souffle{/i} dans le vent";
        int wrap = 10000;       /* one line: this is about the reveal only */
        int k, bad = 0;

        for (k = 0; k < (int)(sizeof(cases) / sizeof(cases[0])); k++) {
            unsigned long partial, whole;
            int x, y;

            mr_surface_fill(g_canvas, MR_ARGB(255, 0, 0, 0));
            mr_font_draw_revealed(g_canvas, g_font, 10, 10, wrap, text,
                                  MR_RGB(255, 255, 255), 0, 0, cases[k].n);
            partial = 5381;
            for (y = 0; y < H; y++) {
                mr_u32 *row = g_canvas->px + (size_t)y * g_canvas->pitch;
                for (x = 0; x < W; x++)
                    partial = partial * 33u + (row[x] & 0xFFu);
            }

            mr_surface_fill(g_canvas, MR_ARGB(255, 0, 0, 0));
            mr_font_draw_revealed(g_canvas, g_font, 10, 10, wrap,
                                  cases[k].cut, MR_RGB(255, 255, 255),
                                  0, 0, -1);
            whole = 5381;
            for (y = 0; y < H; y++) {
                mr_u32 *row = g_canvas->px + (size_t)y * g_canvas->pitch;
                for (x = 0; x < W; x++)
                    whole = whole * 33u + (row[x] & 0xFFu);
            }

            if (partial != whole) {
                printf("   %d characters is not \"%s\"\n",
                       cases[k].n, cases[k].cut);
                bad++;
            }
        }
        check("every cut draws what that much of the text draws", !bad);
    }

    printf("\n-- CJK falls back to the one weight it has --\n");
    {
        /* NotoSansJP ships Regular only, so the baker leaves the variants
         * out and the runtime must fall back rather than drop the glyph. */
        /* U+98A8 風, from the extras menu -- a character the charset really
         * contains, since an absent one would pass the width test at zero. */
        check("a bold kana still draws", width("{b}\xe9\xa2\xa8{/b}") > 0);
        check("  at the regular width",
              width("{b}\xe9\xa2\xa8{/b}") == width("\xe9\xa2\xa8"));
        check("  and looks the same",
              shape("{b}\xe9\xa2\xa8{/b}") == shape("\xe9\xa2\xa8"));
    }

    printf("\n-- the space at a line break costs nothing --\n");
    {
        /* A break happens *at* a space, so the space ending a line is the
         * break and must not be measured as part of it. We used to measure
         * the candidate word with its trailing space attached, which pushed
         * a word to the next line whenever the line fit with less than a
         * space to spare. In the game that was one word per line of
         * dialogue against Ren'Py (wrap-rp.png / wrap-mrpy.png).
         *
         * The wrap width is chosen from the text itself rather than written
         * down: exactly the width of "aaa bbb", so "aaa bbb" fits and
         * anything more does not. That is the only width at which the two
         * behaviours differ, which is the point -- at any other width this
         * passes either way.
         */
        /* All four words identical, so no question of one being wider than
         * another: at exactly the width of two of them, the right answer is
         * two lines of two. Measuring the trailing space makes every line
         * hold one word, which is three lines -- counting lines separates
         * the two behaviours only when the text is a multiple of the line,
         * which is why this is four words and not three. */
        const char *two  = "aaa aaa";
        const char *four = "aaa aaa aaa aaa";
        int w = width(two);
        int lines;

        check("the probe width is two words", w > 0);
        check("  and three would not fit", width("aaa aaa aaa") > w);

        check("two words fit in their own width",
              mr_font_draw_wrapped(NULL, g_font, 0, 0, w, two,
                                   0, 0, 0) == 1);

        lines = mr_font_draw_wrapped(NULL, g_font, 0, 0, w, four, 0, 0, 0);
        printf("   \"%s\" at %dpx wraps to %d lines\n", four, w, lines);
        check("  and four words make two lines of two, not three lines",
              lines == 2);

        /* The same rule applies to placing a line, not just to deciding
         * where it ends: a centred line measured with its trailing space
         * sits half a space left of centre. Checked by drawing the same
         * text with and without one and comparing the pixels. */
        {
            unsigned long a, b;

            mr_surface_fill(g_canvas, MR_ARGB(255, 0, 0, 0));
            mr_font_draw_aligned(g_canvas, g_font, 20, 20, 400, "aaa",
                                 MR_RGB(255, 255, 255), 0, 0, -1, 0.5f);
            a = hash_canvas();

            mr_surface_fill(g_canvas, MR_ARGB(255, 0, 0, 0));
            mr_font_draw_aligned(g_canvas, g_font, 20, 20, 400, "aaa ",
                                 MR_RGB(255, 255, 255), 0, 0, -1, 0.5f);
            b = hash_canvas();

            check("a trailing space does not move a centred line", a == b);
        }
    }

    printf("\n-- a newline is a line break, not a character --\n");
    {
        /* mr_font_text_width measured the whole string as one run, so a
         * message with an embedded \n came back as wide as both lines laid
         * end to end. That is what sized the confirm frame: slrender's
         * measure_inner asks for the natural width, sees it overflow the
         * screen, and falls back to wrapping at the full available width --
         * so the load prompt's frame spanned the whole 1280 instead of
         * hugging its longest line, as Ren'Py's does.
         *
         * The two halves are different lengths on purpose: with equal halves
         * "max" and "first" would agree and the test would not distinguish
         * them. */
        const char *a = "Loading will lose unsaved progress.";
        const char *b = "Are you sure?";
        char both[128];
        sprintf(both, "%s\n%s", a, b);

        check("the halves differ in width", width(a) != width(b));
        check("  and the longer one is the first", width(a) > width(b));
        check("the width of two lines is the wider line",
              width(both) == width(a));
        check("  not the sum", width(both) < width(a) + width(b));
    }

    mr_font_free(g_font);
    mr_surface_free(g_canvas);
    mr_surface_free(g_stage);
    mr_vfs_shutdown();

    printf(fails ? "\n%d FAILED\n" : "\nall good\n", fails);
    return fails ? 1 : 0;
}

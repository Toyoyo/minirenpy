/* Python string prefixes: u'...', b'...', r'...', f'...' and the pairs.
 *
 * Ren'Py scripts do write them. One game's gui.rpy spells every colour it
 * defines as u'#c184ff', and the expression parser had no notion of a prefix:
 * `u'#c184ff'` read as the name `u` followed by a string literal, so the
 * define came out as an unset variable. Every colour in the file was then
 * None.
 *
 * That failed almost invisibly. A missing colour and an unset one are the
 * same thing to the renderer, so the game looked right -- the only symptom
 * was that the main menu's buttons no longer lit up under the pointer, while
 * still being perfectly clickable, because idle and hover had become the same
 * (defaulted) colour.
 *
 * So this is in two halves, and the second is the one that matters:
 *
 *   - the literal parses, with each prefix, and r keeps its backslashes.
 *   - a define that uses one reaches the style table. The fixture's script.rpy
 *     sets gui.navigation_button_text_hover_color = u'#cc3366', and what is
 *     checked here is the colour the *renderer* resolves for a hovered
 *     navigation button -- mr_sl_style_color is color_prop, the same function
 *     draw_inner calls for a textbutton's text.
 *
 * The fallback is what makes the second half a real assertion rather than a
 * restatement of the first: with the prefix unparsed the style does not lose
 * its hover colour, it inherits #66a3e0 from gui_button_text. Checking "is
 * there a hover colour at all", or comparing hovered pixels against idle
 * ones, passes either way.
 *
 * Runs on tools/harness/story.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "mr.h"
#include "lex.h"
#include "expr.h"
#include "interp.h"
#include "vfs.h"
#include "script.h"
#include "image.h"
#include "scene.h"
#include "ui.h"
#include "slrender.h"
#include "style.h"

static mr_surface *g_st;
mr_surface *mr_stage(void) { return g_st; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); }
/* The load and expansion lines only. What this test asserts on comes out of
 * the style table, and an empty table has exactly the same symptom as a
 * colour that failed to parse -- so the output has to say which it was. */
void mr_log(const char *f, ...)
{
    va_list a;

    if (strncmp(f, "script:", 7) && strncmp(f, "style:", 6))
        return;
    va_start(a, f);
    printf("   | ");
    vprintf(f, a);
    printf("\n");
    va_end(a);
}
void mr_quit(void) {}
void mr_invalidate(void) {}
void hostshim_wave_sink(const short *d, int n) { (void)d; (void)n; }

static int fails;

static void check(const char *label, int ok)
{
    printf("  %-56s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

/* Evaluate `expr` into x and return x's value as a string, or NULL if it did
 * not come out a string at all -- which is the failure mode here: the name `u`
 * is unset, so x ends up None. */
static const char *strval(const char *expr)
{
    static mr_value v;
    char line[256];

    sprintf(line, "x = %s", expr);
    mr_expr_exec(line);
    if (!mr_var_get("x", &v) || v.type != MR_V_STR)
        return NULL;
    return v.s;
}

static void one(const char *expr, const char *want)
{
    const char *got = strval(expr);
    char label[96];

    sprintf(label, "  %-20s -> \"%s\"", expr, want);
    if (!got)
        printf("   %s came out %s\n", expr, "unset or not a string");
    check(label, got && !strcmp(got, want));
}

int main(int argc, char **argv)
{
    mr_script *s;

    setbuf(stdout, NULL);
    if (argc < 2) {
        fprintf(stderr, "usage: prefixtest <story-dir>\n");
        return 2;
    }

    mr_vfs_init(NULL, argv[1]);
    mr_image_init(argv[1]);
    mr_scene_init();
    s = mr_script_load(argv[1]);
    mr_interp_init(s, argv[1]);
    mr_ui_init(argv[1]);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);

    /* mr_interp_start, not just mr_interp_init: the init phase that runs the
     * `define`s and then calls mr_style_finalize lives in start. Without it
     * the style table is empty, and an empty table answers every lookup with
     * the caller's default -- which is indistinguishable from the bug this
     * test is about. The "style: N styles, N gui expansions" line above is
     * there so that an empty table cannot be mistaken for a parse failure
     * again. */
    mr_interp_start("start");

    printf("\n-- a prefixed literal is a string --\n");
    one("u'abc'", "abc");
    one("U\"abc\"", "abc");
    one("b'abc'", "abc");
    one("B'abc'", "abc");
    one("f'abc'", "abc");
    one("rb'abc'", "abc");
    one("'abc'", "abc");

    printf("\n-- r keeps its backslashes, which is the point of it --\n");
    /* Written doubled here because this source is C: the engine sees a\nb
     * with a real backslash in it. A plain literal resolves the escape to a
     * newline; a raw one must not. */
    one("'a\\nb'", "a\nb");
    one("r'a\\nb'", "a\\nb");

    printf("\n-- and a name that merely starts with one of the letters --\n");
    /* The guard against over-reading. The prefix is only a prefix when the
     * quote follows immediately, so none of these may be mistaken for one.
     * Without this, a variable called `b` or a call to `format(...)` would
     * start parsing as a string literal. */
    mr_expr_exec("u = 5");
    mr_expr_exec("unlocked = 7");
    mr_expr_exec("rb = 9");
    check("  a variable named u is still a variable",
          mr_var_get_int("u", -1) == 5);
    check("  so is one whose name starts with u",
          mr_var_get_int("unlocked", -1) == 7);
    check("  and one named exactly like a prefix pair",
          mr_var_get_int("rb", -1) == 9);

    printf("\n-- the define reaches the colour the renderer draws with --\n");
    {
        /* The fixture: gui.navigation_button_text_hover_color = u'#cc3366',
         * expanded into style navigation_button_text by mr_style_finalize
         * through gui.button_text_properties("navigation_button"). */
        mr_u32 hov = mr_sl_style_color("navigation_button_text", "hover_",
                                       "color", MR_RGB(0, 0, 0));
        mr_u32 idle = mr_sl_style_color("navigation_button_text", "idle_",
                                        "color", MR_RGB(0, 0, 0));

        /* The define itself, so a failure below says whether the value never
         * arrived or arrived and was not expanded. */
        {
            mr_value dv;
            printf("   gui.navigation_button_text_hover_color = %s\n",
                   mr_var_get("gui.navigation_button_text_hover_color", &dv) &&
                   dv.type == MR_V_STR ? dv.s : "(unset)");
        }
        printf("   hover %02x%02x%02x, idle %02x%02x%02x\n",
               MR_R(hov), MR_G(hov), MR_B(hov),
               MR_R(idle), MR_G(idle), MR_B(idle));

        /* Not "hover differs from idle": it does that even unparsed, by
         * inheriting #66a3e0 from gui_button_text. It has to be *this*
         * colour, which only the prefixed define can supply. */
        check("a hovered navigation button uses the defined colour",
              hov == MR_RGB(0xcc, 0x33, 0x66));
        check("  and the idle one is untouched",
              idle != MR_RGB(0xcc, 0x33, 0x66));
    }

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}

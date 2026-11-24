/* `[name]` substitution in dialogue.
 *
 *     m "lalala [selchoice] lalala"
 *
 * displayed the brackets verbatim. Ren'Py runs *every* Text displayable
 * through substitutions.substitute(), so a say is substituted exactly as a
 * screen's `text` is -- and here the function doing it was a static inside
 * slrender.c, reachable only from the screen renderer. So interpolation
 * worked everywhere except the place a writer reaches for it first.
 *
 * Two halves, and the second is the one that would have been missed:
 *
 *   - the drawn text has the value in it, not the brackets.
 *   - the *character count* is of the drawn text. The typewriter reveals
 *     N characters of the line and the auto-forward delay scales by N, so
 *     counting `[sub_num]` while drawing `2` reveals at the wrong rate and
 *     stops revealing before the end of the line. One definition of "the
 *     text of this line" is the point of say_text().
 *
 * Runs on tools/harness/story, label interp_say.
 */
#include <stdio.h>
#include <string.h>

#include "mr.h"
#include "expr.h"
#include "interp.h"
#include "vfs.h"
#include "script.h"
#include "image.h"
#include "scene.h"
#include "ui.h"
#include "slrender.h"

#include <stdarg.h>

static mr_surface *g_st;
mr_surface *mr_stage(void) { return g_st; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); }

/* Only the layout lines, which is where a rendered screen's text shows up. */
static char g_dump[65536];
void mr_log(const char *f, ...)
{
    va_list a; char ln[512];
    if (strncmp(f, "layout", 6)) return;
    va_start(a, f); vsprintf(ln, f, a); va_end(a);
    if (strlen(g_dump) + strlen(ln) + 2 < sizeof(g_dump)) {
        strcat(g_dump, ln); strcat(g_dump, "\n");
    }
}
void mr_quit(void) {}
void mr_invalidate(void) {}
void hostshim_wave_sink(const short *d, int n) { (void)d; (void)n; }

static long T;
static int  fails;

static void check(const char *label, int ok)
{
    printf("  %-56s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

static void step(void) { mr_interp_update(T); T += 33; }

/* The line as the text layer would draw it, and how many characters of it the
 * engine thinks there are. `shown` is -1 once the whole line is revealed, so
 * the count is taken with the typewriter off. */
static const char *line(int *shown)
{
    const char *who, *what;
    mr_u32 c;

    if (!mr_interp_dialogue(&who, &what, &c, shown))
        return NULL;
    return what;
}

/* Step and advance until the line on screen contains `needle`, and return it;
 * NULL if it never shows up.
 *
 * Waiting for a *named* line rather than for "any line" on purpose. A plain
 * "advance until there is dialogue" helper returns the line that is already
 * up, which after a jump is the one from before it -- the previous version of
 * this file measured the typewriter on a leftover line from `label start` and
 * reported a 70-character string while printing the right one, because the
 * two reads straddled the update that actually moved. */
static const char *wait_for(const char *needle)
{
    int i;

    for (i = 0; i < 400; i++) {
        const char *w = line(NULL);
        if (w && strstr(w, needle))
            return w;
        if (w && *w)
            mr_interp_advance();
        step();
    }
    return NULL;
}

int main(int argc, char **argv)
{
    mr_script *s;
    const char *w;

    setbuf(stdout, NULL);
    if (argc < 2) {
        fprintf(stderr, "usage: substtest <story-dir>\n");
        return 2;
    }

    mr_vfs_init(NULL, argv[1]);
    mr_image_init(argv[1]);
    mr_scene_init();
    s = mr_script_load(argv[1]);
    mr_interp_init(s, argv[1]);
    mr_ui_init(argv[1]);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);

    /* Instant text, so a line is fully revealed and the count below is of the
     * whole line rather than of however much had appeared. */
    mr_interp_start("interp_say");
    mr_expr_exec("_preferences.text_cps = 0");

    printf("\n-- a line with three substitutions --\n");
    w = wait_for("name");
    printf("   %s\n", w ? w : "(none)");
    check("the line is on screen", w != NULL);
    /* The whole point. With the bug this reads
     * "Number [sub_num], name [sub_str], and [sub_num!t] with a flag." */
    check("  an int is substituted",
          w && strstr(w, "Number 2,") != NULL);
    check("  so is a string",
          w && strstr(w, "name Ada,") != NULL);
    /* Ren'Py's !conversion flags select a formatting we do not implement, so
     * the name is used and the flag dropped -- not left in the output. */
    check("  and a !flag is dropped, not printed",
          w && strstr(w, "and 2 with a flag") != NULL);
    check("  no brackets survive", w && strchr(w, '[') == NULL);

    printf("\n-- and the next one, which has nothing to substitute --\n");
    w = wait_for("A plain line");
    printf("   %s\n", w ? w : "(none)");
    check("it comes through unchanged",
          w && !strcmp(w, "A plain line, no brackets in it at all."));

    printf("\n-- the typewriter counts the drawn text, not the source --\n");

    /* This is the half that is easy to write vacuously, and the first version
     * here was: counting the *source* makes the line reveal more slowly, not
     * incorrectly, so "does it finish?" passes either way. It finishes with
     * 24 characters of dead time at the end -- the typewriter sits on the
     * completed line waiting to be told it is done.
     *
     * So the assertion is on *how long* the reveal takes, against the length
     * of the string actually drawn. At 30 cps and 33ms a frame it is close to
     * one character a frame, so the two numbers are directly comparable:
     * 38 drawn characters, and 62 if the brackets are counted instead. */
    {
        int shown = 0;
        const char *drawn;
        long drawn_len = 0;
        long frames;
        int  max_shown;

        /* Restart the label: the substituted line is its first, so this needs
         * no navigation at all. The version that navigated silently measured
         * the wrong line and the assertion below then failed for a reason
         * that had nothing to do with the engine. */
        mr_interp_start("interp_say");
        mr_expr_exec("_preferences.text_cps = 30");
        T += 33;

        w = wait_for("Number 2,");
        check("back on the substituted line, from the top", w != NULL);

        drawn = line(&shown);
        for (; *drawn; drawn++) {
            if ((*drawn & 0xC0) != 0x80)
                drawn_len++;
        }
        check("  and it is still typing", shown >= 0);

        /* The direct question: how far does the typewriter think it has to
         * count? `shown` is the index it reveals up to, so the largest value
         * it reaches before declaring the line finished *is* the engine's idea
         * of the line's length. Measuring the elapsed frames instead was the
         * first attempt and proved nothing -- see the note below. */
        max_shown = -1;
        for (frames = 0; frames < 600; frames++) {
            int sh = 0;
            if (!line(&sh) || sh < 0)
                break;
            if (sh > max_shown)
                max_shown = sh;
            step();
        }

        printf("   measuring \"%s\"\n", line(NULL));
        printf("   %ld characters drawn, typewriter reached %d, %ld frames\n",
               drawn_len, max_shown, frames);
        check("  it reveals fully", frames < 600);
        /* The assertion. Counting the source makes the typewriter ask for 62
         * characters of a 38-character string: it reveals the whole line and
         * then sits on it, apparently finished but not yet "complete", for
         * the remaining 24 characters' worth of time. */
        check("  and never asks for more characters than it drew",
              max_shown < (int)drawn_len);
        check("  while really having typed, not jumped",
              max_shown > (int)drawn_len / 2);
    }

    /* The screen path, which is where substitution already worked.
     *
     * Checked because it had *no* coverage at all: breaking node_text's
     * substitution leaves uitest, preftest and bartest green, so the
     * behaviour that has worked all along was the untested one -- and this
     * change moved the function it depends on out of slrender.c. One
     * implementation, so one place to break, and now both callers are pinned.
     *
     * `screen title` in the fixture is `text "[title]"`. */
    printf("\n-- the same function still serves screen text --\n");
    {
        mr_expr_exec("title = \"Chapter Nine\"");
        g_dump[0] = '\0';
        mr_sl_dump_next();
        mr_interp_show_screen("title");
        mr_interp_update(T);
        mr_interp_render(g_st, T);

        printf("   dump has: %s\n", strstr(g_dump, "Chapter") ?
               "Chapter Nine" : "no substituted title");
        check("a screen's [name] is substituted",
              strstr(g_dump, "Chapter Nine") != NULL);
        check("  and the brackets are gone",
              strstr(g_dump, "[title]") == NULL);
    }

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}

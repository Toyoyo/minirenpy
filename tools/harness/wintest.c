/* The title-card case: `window hide`, a scene change, a screen, and a
 * renpy.pause(). Rolling back from the pause must bring the say window back.
 * Reproduces by running the real script through that exact passage.
 *
 * Runs on tools/harness/story, whose chapter_two is that passage: `window
 * hide`, `scene dark with fade`, `show screen title with fade`,
 * `renpy.pause()`. It used to run on the mounted game and find the card by
 * matching one line of Miwa's prose, which meant it quietly did nothing on
 * every other game -- see the comment at the search loop. */
#include <stdio.h>
#include <string.h>
#include "mr.h"
#include "save.h"
#include "history.h"
#include "expr.h"
#include "interp.h"
#include "vfs.h"
#include "script.h"
#include "image.h"
#include "scene.h"

static mr_surface *g_st;
mr_surface *mr_stage(void) { return g_st; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); }
void mr_log(const char *f, ...) { (void)f; }
void mr_quit(void) {}
void mr_invalidate(void) {}
void hostshim_wave_sink(const short *d, int n) { (void)d; (void)n; }

static long T;
static char g_cur[220];
static void snap(void)
{
    const char *who, *what; mr_u32 c; int shown;
    if (mr_interp_dialogue(&who, &what, &c, &shown))
        sprintf(g_cur, "%.48s", what);
    else
        strcpy(g_cur, "(no text)");
}
/* Counts, and main returns it. Six harnesses printed their verdict and
 * exited 0 regardless, which run.sh reads as a pass; see NOTES, "A test
 * that cannot go red". */
static int fails;

static void check(const char *l, long got, long want)
{
    printf("%-34s %-6ld %s\n", l, got, got == want ? "ok" : "** WANT DIFFERENT **");
    if (got != want)
        fails++;
}

int main(int argc, char **argv)
{
    mr_script *s;
    int i;
    char last_line[220];
    int  reached;

    setbuf(stdout, NULL);
    mr_vfs_init(NULL, argv[1]);
    mr_image_init(argv[1]);
    mr_scene_init();
    s = mr_script_load(argv[1]);
    mr_interp_init(s, argv[1]);
    mr_interp_start("start");
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);

    /* Run forward until we are sitting on the renpy.pause() after the title
     * card: the display has a screen up and no dialogue. */
    last_line[0] = '\0';
    reached = 0;
    for (i = 0; i < 4000; i++) {
        int count = 0;
        const char *texts[16];

        mr_interp_update(T); T += 33;

        /* The card is past a menu in this fixture, and a menu does not
         * answer itself: without this the loop spent all 4000 steps
         * sitting in front of the choice and then reported that the
         * engine never reached a title card. Which option hardly
         * matters -- both paths lead to chapter_two. */
        if (mr_interp_menu(&count, texts, 16) && count > 0)
            mr_interp_choose(count - 1);

        snap();
        if (strcmp(g_cur, "(no text)"))
            strcpy(last_line, g_cur);
        /* The title card: a line has been spoken, there is none now, and
         * the interpreter is parked on the `renpy.pause()` rather than
         * merely mid-transition.
         *
         * This used to look for `strstr(last_line, "temps d'aller
         * dormir")` -- one line of one game's script -- and the harness
         * ran against whatever game was mounted. On any other game the
         * line never appeared, the loop ran out, and the bail below
         * `return 0`d: a green line for a test that asserted nothing, on
         * every game but one, including the no-game run. mr_interp_waiting
         * is the same condition stated as a fact about the engine. */
        if (!strcmp(g_cur, "(no text)") && last_line[0] &&
            mr_interp_waiting()) {
            reached = 1;
            break;
        }
        mr_interp_advance();
    }

    if (!reached) {
        /* A failure, not a shrug. The fixture has the passage -- `window
         * hide`, `scene dark with fade`, `show screen title`,
         * `renpy.pause()` in chapter_two -- so not finding it means the
         * engine did not get there. */
        printf("** FAIL ** never reached a title card: no `renpy.pause()`\n"
               "   with the window hidden was found in 4000 steps;\n"
               "   last line seen was \"%s\".\n", last_line);
        return 1;
    }

    printf("last spoken line : %s\n", last_line);
    snap();
    printf("at title card    : %s\n", g_cur);
    check("sitting on the title card", strcmp(g_cur, "(no text)") == 0, 1);

    /* Back off the card. Bounded rather than exactly one step: what sits
     * between the card and the last spoken line is the script's business
     * -- here it is the menu that chose this chapter, in the game it was
     * the say itself -- and the bug this test exists for is the window
     * never coming back at all, at any depth. Each step is still a real
     * rollback, so a rollback that does nothing fails the same way.
     *
     * The step count is printed: if it grows, something has been inserted
     * into the fixture ahead of the card, which is worth noticing even
     * though it is not a failure. */
    {
        int steps;

        for (steps = 1; steps <= 4; steps++) {
            mr_interp_rollback();
            mr_interp_update(T); T += 33;
            snap();
            if (strcmp(g_cur, "(no text)"))
                break;
        }
        printf("after rollback   : %s (%d step%s back)\n",
               g_cur, steps, steps == 1 ? "" : "s");
        check("text came back", strcmp(g_cur, "(no text)") != 0, 1);
        check("it is the line before", strcmp(g_cur, last_line) == 0, 1);
    }

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}

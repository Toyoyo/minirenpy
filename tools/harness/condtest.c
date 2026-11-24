/* Menu choices with a condition:
 *
 *     menu:
 *         "I'm feeling desperate":
 *             jump neutralend
 *
 *         "Then, so be it" if lifechoice1 == True:
 *             jump good0
 *
 * The parser read the caption and ignored the rest of the line, so the
 * condition was not merely mis-evaluated, it was never there: every option
 * was always offered. A game using this to gate an ending showed the ending's
 * option from the start.
 *
 * Two numberings come out of this, and keeping them apart is the whole of the
 * work. A *declared* index is the option's position in the script; an
 * *offered row* is a line on screen. With a condition they differ, and three
 * things index a menu: the text layer draws rows, the rollback log records
 * the answer, and the CHOICE statements are numbered as declared. Ren'Py's
 * menu returns the unfiltered index too (ast.py:1809), which is what makes a
 * recorded answer survive a condition changing underneath it.
 *
 * Runs on tools/harness/story, label cond_menu.
 */
#include <stdio.h>
#include <string.h>

#include "mr.h"
#include "history.h"
#include "expr.h"
#include "interp.h"
#include "vfs.h"
#include "script.h"
#include "image.h"
#include "scene.h"
#include "ui.h"
#include "tl.h"

static mr_surface *g_st;
mr_surface *mr_stage(void) { return g_st; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); }
void mr_log(const char *f, ...) { (void)f; }
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

/* A frame, as the real loop runs one: update, then tell the interpreter
 * whether a *finished* line has now been painted. That report is not
 * decoration -- mr_interp_redraw_kind keeps asking for frames until it
 * arrives, so a harness that never sends it sees the engine ask for frames
 * forever and cannot tell a line that animates from one that does not. */
static void step(void)
{
    const char *who, *what;
    mr_u32 c;
    int shown = -1;

    mr_interp_update(T);
    if (mr_interp_dialogue(&who, &what, &c, &shown))
        mr_interp_say_painted(shown < 0);
    T += 33;
}

static const char *line(void)
{
    static char cur[400];
    const char *who, *what;
    mr_u32 c;
    int shown;

    if (mr_interp_dialogue(&who, &what, &c, &shown))
        sprintf(cur, "%.200s", what);
    else
        strcpy(cur, "(no text)");
    return cur;
}

/* The speaker of the line on screen, "" for the narrator, NULL for no line. */
static const char *speaker(void)
{
    const char *who, *what;
    mr_u32 c;
    int shown;

    if (!mr_interp_dialogue(&who, &what, &c, &shown))
        return NULL;
    return who;
}

static int rows(const char **t, int max)
{
    int n = 0;
    if (!mr_interp_menu(&n, t, max))
        return 0;
    return n;
}

static int menu_up(void)
{
    const char *t[8];
    return rows(t, 8) > 0;
}

/* Forward to the menu, or to a line of dialogue if there is no menu left to
 * reach. Returns 1 if a menu came up. */
static int to_the_menu(void)
{
    int i;
    for (i = 0; i < 300; i++) {
        step();
        if (menu_up())
            return 1;
        mr_interp_advance();
    }
    return 0;
}

/* Walk until the line on screen contains `needle`. The fixture's conditional
 * menus are followed by named lines, so this is how "the menu was skipped"
 * is observed: the line after it arrives without a menu on the way. */
static int reach(const char *needle, int *saw_menu)
{
    int i;
    *saw_menu = 0;
    for (i = 0; i < 300; i++) {
        if (menu_up())
            *saw_menu = 1;
        if (strstr(line(), needle))
            return 1;
        mr_interp_advance();
        step();
    }
    return 0;
}

int main(int argc, char **argv)
{
    mr_script *s;
    const char *t[8];
    int n;

    setbuf(stdout, NULL);
    if (argc < 2) {
        fprintf(stderr, "usage: condtest <story-dir>\n");
        return 2;
    }

    mr_vfs_init(NULL, argv[1]);
    mr_image_init(argv[1]);
    mr_scene_init();
    s = mr_script_load(argv[1]);
    mr_interp_init(s, argv[1]);
    mr_ui_init(argv[1]);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);

    printf("\n-- a false condition is not offered --\n");
    mr_interp_start("cond_menu");
    check("the menu came up", to_the_menu());
    n = rows(t, 8);
    printf("   %d rows: %s | %s\n", n, n > 0 ? t[0] : "-",
           n > 1 ? t[1] : "-");
    /* Three declared; cond_a is False. */
    check("  two of the three declared options are offered", n == 2);
    check("  the unconditional one is first",
          n > 0 && !strcmp(t[0], "First, always"));
    check("  the one whose condition held is second",
          n > 1 && !strcmp(t[1], "Third, when unlocked"));
    check("  and the one whose condition failed is absent",
          !(n > 1 && strstr(t[1], "never")));

    printf("\n-- and a row is not a declared index --\n");
    /* Row 1 is declared option 2. Picking it has to run *that* block: an
     * engine that passes the row straight through to the CHOICE statements
     * runs option 1's block and sets cond_picked to 2. Both are plausible
     * numbers, which is exactly why the fixture has three options with the
     * middle one conditioned away rather than two. */
    mr_interp_choose(1);
    step();
    printf("   cond_picked = %ld (3 = the right branch, 2 = off by a row)\n",
           mr_var_get_int("cond_picked", -1));
    check("picking the second row runs the third option's block",
          mr_var_get_int("cond_picked", -1) == 3);

    printf("\n-- a menu with nothing left to offer is skipped --\n");
    {
        int saw;
        /* The fixture's second menu conditions everything away. Ren'Py runs
         * straight past it rather than showing an empty one; an empty menu
         * here would be unanswerable and the game would hang on it. */
        check("the line after the conditional menu arrives",
              reach("After the conditional menu", &saw));
        mr_interp_advance();
        step();
        /* This is the check that catches showing an empty menu rather than
         * skipping it: nothing is drawn and nothing can be clicked, so the
         * game simply stops here and this line never arrives. "No menu was
         * shown" stays green under that mutation, honestly -- a menu with no
         * rows shows nothing. The hang is the symptom, not the drawing. */
        check("the line after the empty menu arrives too",
              reach("never offered", &saw));
        check("  and no menu was shown on the way", !saw);
        check("  with no option's block having run",
              mr_var_get_int("cond_picked", -1) == 3);
    }

    printf("\n-- config.menu_include_disabled keeps it, unpickable --\n");
    /* Off by default, which is why everything above sees the option dropped.
     * Turned on, a failed condition still produces a row, but its
     * ChoiceReturn is built with `sensitive=condition`
     * (menuexports.py:101-110) -- so it draws insensitive and is not
     * focusable, which in Ren'Py means it takes neither hover nor click. */
    {
        int y, seen_mid = 0, seen_first = 0, seen_last = 0;

        mr_expr_exec("config.menu_include_disabled = True");
        mr_interp_start("cond_menu");
        check("the menu came up", to_the_menu());
        n = rows(t, 8);
        printf("   %d rows, sensitive: %d %d %d\n", n,
               mr_interp_menu_sensitive(0), mr_interp_menu_sensitive(1),
               mr_interp_menu_sensitive(2));
        check("  all three declared options are rows now", n == 3);
        check("  and the middle one is the insensitive one",
              mr_interp_menu_sensitive(0) && !mr_interp_menu_sensitive(1) &&
              mr_interp_menu_sensitive(2));

        /* The hit test, swept down the middle of the stage rather than at a
         * computed rectangle: menu_rect is private to the text layer, and
         * what matters is only that no point anywhere resolves to that row. */
        for (y = 0; y < MR_STAGE_H; y++) {
            int hit = mr_ui_menu_hit(MR_STAGE_W / 2, y);
            if (hit == 0) seen_first = 1;
            if (hit == 1) seen_mid = 1;
            if (hit == 2) seen_last = 1;
        }
        printf("   hit test reaches rows: %s%s%s\n",
               seen_first ? "0 " : "", seen_mid ? "1 " : "",
               seen_last ? "2" : "");
        check("  the pointer can reach the two live rows",
              seen_first && seen_last);
        check("  and never the disabled one", !seen_mid);

        /* And the same rule at the one place a choice is acted on, so that no
         * other caller can route around the hit test. */
        mr_interp_choose(1);
        step();
        check("  choosing it directly does nothing",
              mr_var_get_int("cond_picked", -1) == 0 && menu_up());
        mr_expr_exec("config.menu_include_disabled = False");
    }

    printf("\n-- the recorded answer is the declared index --\n");
    /* Which is what makes it survive the condition changing. The log holds
     * "option 2 of the three declared", not "the second row", so a rollback
     * that lands back on the menu can still find it however the rows have
     * been renumbered since. */
    {
        int i;

        mr_expr_exec("config.rollback_enabled = True");
        mr_rollback_reset();
        mr_interp_start("cond_menu");
        check("back at the menu", to_the_menu());
        mr_interp_choose(1);             /* declared 3 */
        step();

        /* Stepping after each one: a rollback restores the pc of the menu
         * statement, and it is executing that statement that re-evaluates
         * the conditions and puts the rows back up. */
        for (i = 0; i < 20 && !menu_up(); i++) {
            mr_interp_rollback();
            step();
        }
        printf("   %d rollbacks to get back to the menu\n", i);
        check("rolling back reaches the menu again", menu_up());
        check("  and the choice's effect is undone",
              mr_var_get_int("cond_picked", -1) == 0);

        /* Still three declared, still two offered: nothing has changed, so
         * the recorded answer is replayable. */
        mr_interp_rollforward();
        step();
        printf("   after rollforward: cond_picked = %ld\n",
               mr_var_get_int("cond_picked", -1));
        check("rolling forward answers it with the recorded option",
              mr_var_get_int("cond_picked", -1) == 3);
    }

    printf("\n-- unless the condition has since turned it off --\n");
    /* `if roll_forward not in choices: roll_forward = None`
     * (menuexports.py:308). The recorded option is no longer on offer, so
     * there is nothing to replay and the rollforward stops at the menu
     * instead of picking something else. */
    {
        int i;

        mr_rollback_reset();
        mr_interp_start("cond_menu");
        check("at the menu once more", to_the_menu());
        mr_interp_choose(1);             /* declared 3 again */
        step();

        for (i = 0; i < 20 && !menu_up(); i++) {
            mr_interp_rollback();
            step();
        }
        check("rolled back to it", menu_up());

        /* Turn the answered option off.
         *
         * Through persistent data, and that is not incidental: an ordinary
         * variable cannot do this, because the rollback that steps off the
         * menu restores the store and brings the old value back with it. The
         * first version of this test set a plain variable and silently
         * measured nothing -- two rows still offered, the rollforward
         * answering as before. Ren'Py behaves the same way, so the honest
         * trigger is the one a real game uses to gate an ending. */
        mr_expr_exec("persistent.cond_ok = False");
        mr_interp_rollback();            /* off the menu, onto the line before */
        step();
        mr_interp_rollforward();         /* and back onto the menu statement */
        step();
        n = rows(t, 8);
        printf("   %d row now: %s\n", n, n > 0 ? t[0] : "-");
        check("only the unconditional option is left", n == 1);

        mr_interp_rollforward();
        step();
        printf("   cond_picked = %ld, menu still up: %s\n",
               mr_var_get_int("cond_picked", -1), menu_up() ? "yes" : "no");
        check("the rollforward stops at the menu", menu_up());
        check("  and picks nothing on its own",
              mr_var_get_int("cond_picked", -1) == 0);
    }

    /* ---------------------------------------------------------------- *
     * A menu's caption, and what it did to the options.
     *
     * Reported from play:
     *
     *     menu:
     *         h "So, was it a date or not?"
     *         "Yes, probably... maybe...":   -> ran nothing, fell past the menu
     *         "No...":                       -> ran the *first* option's block
     *
     * and the caption never appeared. One cause for all of it: pass one
     * emitted a CHOICE only for lines starting with a quote, pass two counted
     * every line at the body indent as an option, so the caption consumed the
     * first CHOICE's slot and every option's target slid up by one. The last
     * option's block became unreachable.
     *
     * What makes this testable is the fixture's shape: each option runs
     * several statements and the line after the menu is distinct from all of
     * them, so a shifted target lands on a real line and only naming the line
     * tells right from wrong.
     * ---------------------------------------------------------------- */
    printf("\n-- a menu that opens with a line of dialogue --\n");
    {
        mr_expr_exec("config.menu_include_disabled = False");
        mr_interp_start("caption_menu");
        check("the menu came up", to_the_menu());
        printf("   caption: %s \"%s\"\n",
               speaker() ? speaker() : "(none)", line());
        check("the caption is displayed",
              strstr(line(), "was it a date") != NULL);
        check("  by the character who says it",
              speaker() && !strcmp(speaker(), "Ada"));

        n = rows(t, 8);
        printf("   %d rows: %s | %s\n", n, n > 0 ? t[0] : "-",
               n > 1 ? t[1] : "-");
        check("  and the caption is not one of the options", n == 2);
        check("  the first option is the first option",
              n > 0 && strstr(t[0], "Yes, probably"));
        check("  and the second is the second",
              n > 1 && !strcmp(t[1], "No..."));
    }

    printf("\n-- and it types itself out while the menu is up --\n");
    /* Reported from play as "the text is displayed after I press space or
     * click". The caption was being set up correctly and drawn once; what was
     * missing was any reason to draw it again. ST_MENU answered
     * MR_REDRAW_NONE unconditionally, so the frame loop went idle with the
     * line part-revealed and only woke on input -- at which point the whole
     * line appeared at once and looked like it had been waiting for the
     * click.
     *
     * So the assertion is on the engine asking for frames, which is the thing
     * that was wrong, rather than on the characters appearing: the reveal
     * itself always worked. */
    {
        int shown = -1, frames = 0, asked = 0, max_shown = -1;
        const char *who, *what;
        mr_u32 c;

        mr_interp_dialogue(&who, &what, &c, &shown);
        printf("   on arrival the caption is at %d characters\n", shown);
        check("the caption starts out part-revealed, not complete",
              shown >= 0);

        for (frames = 0; frames < 400; frames++) {
            mr_interp_dialogue(&who, &what, &c, &shown);
            if (shown > max_shown)
                max_shown = shown;
            if (shown < 0)
                break;
            if (mr_interp_redraw_kind() != MR_REDRAW_NONE)
                asked++;
            step();
        }
        printf("   revealed to %d over %d frames, %d of them asked for a "
               "repaint\n", max_shown, frames, asked);
        check("  it finishes on its own, with nothing pressed", shown < 0);
        check("  having asked for a frame all the way through",
              frames > 1 && asked >= frames);
        /* And then it stops: a static menu with a finished caption must not
         * hold the frame loop awake, which on the target costs real time. */
        step();
        check("  and goes quiet once the whole line is painted",
              mr_interp_redraw_kind() == MR_REDRAW_NONE);
        check("  with the menu still up and unanswered", menu_up());
    }

    printf("\n-- and a press finishes it early, as it does any line --\n");
    /* The press is consumed and does nothing else: it completes the caption,
     * it does not answer the menu, and there is nothing to advance to. In
     * Ren'Py this is the Text displayable's own dismiss handling, which ends
     * every slow text on screen and raises IgnoreEvent (text/text.py:2661).
     *
     * Reached from the top of the label so the caption is genuinely part way
     * through -- pressing at a line that has already finished revealing
     * proves nothing. */
    {
        int shown = -1, before_rows, i;
        const char *who, *what;
        mr_u32 c;

        mr_interp_start("caption_menu");
        check("back at the captioned menu", to_the_menu());
        /* A few frames first: the menu's own frame leaves the caption at
         * zero characters, and jumping to complete from zero would also
         * happen if the press merely restarted it. Press from a partial
         * reveal instead. */
        for (i = 0; i < 4; i++)
            step();
        mr_interp_dialogue(&who, &what, &c, &shown);
        before_rows = rows(t, 8);
        printf("   pressing at %d characters, %d rows up\n", shown,
               before_rows);
        check("the caption is part way through, not at the start",
              shown > 0);

        mr_interp_advance();
        step();

        mr_interp_dialogue(&who, &what, &c, &shown);
        printf("   after the press: shown=%d, \"%s\"\n", shown, line());
        check("  the press completes it", shown < 0);
        check("  the whole line is there",
              strstr(line(), "was it a date or not?") != NULL);
        check("  the menu is still up", menu_up());
        check("  with the same rows", rows(t, 8) == before_rows);
        check("  and nothing was answered",
              mr_var_get_int("wasadate", -1) == -1);

        /* Pressing on must be *inert*, and "is the menu still up" does not
         * say that. Letting the branch fall through to the ordinary advance
         * -- g_has_dialogue = 0, g_state = ST_RUN -- leaves g_pc on the menu
         * statement, so the next update simply re-executes it and the menu
         * heals itself: still up, still unanswered, nothing to see. That
         * mutation passed every check here until these two.
         *
         * What a re-run really costs is visible in the two things the menu
         * statement does on arrival: it displays the caption, which restarts
         * the reveal from zero, and it adds a history entry. */
        {
            int hist = mr_history_count();

            for (i = 0; i < 5; i++) {
                mr_interp_advance();
                step();
            }
            mr_interp_dialogue(&who, &what, &c, &shown);
            printf("   after five more presses: shown=%d, history %d -> %d, "
                   "wasadate = %ld\n", shown, hist, mr_history_count(),
                   mr_var_get_int("wasadate", -1));
            check("  pressing on does not leave the menu", menu_up());
            check("  nor run an option's block",
                  mr_var_get_int("wasadate", -1) == -1);
            check("  nor restart the caption", shown < 0);
            check("  nor repeat it in the history",
                  mr_history_count() == hist);
        }
    }

    printf("\n-- each option runs its own block --\n");
    {
        /* Row 0. With the caption eating a slot this ran nothing at all and
         * went straight to the line after the menu. */
        int saw;
        mr_interp_choose(0);
        step();
        printf("   after picking the first: \"%s\"\n", line());
        check("the first option's first line comes up",
              strstr(line(), "Maybe it was, after all") != NULL);
        check("  and its block ran", mr_var_get_int("wasadate", -1) == 1);
        check("  the rest of its block follows",
              reach("One can always dream", &saw));
        check("  then the line after the menu",
              reach("heading home", &saw));
    }
    {
        /* Row 1. With the shift this ran row 0's block -- which is the
         * confusing half of the report: picking "No..." said "Maybe it was,
         * after all." */
        int saw;
        mr_interp_start("caption_menu");
        check("back at the captioned menu", to_the_menu());
        mr_interp_choose(1);
        step();
        printf("   after picking the second: \"%s\"\n", line());
        check("the second option's first line comes up",
              strstr(line(), "I don't think it was") != NULL);
        check("  and its block ran, not the first's",
              mr_var_get_int("wasadate", -1) == 0);
        check("  the rest of its block follows",
              reach("maybe one day", &saw));
        check("  then the line after the menu",
              reach("heading home", &saw));
    }

    printf("\n-- a bare string caption is narrated --\n");
    {
        int saw;
        check("on to the second menu", reach("heading home", &saw) ||
              mr_var_get_int("wasadate", -1) == 0);
        check("its menu came up", to_the_menu());
        printf("   caption: %s \"%s\"\n",
               speaker() ? speaker() : "(none)", line());
        check("the caption is displayed",
              strstr(line(), "nobody saying it") != NULL);
        /* `config.narrator_menu` is True by default (config.py:453), so a
         * bare caption is said by the narrator rather than becoming an
         * unselectable row in the menu. */
        check("  with no speaker",
              speaker() && !speaker()[0]);
        n = rows(t, 8);
        check("  and it is not a row", n == 2);
        mr_interp_choose(1);
        step();
        check("  the options still line up",
              mr_var_get_int("wasadate", -1) == 11);
    }

    printf("\n-- and it is translated like the say it is in Ren'Py --\n");
    /* A caption's translation identifier, which was wrong and silently so:
     * the line simply stayed in the source language.
     *
     * Ren'Py's caption say carries interact=False, and `get_code` appends the
     * word "nointeract" to the code it digests (ast.py:932) -- before any
     * `with` clause, which goes last. So a caption's identifier differs from
     * the same line written as ordinary dialogue.
     *
     * The two constants below are an *external* oracle, which matters here
     * because every other translation check in the harness compares our
     * parser against our catalogue reader -- a digest that is wrong
     * consistently passes those. `chap1_939df797` is what Ren'Py put in a
     * generated tl/french/script.rpy for
     *
     *     menu:
     *         h "So, was it a date or not?"
     *
     * in `label chap1`, with the line written out as
     * `# h "So, was it a date or not?" nointeract`. `chap1_851ede00` is the
     * same line without the word -- well-formed, in no catalogue, and what
     * we were computing. */
    {
        char got[MR_MAX_NAME + 24];

        mr_tl_identifier("chap1", "h", "So, was it a date or not?", NULL,
                         1, 0, got, sizeof(got));
        printf("   caption   -> %s (Ren'Py: chap1_939df797)\n", got);
        check("a caption's id matches Ren'Py's",
              !strcmp(got, "chap1_939df797"));

        mr_tl_identifier("chap1", "h", "So, was it a date or not?", NULL,
                         0, 0, got, sizeof(got));
        printf("   plain say -> %s (Ren'Py: chap1_851ede00)\n", got);
        check("  and is not the id of the same line as dialogue",
              !strcmp(got, "chap1_851ede00"));

        /* And the parser actually puts one on the statement -- the constants
         * above only prove the function, not that the MENU statement carries
         * its result. The fixture's caption is `a "So, was it a date or
         * not?"` in label caption_menu, so this is the same digest under a
         * different label. */
        {
            int i, found = -1;
            char want[MR_MAX_NAME + 24];

            mr_interp_start("caption_menu");
            for (i = 0; i < s->n; i++) {
                if (s->st[i].kind == MR_ST_MENU && s->st[i].text &&
                    strstr(s->st[i].text, "was it a date")) {
                    found = i;
                    break;
                }
            }
            check("the fixture's captioned MENU statement was found",
                  found >= 0);
            mr_tl_identifier("caption_menu", "a",
                             "So, was it a date or not?", NULL, 1, 0,
                             want, sizeof(want));
            printf("   fixture   -> %s, wanted %s\n",
                   found >= 0 && s->st[found].tlid ? s->st[found].tlid :
                   "(none)", want);
            check("  and carries the caption identifier",
                  found >= 0 && s->st[found].tlid &&
                  !strcmp(s->st[found].tlid, want));
        }

        /* The other caption form goes to the other catalogue, and the
         * absence of an identifier is what says so.
         *
         * A bare string caption is not a Say node in Ren'Py: it is a Menu
         * item, and Menu.get_translation_strings hands every item caption to
         * the *string* catalogue (ast.py:1880) -- the same place the choice
         * captions go. An identifier here would be a well-formed id that no
         * catalogue can contain, so the lookup could never hit and the
         * caption would silently stay in the source language. Which is what
         * it did until this check existed. */
        {
            int i, found = -1;

            for (i = 0; i < s->n; i++) {
                if (s->st[i].kind == MR_ST_MENU && s->st[i].text &&
                    strstr(s->st[i].text, "nobody saying it")) {
                    found = i;
                    break;
                }
            }
            check("the fixture's bare-string caption was found", found >= 0);
            printf("   bare caption: speaker \"%s\", id %s\n",
                   found >= 0 ? s->st[found].name : "?",
                   found >= 0 && s->st[found].tlid ? s->st[found].tlid :
                   "(none, as it should be)");
            check("  it has no speaker, which is what identifies the form",
                  found >= 0 && !s->st[found].name[0]);
            check("  and no say identifier, so the string catalogue answers",
                  found >= 0 && !s->st[found].tlid);
        }
    }

    printf("\n-- the caption is a history entry, and only one --\n");
    /* The rollback log pops one history entry per checkpoint it drops that
     * wrote one. A bare menu writes none and a say writes one; a captioned
     * menu is a menu that writes one, which is why it has a checkpoint kind
     * of its own. Counted either side of the menu rather than predicted. */
    {
        int before, at_menu, i;

        mr_expr_exec("config.rollback_enabled = True");
        mr_rollback_reset();
        mr_history_reset();
        mr_interp_start("caption_menu");
        check("at the captioned menu", to_the_menu());
        at_menu = mr_history_count();
        printf("   history at the menu: %d\n", at_menu);
        check("the caption went into the history", at_menu > 0);
        {
            const char *who = NULL, *what = NULL;
            mr_u32 col;
            /* Index 0, not count-1: mr_history_get is newest first. */
            mr_history_get(0, &who, &what, &col);
            printf("   newest entry: %s \"%s\"\n", who ? who : "(none)",
                   what ? what : "");
            check("  as its own entry, with its speaker",
                  what && strstr(what, "was it a date") && who &&
                  !strcmp(who, "Ada"));
        }

        mr_interp_choose(0);
        step();
        for (i = 0; i < 3; i++) {
            mr_interp_advance();
            step();
        }
        before = mr_history_count();

        for (i = 0; i < 20 && !menu_up(); i++) {
            mr_interp_rollback();
            step();
        }
        printf("   %d entries after the menu, %d back at it (was %d)\n",
               before, mr_history_count(), at_menu);
        check("rolling back reaches the menu", menu_up());
        check("  and the history is exactly what it was there",
              mr_history_count() == at_menu);

        /* One more press has to *leave* it, and this is where a captioned
         * menu went wrong: displaying the caption through do_say took a say
         * checkpoint, and the menu took its own, so there were two points at
         * the same pc. Rolling back dropped both and landed on the menu it
         * was already showing -- Page Up appeared to do nothing. The same
         * regression as the one branchtest pins for an uncaptioned menu, and
         * this is the captioned half of it. */
        mr_interp_rollback();
        step();
        printf("   one more rollback -> \"%.40s\"\n", line());
        check("one rollback at a captioned menu actually leaves it",
              !menu_up());
    }

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}

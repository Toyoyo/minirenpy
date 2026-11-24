/* Switching language must change the line already on screen, change the menu
 * labels, and drop the rollback log -- and a save written in one language must
 * come back translated rather than in the original.
 *
 * The archive is mounted rather than the loose directory: the catalogue is
 * found by enumerating tl/<lang>/, and mr_vfs_enum only enumerates loose files
 * on Windows.
 */
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
#include "tl.h"
#include "anygame.h"

static mr_surface *g_st;
mr_surface *mr_stage(void) { return g_st; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); }
void mr_log(const char *f, ...) { (void)f; }
void mr_quit(void) {}
void mr_invalidate(void) {}
void hostshim_wave_sink(const short *d, int n) { (void)d; (void)n; }

static long T;
static int  fails;
static char cur[400];

static void snap(void)
{
    const char *who, *what;
    mr_u32 c;
    int shown;
    if (mr_interp_dialogue(&who, &what, &c, &shown))
        sprintf(cur, "%.200s", what);
    else
        strcpy(cur, "(no text)");
}

static void step(void)
{
    mr_interp_update(T);
    T += 33;
}

static void check(const char *label, int ok)
{
    printf("%-44s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

/* Is a game-menu screen on the scene? Named rather than counted: the quick
 * menu is a screen as well, so the count is 1 with no menu open and 1 again
 * with one. */
static int menu_up(void)
{
    int i;
    for (i = 0; i < mr_scene_screen_count(); i++) {
        const char *n = mr_scene_screen_name(i);
        if (n && (!strcmp(n, "game_menu") || !strcmp(n, "preferences") ||
                  !strcmp(n, "navigation") || !strcmp(n, "save") ||
                  !strcmp(n, "load")))
            return 1;
    }
    return 0;
}

static const char *screen_list(void)
{
    static char out[200];
    int i;
    out[0] = '\0';
    for (i = 0; i < mr_scene_screen_count(); i++) {
        const char *n = mr_scene_screen_name(i);
        if (strlen(out) + strlen(n ? n : "?") + 2 >= sizeof(out))
            break;
        if (i) strcat(out, " ");
        strcat(out, n ? n : "?");
    }
    return out[0] ? out : "(none)";
}

static const char *ev(const char *expr)
{
    static char out[160];
    mr_value v;
    mr_expr_eval(expr, &v);
    sprintf(out, "%.150s", v.type == MR_V_STR ? v.s : "(not a string)");
    return out;
}

int main(int argc, char **argv)
{
    mr_script *s;
    int i;
    /* `orig` and `other`, not `french` and `english`. Which is which
     * depends on the game: Miwa is written in French and ships tl/english,
     * Hinata is written in English and ships tl/french. See anygame.h. */
    char orig[400], other[400];
    char lang[64], label[MR_MAX_NAME], key[64], orig_key_text[256];
    int  have_lang, translated, have_key;

    setbuf(stdout, NULL);
    if (argc < 3) {
        fprintf(stderr, "usage: langtest <pak> <game-dir>\n");
        return 2;
    }

    mr_vfs_init(argv[1], argv[2]);
    mr_image_init(argv[2]);
    mr_scene_init();
    s = mr_script_load(argv[2]);
    mr_interp_init(s, argv[2]);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);

    have_lang = anygame_other_language(lang, sizeof(lang));
    if (!have_lang) {
        /* Everything below is about switching language. A monolingual game
         * cannot exercise any of it, and saying so beats reporting a green
         * run on a test that did nothing. */
        printf("\nSKIP: this game ships no tl/<lang>/ catalogue besides its "
               "own language,\n      so there is no language to switch "
               "into.\n");
        /* 77, not 0: everything in this test is about switching language,
         * and returning 0 here reported a pass for a run that asserted
         * nothing. run.sh counts 77 as a skip. It is also what happens
         * with no pak built, since mr_vfs_enum cannot see a loose tl/. */
        return 77;
    }
    printf("other language    : %s\n", lang);

    /* Where to stand: a label whose dialogue this catalogue actually
     * translates, not merely one with a lot of dialogue.
     *
     * Shipping a tl/<lang>/ and translating the lines in it are two
     * different things. A game can carry strings only, or be translated
     * from chapter three onwards, and then the label anygame_story_label
     * picks has nothing to switch -- "the line on screen changed" fails on
     * a correct engine, which is the shape of failure this whole header
     * exists to stop. Verified against three games: two translate
     * everything and pick the same label either way, Azami ships no second
     * language at all and skips above.
     */
    translated = anygame_translated_label(s, lang, label, sizeof(label));
    if (translated < 2) {
        printf("\nSKIP: this game ships tl/%s/ but no label in it has two "
               "translated\n      lines in a row, so there is no dialogue "
               "to watch change.\n", lang);
        return 77;
    }
    printf("start label       : %s (%d translated lines)\n",
           label, translated);
    mr_interp_start(label);

    /* Run a few lines so there is a rollback log to lose. */
    for (i = 0; i < 40; i++) {
        step();
        snap();
        if (strcmp(cur, "(no text)"))
            break;
        mr_interp_advance();
    }
    for (i = 0; i < 3; i++) {
        mr_interp_advance();
        step();
    }
    snap();
    strcpy(orig, cur);
    printf("\nline, original    : %s\n", orig);
    printf("depth before      : %d\n", mr_rollback_depth());
    check("rollback log has something in it", mr_rollback_depth() > 0);

    /* A `_()` key the catalogue actually changes, found by asking rather
     * than named. This used to be `_("Retour")` expecting "Retour" then
     * "Back" -- which is only a key at all in a game written in French.
     * anygame.h explains why the candidates are Ren'Py's date names: those
     * are keyed in English in every game whatever its own language. */
    {
        char before[256], act[128];

        /* Not a check: a catalogue that translates dialogue and leaves
         * Ren'Py's own strings alone is a legal, shippable catalogue, and
         * failing on one would be a report about the game. The two
         * assertions that need the key are skipped by name instead, out
         * loud, so this cannot quietly become a test that asserts nothing
         * -- the half it can still do is the half about dialogue, which is
         * the subject anyway. */
        have_key = anygame_translated_key(lang, key, sizeof(key));
        before[0] = '\0';
        orig_key_text[0] = '\0';
        if (have_key) {
            strcpy(before, mr_tl_string(key));
            strcpy(orig_key_text, before); /* for the revert check below */
            printf("probe key         : %s -> %s\n", key, before);
        } else {
            printf("probe key         : none -- tl/%s/ translates no "
                   "engine string this\n                    test knows "
                   "of; the two checks that need one are skipped\n", lang);
        }

        /* --- switch --- */
        sprintf(act, "Language(\"%s\")", lang);
        mr_interp_action(act);
        step();
        snap();
        strcpy(other, cur);
        printf("\nline, %-12s: %s\n", lang, other);
        check("the line on screen changed", strcmp(other, orig) != 0);
        check("and is not empty", other[0] && strcmp(other, "(no text)"));
        if (have_key)
            check("the engine string changed with it",
                  strcmp(mr_tl_string(key), before) != 0);
        else
            printf("%-46s %s\n", "the engine string changed with it",
                   "skipped, no probe key");
    }

    /* The character-name assertion that used to be here -- `_("Maman")`
     * becoming "Mom" -- is gone rather than generalised. A character's
     * displayed name is the script's own text, so there is no key to name
     * that exists in two games, and deriving one from s->chars would only
     * assert that *this* game translated *that* character, which is a fact
     * about the game. The mechanism it covered, mr_tl_string on a `_()`
     * key, is covered by the probe above. */
    /* Switching throws the log away, but the line on screen must get a fresh
     * checkpoint, or advancing one line would leave the first translated line
     * as the one line that cannot be rolled back to. Depth counts steps
     * *back*, so a single checkpoint reads as zero -- there is nowhere to go
     * from the line you are on -- and the observable that matters is what
     * happens after advancing. */
    check("nothing to roll back to yet", mr_rollback_depth() == 0);

    /* Advance until the text actually changes, rather than once.
     *
     * The switch re-runs the say statement, so the line is revealing again
     * and the first advance completes the reveal instead of moving on --
     * which is correct behaviour, and is why there is a second
     * mr_interp_advance with that very comment further down. With a
     * hardcoded chap12 the preamble happened to leave the reveal finished
     * and one advance was enough; from a label chosen by anygame.h it is
     * not, and "advanced" failed while the engine was doing the right
     * thing. Bounded, so a genuine failure to advance still fails. */
    {
        int tries;

        for (tries = 0; tries < 8; tries++) {
            mr_interp_advance();
            step();
            snap();
            if (strcmp(cur, other) != 0)
                break;
        }
        printf("next line         : %s\n", cur);
        check("advanced", strcmp(cur, other) != 0);
    }
    mr_interp_rollback();
    step();
    snap();
    printf("rolled back to    : %s\n", cur);
    check("rolled back to the first translated line", !strcmp(cur, other));
    check("and no further back is offered", mr_rollback_depth() <= 1);

    /* --- a save written now must load back translated --- */
    mr_save_screenshot(g_st);
    check("save written", mr_save_write("9-1", g_st) == 1);
    mr_interp_advance();   /* finishes the reveal the re-run say restarted */
    step();
    mr_interp_advance();
    step();
    snap();
    check("moved off the line", strcmp(cur, other) != 0);
    check("save loaded", mr_save_load("9-1") == 1);
    step();
    snap();
    printf("after load        : %s\n", cur);
    check("loaded line is the translated one", !strcmp(cur, other));
    mr_save_delete("9-1");

    /* --- and back --- */
    mr_interp_action("Language(None)");
    step();
    snap();
    printf("\nback to original  : %s\n", cur);
    check("the line reverted", !strcmp(cur, orig));
    if (have_key)
        check("the engine string reverted with it",
              !strcmp(mr_tl_string(key), orig_key_text));
    else
        printf("%-46s %s\n", "the engine string reverted with it",
               "skipped, no probe key");
    check("no language in force", mr_tl_language() == NULL);

    /* --- the condition the story itself branches on for chapter titles --- */
    {
        mr_value v;
        char act[128], test[160];

        sprintf(act, "Language(\"%s\")", lang);
        sprintf(test, "_preferences.language == \"%s\"", lang);

        mr_interp_action(act);
        step();
        mr_expr_eval(test, &v);
        check("the script's own language test is true", mr_value_truthy(&v));
        mr_interp_action("Language(None)");
        step();
        mr_expr_eval(test, &v);
        check("and false in the original language", !mr_value_truthy(&v));
    }

    /* --- the original language has a catalogue too ---
     *
     * tl/None/ is where a game translates Ren'Py's *own* English strings
     * into the language it is written in -- this one is French, and
     * tl/None/common.rpym carries 383 of them. Ren'Py keys string
     * translations by language with None a valid key
     * (translation/__init__.py:615), so those apply whenever no language is
     * selected, which is the default every player starts in.
     *
     * We loaded nothing at all for the original language, so every string
     * the engine supplies -- the overwrite prompt, the quick-save notice,
     * the page names -- showed in English inside a French game. Nothing
     * caught it because the dialogue, which is the script's own text, was
     * right.
     */
    mr_interp_action("Language(None)");
    step();
    {
        /* The two keys are Ren'Py's own English strings, which is what
         * makes them usable here: they come from renpy/common and are keys
         * in every game regardless of the language it is written in.
         *
         * What is asserted is that tl/None *applied*, not what it said.
         * The expected text used to be spelled out in French -- the
         * overwrite prompt with its U+00A0 before the question mark -- and
         * that is Miwa's. On a game written in English, tl/None carries
         * English and translation is correctly the identity, so there is
         * nothing to see and the check says so instead of failing.
         */
        static const char *keys[] = {
            "Are you sure you want to overwrite your save?",
            "Quick save complete."
        };
        int i, changed = 0;

        for (i = 0; i < 2; i++) {
            const char *out = mr_tl_string(keys[i]);
            printf("  tl/None: \"%.34s\" -> \"%.40s\"\n", keys[i],
                   out ? out : "(null)");
            if (out && strcmp(out, keys[i]) != 0)
                changed++;
        }

        if (changed) {
            check("  engine strings are translated in the original language",
                  changed == 2);
        } else {
            printf("  SKIP: this game's tl/None/ leaves Ren'Py's own strings "
                   "as they are,\n        which is what a game written in "
                   "English looks like.\n");
        }
    }

    /* --- switching language must not stitch the game menu into the story --
     *
     * Reported from play, exactly these steps: advance, right-click into the
     * menu, switch language, right-click out, advance, roll back -- and the
     * game menu came back up with the dialogue box drawn over it.
     *
     * The Language action throws the rollback log away (it replays text from
     * the other language) and takes a fresh checkpoint so the line on screen
     * is not the one line unreachable by rollback. But it runs from inside
     * the preferences screen, so the captured scene had the menu in it, and
     * rolling back to that line restored the menu as part of the story.
     *
     * mr_save_write has the same filter at save.c:350 for the same reason --
     * a save is always taken with the menu up. The checkpoint simply never
     * got it.
     */
    {
        /* Counting screens is not the question: the quick menu is a screen
         * too, so the baseline is 1 and opening the game menu swaps it
         * rather than adding to it. The first version of this check asked
         * for a count and read 1 both before and after -- it passed the
         * "no menu" assertion while the menu was up. Ask by name. */
        int had_menu;

        printf("\n-- language switch, then roll back --\n");

        mr_interp_advance();
        step();
        snap();
        printf("   on: %.50s\n", cur);
        check("the story is up, no game menu", !menu_up());

        mr_interp_game_menu_toggle();     /* right-click in */
        step();
        had_menu = menu_up();
        printf("   screens with the menu open: %s\n", screen_list());
        check("  the menu opened", had_menu);

        mr_interp_action("Language(None)");
        step();

        mr_interp_game_menu_toggle();     /* right-click out */
        step();
        check("  and closed again", !menu_up());

        mr_interp_advance();              /* one more line */
        step();
        snap();

        mr_interp_rollback();
        step();
        snap();
        printf("   after rolling back: screens %s, text %.40s\n",
               screen_list(), cur);

        /* The assertion the bug failed. Asked of the scene rather than of
         * what is drawn, because the menu came back *under* the dialogue
         * box -- a test that only looked at the text would have passed. */
        check("rolling back does not bring the menu with it", !menu_up());
    }

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}

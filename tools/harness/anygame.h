/* anygame.h - pick a starting point and a language out of whatever game is
 * mounted, instead of naming Miwa's.
 *
 * Why this exists
 * ---------------
 *
 * Several harnesses need "somewhere in the story with dialogue to step
 * through" and "a second language to switch into". They used to spell those
 * as `mr_interp_start("chap12")` and `Language("english")`, which are
 * Miwa's. The engine runs more than one game and the suite has to say so.
 *
 * `chap12` is the instructive one. Hinata has a `label chap12` too, so
 * seamtest and langtest both passed against it -- and that proves nothing,
 * because the two games share a Ren'Py GUI template and an author. Two
 * games agreeing on a value is not evidence it is a constant; it is
 * evidence they have the same habits. A third game would have failed on a
 * missing label, with the test reporting whatever `mr_interp_start` does
 * with a name it cannot find rather than reporting the real problem.
 *
 * Language is worse than arbitrary, it is backwards between the two games
 * on hand. Miwa is written in French and ships tl/english; Hinata is
 * written in English and ships tl/french. A test that names either one is
 * testing which language its author happened to write in.
 *
 * Static functions in a header rather than a linked .c: run.sh builds each
 * harness from its own single source plus the engine, and three callers do
 * not justify teaching it about a fourth kind of file.
 */

#ifndef MR_ANYGAME_H
#define MR_ANYGAME_H

#include <stdio.h>
#include <string.h>

#include "mr.h"
#include "script.h"
#include "interp.h"
#include "vfs.h"
#include "tl.h"

/* The label with the longest unbroken run of say statements after it.
 *
 * "Longest run of dialogue" and not "first label" or "most statements",
 * because what the callers do with it is step a few dozen lines and expect
 * text on screen each time. A label that opens with a scene, a transition
 * and a menu gives them nothing to read; `start` is usually exactly that,
 * being the splashscreen and the main menu on the way in.
 *
 * Ties go to the earlier label, so the answer is stable for a given script.
 * Returns 0 if the script has no labels at all, in which case the caller
 * has bigger problems than where to start.
 */
static int anygame_story_label(const mr_script *s, char *out, int outsz)
{
    /* Ren'Py's own entry points, which are not story even when they
     * contain dialogue. `start` is the one that matters: it picked itself
     * on Miwa, being where the prose begins, and the callers then stepped
     * into the opening's transitions and menus rather than into a plain run
     * of lines -- langtest's "advanced" check failed on it while the engine
     * was fine. A chapter label has no such machinery around it. */
    static const char *special[] = {
        "start", "splashscreen", "main_menu", "after_load", "quit",
        "before_main_menu", "after_warp", "credits"
    };
    const int nspecial = (int)(sizeof(special) / sizeof(special[0]));
    int best = -1, bestrun = -1;
    int i, k;

    if (!s || s->nlabels <= 0 || outsz <= 0)
        return 0;

    for (i = 0; i < s->nlabels; i++) {
        int j = s->labels[i].stmt;
        int run = 0;
        int skip = 0;
        int end = s->n;

        for (k = 0; k < nspecial; k++)
            if (strcmp(s->labels[i].name, special[k]) == 0)
                skip = 1;
        if (skip)
            continue;

        /* Stop at the next label. Without this a two-statement label
         * counts the dialogue of whatever happens to follow it in the
         * file and wins on it: a fixture label holding one `$ flag =
         * True` scored the thirty lines of the chapter below it, and
         * every harness that starts here began in a label with nothing
         * to read. Labels are not guaranteed to be in statement order,
         * so this is a scan and not labels[i + 1]. */
        for (k = 0; k < s->nlabels; k++)
            if (s->labels[k].stmt > s->labels[i].stmt &&
                s->labels[k].stmt < end)
                end = s->labels[k].stmt;

        while (j < end && s->st[j].kind == MR_ST_SAY) {
            run++;
            j++;
        }
        /* Not only the opening run: a label that begins with a scene and
         * then talks is still a good starting point, so look a little way
         * in before giving up on it. */
        if (run == 0) {
            int k = s->labels[i].stmt;
            int limit = k + 8;

            while (k < end && k < limit && s->st[k].kind != MR_ST_SAY)
                k++;
            j = k;
            while (j < end && s->st[j].kind == MR_ST_SAY) {
                run++;
                j++;
            }
        }

        if (run > bestrun) {
            bestrun = run;
            best = i;
        }
    }

    if (best < 0 || (int)strlen(s->labels[best].name) >= outsz)
        return 0;

    strcpy(out, s->labels[best].name);
    return 1;
}

/* Start the interpreter somewhere with dialogue, and say where.
 *
 * The three lines every caller of the above wrote out by hand, and the
 * thing seven harnesses did *not* do: they said `mr_interp_start("chap12")`
 * instead, which is one game's label. That survived for a long time because
 * the second game on hand has a chap12 too. Against a script without one
 * the interpreter sits at pc -1 and the symptom is never "no such label" --
 * persisttest saved its position as `?-1`, the load refused the slot, and
 * the red line said "save loaded ** FAIL **".
 *
 * Printing the label is part of the job: when a harness does fail, the
 * first question is where it was standing.
 */
static char anygame_label[MR_MAX_NAME];

static void anygame_start_story(const mr_script *s)
{
    char label[MR_MAX_NAME];

    anygame_label[0] = '\0';
    if (!anygame_story_label(s, label, sizeof(label))) {
        printf("no story label in the mounted script\n");
        return;
    }
    printf("start label: %s\n", label);
    strcpy(anygame_label, label);
    mr_interp_start(label);
}

/* A language this game ships a catalogue for, other than its own.
 *
 * tl/<lang>/ is where Ren'Py puts them, so the languages are the directory
 * names one level down. mr_vfs_enum matches a *prefix* and not a directory,
 * so the name is the span up to the next '/'.
 *
 * tl/None/ is skipped. It is Ren'Py's slot for strings in the game's
 * *original* language -- selecting it would be asking to switch into the
 * language the engine is already in, which is what the caller is switching
 * away from.
 *
 * Returns 0 if the game is monolingual, which a caller must treat as "skip
 * this check" rather than as a failure: a game with no second language is
 * not a misbehaving engine.
 */
static int anygame_other_language(char *out, int outsz)
{
    char path[256];
    int i;

    if (outsz <= 0)
        return 0;

    for (i = 0; mr_vfs_enum("tl/", NULL, i, path, sizeof(path)); i++) {
        const char *p = path + 3;
        const char *slash = strchr(p, '/');
        int n;

        if (!slash)
            continue;
        n = (int)(slash - p);
        if (n <= 0 || n >= outsz)
            continue;

        memcpy(out, p, (size_t)n);
        out[n] = '\0';
        /* Case-insensitively, because the directory is "None" on disk and
         * the archive lowercases its names. */
        if (strcmp(out, "none") == 0 || strcmp(out, "None") == 0)
            continue;
        return 1;
    }

    out[0] = '\0';
    return 0;
}

/* A `_("...")` key whose translation differs between the game's own
 * language and `lang`, or 0 if none of the candidates does.
 *
 * The candidates are Ren'Py's own date names. That looks like an odd choice
 * for a test about menu labels until you ask what else could be used: a
 * game-authored string like "Retour" is a key only in the language its
 * author wrote in, so it cannot be named here. Ren'Py's common strings are
 * keyed in English whatever the game's language, because they come from
 * renpy/common/*.rpy -- so "{#month}September" is a valid key in every
 * game, and any game shipping a catalogue at all has common.rpy in it.
 *
 * The tag is left on. The caller is comparing two catalogue answers against
 * each other, not against rendered output, so there is nothing to strip.
 */
static int anygame_translated_key(const char *lang, char *out, int outsz)
{
    static const char *keys[] = {
        "{#month}September", "{#month}January", "{#weekday}Friday",
        "{#weekday}Monday", "{#month}December"
    };
    const int n = (int)(sizeof(keys) / sizeof(keys[0]));
    int i;

    for (i = 0; i < n; i++) {
        char own[256];
        const char *s;

        mr_tl_set_language(NULL);
        s = mr_tl_string(keys[i]);
        if (!s || strlen(s) >= sizeof(own))
            continue;
        strcpy(own, s);

        mr_tl_set_language(lang);
        s = mr_tl_string(keys[i]);
        if (!s)
            continue;

        if (strcmp(own, s) != 0 && (int)strlen(keys[i]) < outsz) {
            mr_tl_set_language(NULL);
            strcpy(out, keys[i]);
            return 1;
        }
    }

    mr_tl_set_language(NULL);
    if (outsz > 0)
        out[0] = '\0';
    return 0;
}

/* Somewhere else to be: the best story label that is not `avoid`.
 *
 * For the tests that need the interpreter to leave where it was -- a save
 * that has to restore a position, a jump that has to discard a
 * rollforward future. Those said `mr_interp_start("credits")` and
 * `Jump("chap2")`, which are one game's labels, and against a script
 * without them the jump quietly does nothing: the test then asserts that
 * the player moved, finds they did not, and reports a rollback bug.
 *
 * Returns 0 if the script has only one story label, which a caller has to
 * handle -- there is genuinely nowhere else to go.
 */
static int anygame_other_label(const mr_script *s, const char *avoid,
                               char *out, int outsz)
{
    static const char *special[] = {
        "start", "splashscreen", "main_menu", "after_load", "quit",
        "before_main_menu", "after_warp"
    };
    const int nspecial = (int)(sizeof(special) / sizeof(special[0]));
    int i, k, best = -1, bestrun = -1;

    if (!s || outsz <= 0)
        return 0;

    for (i = 0; i < s->nlabels; i++) {
        int j = s->labels[i].stmt, end = s->n, run = 0, skip = 0;

        if (avoid && !strcmp(s->labels[i].name, avoid))
            continue;
        for (k = 0; k < nspecial; k++)
            if (!strcmp(s->labels[i].name, special[k]))
                skip = 1;
        if (skip)
            continue;

        for (k = 0; k < s->nlabels; k++)
            if (s->labels[k].stmt > s->labels[i].stmt &&
                s->labels[k].stmt < end)
                end = s->labels[k].stmt;

        /* Every say in the label, not the run starting at it.
         * labels[i].stmt is the LABEL statement, so a run counted from
         * there is always zero -- which made this pick whichever
         * candidate came first in the table, and on the fixture that is a
         * two-statement label with no dialogue at all. (It is also why
         * anygame_story_label above needs its lookahead.) */
        for (; j < end; j++)
            if (s->st[j].kind == MR_ST_SAY)
                run++;

        if (run > bestrun) {
            bestrun = run;
            best = i;
        }
    }

    /* bestrun <= 0 means every other label is silent, which is no use to
     * a caller that wants to see the line on screen change. */
    if (best < 0 || bestrun <= 0 ||
        (int)strlen(s->labels[best].name) >= outsz)
        return 0;
    strcpy(out, s->labels[best].name);
    return 1;
}

/* Like anygame_story_label, but the run has to be *translated* in `lang`.
 *
 * A game can ship a tl/<lang>/ that this engine can load and still have no
 * translated dialogue in it -- strings only, or a translation that covers
 * chapter three and not chapter one. A test about the line on screen
 * changing when the language does then fails on a label it picked itself,
 * which is a fact about the game's translation progress and not about the
 * engine.
 *
 * So: count only says that actually have an entry, and report how many the
 * winner has (0 means there is nothing anywhere, and the caller should
 * skip rather than assert).
 *
 * Leaves the language where it found it -- NULL -- because callers switch
 * languages themselves and a side effect here reverted their choice once
 * already (mr_tl_sync follows _preferences.language; see NOTES).
 */
static int anygame_translated_label(const mr_script *s, const char *lang,
                                    char *out, int outsz)
{
    static const char *special[] = {
        "start", "splashscreen", "main_menu", "after_load", "quit",
        "before_main_menu", "after_warp", "credits"
    };
    const int nspecial = (int)(sizeof(special) / sizeof(special[0]));
    int best = -1, bestrun = 0;
    int i, k;

    if (!s || s->nlabels <= 0 || outsz <= 0 || !lang)
        return 0;

    mr_tl_set_language(lang);

    for (i = 0; i < s->nlabels; i++) {
        int j = s->labels[i].stmt;
        int end = s->n;
        int run = 0, skip = 0;

        for (k = 0; k < nspecial; k++)
            if (strcmp(s->labels[i].name, special[k]) == 0)
                skip = 1;
        if (skip)
            continue;

        for (k = 0; k < s->nlabels; k++)
            if (s->labels[k].stmt > s->labels[i].stmt &&
                s->labels[k].stmt < end)
                end = s->labels[k].stmt;

        for (; j < end; j++) {
            const mr_stmt *st = &s->st[j];
            if (st->kind != MR_ST_SAY || !st->tlid || !st->tlid[0])
                continue;
            if (mr_tl_dialogue(st->tlid, st->text) != st->text)
                run++;
        }

        if (run > bestrun) {
            bestrun = run;
            best = i;
        }
    }

    mr_tl_set_language(NULL);

    if (best < 0 || (int)strlen(s->labels[best].name) >= outsz)
        return 0;

    strcpy(out, s->labels[best].name);
    return bestrun;
}

#endif /* MR_ANYGAME_H */

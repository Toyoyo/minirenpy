/* tltest.c - does a say statement find its translation?
 *
 * The translation identifier is `<label>_<md5-of-the-reconstructed-line>`,
 * computed by the parser (tl.c, mr_tl_identifier, after Ren'Py's
 * ast.py:941) and matched against what the tl file says. Getting it wrong
 * is silent: the line simply stays in its original language.
 *
 * Which is why the ids this runs against are **Ren'Py's own**, not ours.
 * tools/harness/fixgame/script_game.rpy holds twenty say statements copied
 * verbatim from a real game, and tl/english/script_game.rpy holds the
 * twenty `translate english chap6_83afebf2:` blocks Ren'Py generated for
 * them. The parser re-derives those ids from the source lines here; if the
 * derivation drifts, nothing matches. A catalogue generated from our own
 * parser would agree with any drift and prove nothing, which is the trap
 * this fixture exists to avoid.
 *
 * The rest of the fixture's dialogue is invented and deliberately *not*
 * translated, so the miss path is exercised too: a lookup that finds
 * nothing has to hand back the original text, not an empty string.
 *
 * This used to walk a real game and assert complete coverage of it, which
 * made it a content audit -- red on one untranslated line, and red on a
 * different game entirely. That is a job for the game's own build, not for
 * the engine's suite.
 *
 * Build against the parser and the catalogue only -- no graphics, no audio:
 *
 *   gcc -O1 -g -Wall -I src -I tools/hostshim -o /tmp/tltest \
 *       tools/harness/tltest.c src/lex.c src/parse.c src/vfs.c src/expr.c \
 *       src/style.c src/slang.c src/slparse.c src/tl.c src/md5.c
 *   /tmp/tltest fixture.pak tools/harness/fixgame
 *
 * The archive is needed rather than the loose directory: mr_vfs_enum only
 * enumerates loose files on Windows, and the catalogue is found by enumeration.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "mr.h"
#include "script.h"
#include "vfs.h"
#include "expr.h"
#include "tl.h"

static int g_quiet = 1;

void mr_log(const char *fmt, ...)
{
    va_list ap;
    if (g_quiet)
        return;
    va_start(ap, fmt);
    fputs("  log: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void mr_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("FATAL: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static int host_call(const char *name, mr_value *argv, const char **argname,
                     int argc, mr_value *result)
{
    (void)argname;
    memset(result, 0, sizeof(*result));
    if (!strcmp(name, "_") && argc > 0) {
        *result = argv[0];
        return 1;
    }
    return 0;
}

static int fails;

static void check(const char *what, int ok)
{
    printf("%-54s %s\n", what, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

static void check_str(const char *orig, const char *want)
{
    const char *got = mr_tl_string(orig);
    int ok = !strcmp(got, want);
    printf("  %-34s -> %-26s %s\n", orig, got, ok ? "ok" : "WRONG");
    if (!ok)
        fails++;
}

/* A say statement is "borrowed" -- one of the real lines in
 * script_game.rpy -- if its identifier is under one of those two labels.
 * Everything else in the fixture is invented and has no translation. */
static int borrowed_id(const char *tlid)
{
    return tlid && (!strncmp(tlid, "chap0_", 6) || !strncmp(tlid, "chap6_", 6));
}

/* The statement whose source text is exactly `text`, or NULL. Looking a
 * line up by its text and then asserting on the *id* is the check that
 * matters: it says our parser derived what Ren'Py derived. */
static const mr_stmt *stmt_by_text(const mr_script *s, const char *text)
{
    int i;
    for (i = 0; i < s->n; i++)
        if (s->st[i].kind == MR_ST_SAY && s->st[i].text &&
            !strcmp(s->st[i].text, text))
            return &s->st[i];
    return NULL;
}

int main(int argc, char **argv)
{
    mr_script *s;
    int i, says = 0, hit = 0, noid = 0, captions = 0;
    int borrowed = 0, invented = 0, invented_wrong = 0;

    if (argc < 3) {
        fprintf(stderr, "usage: tltest <pak> <game-dir>\n");
        return 2;
    }
    if (argc > 3 && !strcmp(argv[3], "-v"))
        g_quiet = 0;

    if (!mr_vfs_init(argv[1], argv[2])) {
        fprintf(stderr, "could not mount %s\n", argv[1]);
        return 1;
    }
    mr_expr_init(host_call);

    s = mr_script_load(argv[2]);
    if (!s) {
        fprintf(stderr, "script load failed\n");
        return 1;
    }

    mr_tl_set_language("english");
    if (!mr_tl_language()) {
        printf("FAIL: no english catalogue loaded\n");
        return 1;
    }

    for (i = 0; i < s->n; i++) {
        const mr_stmt *st = &s->st[i];
        const char *tr;
        int is_borrowed;

        /* A menu's caption is a say in Ren'Py and is translated as one, but
         * it rides on the MENU statement here -- so it has to be counted
         * explicitly or it is simply never looked at. It was not, which is
         * how a caption's identifier could be wrong without this noticing. */
        if (st->kind == MR_ST_MENU && st->text)
            captions++;
        else if (st->kind != MR_ST_SAY)
            continue;

        says++;
        if (!st->tlid || !st->tlid[0]) {
            noid++;
            continue;
        }
        is_borrowed = borrowed_id(st->tlid);
        tr = mr_tl_dialogue(st->tlid, st->text);

        if (is_borrowed) {
            borrowed++;
            if (tr != st->text)
                hit++;
            else
                printf("  miss: %s:%d %s\n    %.70s\n",
                       s->files[st->file], st->line, st->tlid, st->text);
        } else {
            invented++;
            /* The miss path: no entry, so the original comes back -- the
             * same pointer, and never an empty string. */
            if (tr != st->text || !tr || !tr[0])
                invented_wrong++;
        }
    }

    printf("\n=== dialogue ===\n");
    printf("  say statements  %d\n", says);
    printf("  of those, menu captions %d\n", captions);
    printf("  borrowed, with a real Ren'Py id  %d\n", borrowed);
    printf("  of those, translated             %d\n", hit);
    printf("  invented, with no translation    %d\n", invented);
    printf("  without an id   %d\n", noid);

    /* script_game.rpy has twenty of them and tl/english/script_game.rpy has
     * twenty blocks. Stated as a number so that losing the file, or losing
     * the labels out of the parser's file list, fails here rather than
     * passing with nothing to check. */
    check("the borrowed lines are all there", borrowed == 20);
    check("  and every one finds its translation", borrowed == hit);
    check("the invented lines are there too", invented > 20);
    check("  and every one falls through unchanged", invented_wrong == 0);
    check("no say statement went without an id", noid == 0);

    /* The identifier itself, which is the thing that cannot be checked by
     * generating the catalogue: this id came out of Ren'Py. */
    {
        const mr_stmt *st = stmt_by_text(s, "Puis une autre.");
        check("a borrowed line is in the script", st != NULL);
        if (st) {
            printf("   (\"Puis une autre.\" -> %s)\n", st->tlid);
            check("  and the parser derives Ren'Py's own identifier for it",
                  st->tlid && !strcmp(st->tlid, "chap6_83afebf2"));
            check("  and that identifier reaches the translation",
                  !strcmp(mr_tl_dialogue("chap6_83afebf2", st->text),
                          "Then another."));
        }
        /* An escaped quote inside the line: it is part of what is digested,
         * so this one is a check on encode_say_string as much as on md5. */
        st = stmt_by_text(s, "\"Merci maman.\"");
        check("a line with quotes in it derives its id too",
              st && st->tlid && !strcmp(st->tlid, "chap6_5849b79b"));
    }

    /* --- strings ---
     *
     * The fixture is a French-written game (tl/None/fixture.rpym says why),
     * so the interesting direction is the default one: no language selected
     * and Ren'Py's own English strings coming out French.
     */
    printf("\n=== strings, with no language set ===\n");
    mr_tl_set_language(NULL);
    check_str("Back", "Retour");
    check_str("Are you sure you want to quit?", "Voulez-vous vraiment quitter ?");
    /* Not in any catalogue: must come back unchanged, never empty. */
    check_str("this is in no catalogue", "this is in no catalogue");

    /* {#...} disambiguation tags. The catalogue carries them, so the exact
     * match answers; the stripped retry only matters when it does not. */
    check_str("{#weekday}Monday", "{#weekday}Lundi");
    check_str("{#weekday_short}Mon", "{#weekday_short}Lun");
    /* Tagged in the source, untagged in the catalogue: Ren'Py asks a second
     * time with the tags removed rather than giving up. */
    check_str("{#somewhere}Back", "Retour");
    check_str("{#a}Ba{#b}ck", "Retour");
    /* An unterminated tag is not a tag, so nothing is stripped. */
    check_str("{#Back", "{#Back");

    printf("\n=== strings, in english ===\n");
    mr_tl_set_language("english");
    check_str("Quitter le jeu de rythme ?", "Quit the rhythm game?");
    check_str("Nouveau record !", "New record!");
    /* An English key under the English catalogue has no entry, and the
     * answer is the key -- which is already English. That fall-through is
     * what makes switching language work at all for Ren'Py's own strings. */
    check_str("Back", "Back");

    printf("\n=== switching back ===\n");
    mr_tl_set_language(NULL);
    check_str("Back", "Retour");
    mr_tl_set_language("english");
    check_str("Back", "Back");

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    mr_tl_shutdown();
    mr_script_free(s);
    mr_expr_shutdown();
    mr_vfs_shutdown();
    return fails ? 1 : 0;
}

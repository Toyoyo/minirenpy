/* Dates are formatted with names from the catalogue, not from the locale.
 *
 * Ren'Py's _strftime (00action_file.rpy:77) intercepts %a %A %b %B and takes
 * the names from the translation catalogue, passing the rest to strftime.
 * The reason is not politeness to translators: `strftime("%B")` returns the
 * C locale's English whatever language the game is in, so a French game
 * would read "Saturday September" -- worse than the numbers we used to print.
 *
 * Takes the pak and the game directory, because the whole point is the
 * catalogue: without it mr_tl_string returns its input and every assertion
 * below would be about the English fall-back.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "mr.h"
#include "vfs.h"
#include "tl.h"
#include "expr.h"
#include "datefmt.h"

void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); }
void mr_log(const char *f, ...) { (void)f; }

static int fails;

/* Is a catalogue in force that actually renames the months?
 *
 * The test needs this and not merely "a catalogue is loaded". When the key
 * misses, mr_tl_string hands back the key, catname strips its tag and
 * returns "September" -- and datefmt.c's own English table says
 * "September" too, so every comparison below would pass without the
 * catalogue being consulted at all. Non-vacuity rests on the two differing.
 */
static int months_are_renamed(void)
{
    return strcmp(mr_tl_string("{#month}September"),
                  "{#month}September") != 0;
}

/* Put the engine in a language whose date names differ from English, and
 * say which. 0 if this game has none.
 *
 * Candidates in order: the language the script is written in (NULL, which
 * reads tl/None/ -- Ren'Py's slot for strings in the *original* language),
 * then each tl/<lang>/ the archive carries. The first that renames the
 * months wins.
 *
 * Ordered that way, and chosen by result rather than by name, because the
 * two games on hand want opposite answers. Miwa is written in French and
 * ships tl/english: its French month names are in tl/None, so NULL is the
 * translating choice and "english" is an identity. Hinata is written in
 * English and ships tl/french: NULL gives English and "french" is the one
 * that renames. A test that named either language would be testing the
 * author's habits.
 */
static int pick_translating_language(char *out, int outsz)
{
    char path[256];
    int i;

    mr_tl_set_language(NULL);
    if (months_are_renamed()) {
        if (outsz > 0)
            strcpy(out, "(the script's own)");
        return 1;
    }

    /* tl/<lang>/... -- mr_vfs_enum matches a prefix, not a directory, so
     * the language is the span up to the next '/'. */
    for (i = 0; mr_vfs_enum("tl/", NULL, i, path, sizeof(path)); i++) {
        char lang[64];
        char *slash = strchr(path + 3, '/');
        int n;

        if (!slash)
            continue;
        n = (int)(slash - (path + 3));
        if (n <= 0 || n >= (int)sizeof(lang))
            continue;
        memcpy(lang, path + 3, (size_t)n);
        lang[n] = '\0';

        mr_tl_set_language(lang);
        if (months_are_renamed()) {
            if (n < outsz) {
                strcpy(out, lang);
                return 1;
            }
        }
    }

    mr_tl_set_language(NULL);
    if (outsz > 0)
        out[0] = '\0';
    return 0;
}

/* The catalogue's own answer for a date name, as datefmt.c emits it.
 *
 * Two steps, and the second is the one that is easy to miss: a catalogue
 * translates the whole key including its disambiguation tag, so
 * catname("{#month}September") comes back as "{#month}Septembre", and
 * datefmt.c drops the tag before emitting (past_tag, datefmt.c:52). A test
 * comparing against the raw catalogue answer fails on every name while the
 * output is perfectly correct -- which is exactly what it did. */
static const char *catname(const char *key)
{
    const char *s = mr_tl_string(key);

    if (s && s[0] == '{' && s[1] == '#') {
        const char *e = strchr(s, '}');
        if (e)
            return e + 1;
    }
    return s;
}

static void check(const char *label, int ok)
{
    printf("%-56s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

/* A fixed moment, built through mktime so it is that local time whatever the
 * machine's zone: Friday 11 September 2026, 16:09. A *known weekday* is the
 * point -- the Monday/Sunday numbering differs between Python's tm_wday and
 * C's, and getting it wrong shifts every date by one day. */
static long moment(int year, int mon, int mday, int hour, int min)
{
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = year - 1900;
    t.tm_mon  = mon - 1;
    t.tm_mday = mday;
    t.tm_hour = hour;
    t.tm_min  = min;
    t.tm_isdst = -1;
    return (long)mktime(&t);
}

static const char *fmt(const char *f, long when)
{
    static char buf[128];
    mr_format_time(buf, sizeof(buf), f, when);
    return buf;
}

int main(int argc, char **argv)
{
    long fri, sun;
    char lang[64];
    int  skipped_catalogue = 0;

    setbuf(stdout, NULL);
    if (argc < 3) {
        fprintf(stderr, "usage: datetest <pak> <game-dir>\n");
        return 2;
    }
    mr_vfs_init(argv[1], argv[2]);
    mr_tl_sync();

    /* A catalogue that has something to say, or nothing below means
     * anything.
     *
     * Not simply "is {#month}September translated with no language set",
     * which is what this used to be. That holds on a game written in
     * French -- Miwa's tl/None/ carries French for Ren'Py's own strings --
     * and is false on a game written in English, where the key correctly
     * misses and the English table in datefmt.c is the right answer. The
     * guard failed on Hinata while every assertion under it passed.
     *
     * So: find a language the game actually ships and switch to it. That
     * makes the catalogue live on any game, and incidentally makes the
     * checks below stronger on an English-native one, where an engine that
     * ignored the catalogue entirely would otherwise agree with it.
     *
     * mr_tl_string and not catname here: a key that missed comes back as
     * itself, tag and all, and catname would strip the tag and make the
     * comparison differ whatever happened -- a guard that always passes,
     * in front of the assertions it guards. */
    /* pick_translating_language uses mr_tl_set_language and deliberately no
     * mr_tl_sync afterwards: sync follows _preferences.language and would
     * throw the choice away, reinstating whatever the persistent file in
     * /tmp was left holding by the last harness to run there. */
    if (pick_translating_language(lang, sizeof(lang))) {
        printf("   language: %s\n", lang);
        check("the catalogue renames the months", months_are_renamed());
    } else {
        /* Not a failure. A game with no catalogue that touches the date
         * names is not misbehaving, and datefmt.c's English table is the
         * right answer for it -- there is simply nothing here to tell apart
         * from that table, so the checks below cannot be made non-vacuous.
         * Printed rather than silently passed, because a skip nobody sees
         * is how a suite reports green on a test that stopped testing. */
        printf("   SKIP: this game has no catalogue that renames the "
               "months, so the\n"
               "         comparisons below cannot distinguish the "
               "catalogue from\n"
               "         datefmt.c's own English table.\n");
        /* The numeric and edge-case checks below are still worth running --
         * they are about the formatter, not the catalogue -- so this does
         * not return here. But the exit code has to say the catalogue half
         * was not exercised, or a game with no catalogue reports a full
         * pass for half a test. run.sh counts 77 as a skip. */
        skipped_catalogue = 1;
    }

    fri = moment(2026, 9, 11, 16, 9);    /* a Friday */
    sun = moment(2026, 2,  1,  9, 5);    /* a Sunday, to pin the wrap-around */

    printf("\n-- the names come from the catalogue --\n");
    printf("   %s\n", fmt("%A %d %B %Y, %H:%M", fri));

    /* Asked of the catalogue, not written down.
     *
     * This used to assert "Vendredi" and "Septembre", which is what Miwa
     * returns -- that game is written in French, so its tl/None/ carries
     * French for Ren'Py's own strings and an unset language already yields
     * them. Hinata is the other way round: English base, a french/
     * catalogue, and tl/None/ in English. The seven assertions below all
     * failed on it, and none of them had found a bug.
     *
     * What the engine has to get right is that %A and %B come from the
     * catalogue rather than from strftime -- `strftime("%B")` returns the C
     * locale's English whatever language the game is in. So compare against
     * the catalogue's own answer for the same key. That holds in any
     * language, still fails if datefmt.c looks up the wrong key, and still
     * fails if it bypasses the catalogue, because the C locale and the
     * catalogue only agree when the game is English -- which is why the
     * English case is checked separately below. */
    check("the weekday is translated",
          !strcmp(fmt("%A", fri), catname("{#weekday}Friday")));
    check("the month is translated",
          !strcmp(fmt("%B", fri), catname("{#month}September")));
    check("  and the short forms too",
          !strcmp(fmt("%a", fri), catname("{#weekday_short}Fri")) &&
          strlen(fmt("%b", fri)) > 0);

    /* Sunday is the one that catches the numbering: Python counts weekdays
     * from Monday and C from Sunday, so an unconverted index turns Sunday
     * into Monday and every other day by one. Compared against the
     * catalogue's Sunday *and* asserted different from its Monday, since in
     * a game whose catalogue happened to leave both untranslated the first
     * check alone would pass on an off-by-one. */
    printf("   %s\n", fmt("%A %d %B %Y", sun));
    check("Sunday is Sunday, not Monday",
          !strcmp(fmt("%A", sun), catname("{#weekday}Sunday")) &&
          strcmp(catname("{#weekday}Sunday"),
                 catname("{#weekday}Monday")) != 0);

    printf("\n-- the tag is dropped, not printed --\n");
    check("no {#...} survives in the output",
          strchr(fmt("{#file_time}%A %d %B %Y, %H:%M", fri), '{') == NULL);
    check("  and the name carries none either",
          strchr(fmt("%A", fri), '{') == NULL);

    printf("\n-- the numbers --\n");
    check("%d is the day, zero padded",   !strcmp(fmt("%d", fri), "11"));
    check("%m is the month number",       !strcmp(fmt("%m", fri), "09"));
    check("%Y is the year",               !strcmp(fmt("%Y", fri), "2026"));
    check("%y is two digits",             !strcmp(fmt("%y", fri), "26"));
    check("%H:%M is the time",            !strcmp(fmt("%H:%M", fri), "16:09"));
    check("%I is the 12-hour clock",      !strcmp(fmt("%I %p", fri), "04 PM"));
    check("%% is a literal percent",      !strcmp(fmt("100%%", fri), "100%"));

    printf("\n-- whole formats, as the screens write them --\n");
    {
        /* screens.rpy:657 and its English translation, which reorders the
         * date and adds a comma -- the format string is translatable, so
         * this is two different patterns, not one. */
        /* Built from the catalogue's own names rather than spelled out, for
         * the reason given above the weekday checks: the expected text is
         * the game's language, and the two games on hand are in different
         * ones. The *shape* is what matters here -- which fields appear, in
         * what order, with what punctuation -- and that is the same
         * assertion either way. */
        const char *fr = fmt("{#file_time}%A %d %B %Y, %H:%M", fri);
        const char *en_pattern = "{#file_time}%A, %B %d %Y, %H:%M";
        char en[128], want[256];

        sprintf(want, "%s 11 %s 2026, 16:09",
                catname("{#weekday}Friday"),
                catname("{#month}September"));
        printf("   native: %s\n", fr);
        check("the native format reads as Ren'Py renders it",
              !strcmp(fr, want));

        mr_format_time(en, sizeof(en), en_pattern, fri);
        sprintf(want, "%s, %s 11 2026, 16:09",
                catname("{#weekday}Friday"),
                catname("{#month}September"));
        printf("   other pattern, same names: %s\n", en);
        check("  a different *pattern* reorders day and month",
              !strcmp(en, want));
    }

    printf("\n-- edges --\n");
    check("an unknown specifier is left visible",
          strchr(fmt("%Q", fri), 'Q') != NULL);
    {
        char tiny[8];
        check("a trailing %% does not run off the end",
              mr_format_time(tiny, sizeof(tiny), "%", fri) >= 0);
    }
    {
        /* Truncation must stay inside the buffer and stay terminated. */
        char small[8];
        int n = mr_format_time(small, sizeof(small),
                               "%A %d %B %Y, %H:%M", fri);
        check("a short buffer is truncated, not overrun",
              n < (int)sizeof(small) && small[sizeof(small) - 1] == '\0' &&
              strlen(small) == (size_t)n);
    }

    printf(fails ? "\n%d FAILED\n" : "\nall good\n", fails);
    mr_tl_shutdown();
    mr_vfs_shutdown();
    if (fails)
        return 1;
    return skipped_catalogue ? 77 : 0;
}

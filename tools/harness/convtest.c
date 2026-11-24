/* The `!t` conversion in a substitution: `text "[title!t]"`.
 *
 * Text is translated as a *template*, before interpolation -- that is
 * Ren'Py's order (substitutions.substitute) and ours (slrender.c, which runs
 * mr_tl_string over the raw string and then mr_expr_substitute over the
 * result). So a screen that says `text "[title]"` looks up the literal
 * `[title]` in the string catalogue and the variable's own contents are never
 * a key.
 *
 * That is why a chapter title assigned under one language stays in that
 * language when the player switches:
 *
 *     $ title = "Avril - Nouvelle annee"
 *     if _preferences.language == "english":
 *         $ title = "April - New year"
 *
 * Nothing re-runs those lines, and a save carries the string they computed --
 * so a French slot loaded in English draws a French card. Reported from play.
 * Ren'Py behaves identically and the way out it provides is the `t`
 * conversion, which translates the *substituted value*:
 *
 *     if "t" in conv:
 *         value = renpy.translation.translate_string(value)
 *                                     -- substitutions.py:282-283
 *
 * We parsed the flag and threw it away. Now `t` is implemented, and the rest
 * (r, s, i, q, u, l, c) are still ignored -- which is asserted here too, so
 * that the divergence is written down rather than assumed.
 *
 * Takes no arguments and reads no game. The string catalogue has to come
 * through mr_vfs_enum, which only enumerates an archive on this host, so the
 * test writes a one-entry MRPK of its own and mounts it. That also makes it
 * independent of which game is installed, which a translation test otherwise
 * is not -- tltest is the cautionary example.
 *
 * Run from a scratch directory: it writes and removes CONVTEST.PAK.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "mr.h"
#include "vfs.h"
#include "expr.h"
#include "tl.h"

void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); exit(1); }
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

static int fails;

static void checks(const char *what, const char *got, const char *want)
{
    int ok = !strcmp(got, want);
    printf("%-50s %s\n   got  '%s'\n   want '%s'\n", what,
           ok ? "ok" : "** FAIL **", got, want);
    if (!ok)
        fails++;
}

/* The catalogue, as a .rpy under tl/english/. The `old` is the text a `$`
 * line would have assigned in the script's own language. */
static const char CATALOGUE[] =
    "# A harness catalogue, not a game's.\n"
    "\n"
    "translate english strings:\n"
    "\n"
    "    # Avril - Nouvelle annee\n"
    "    old \"Avril - Nouvelle annee\"\n"
    "    new \"April - New year\"\n"
    "\n"
    "    old \"Court\"\n"
    "    new \"Short\"\n";

#define PAK_NAME "CONVTEST.PAK"
#define NAMELEN  256          /* MRPK v2: MR_PATH_MAX, and the entry is read
                               * as one fread into a struct array, so the
                               * width is part of the format */
#define DATA_AT  2048         /* what mkpak.py aligns to */

static void put32(FILE *f, unsigned long v)
{
    fputc((int)(v & 0xFF), f);
    fputc((int)((v >> 8) & 0xFF), f);
    fputc((int)((v >> 16) & 0xFF), f);
    fputc((int)((v >> 24) & 0xFF), f);
}

/* One file, so the directory is sorted whatever name_cmp does. */
static int write_pak(void)
{
    FILE *f = fopen(PAK_NAME, "wb");
    char name[NAMELEN];
    long size = (long)sizeof(CATALOGUE) - 1;
    int i;

    if (!f)
        return 0;

    put32(f, 0x4B50524DUL);          /* 'MRPK' */
    put32(f, 2);                     /* version */
    put32(f, 1);                     /* entries */
    put32(f, (unsigned long)(DATA_AT + size));   /* directory offset */

    for (i = 16; i < DATA_AT; i++)
        fputc(0, f);
    fwrite(CATALOGUE, 1, (size_t)size, f);

    memset(name, 0, sizeof(name));
    strcpy(name, "tl/english/strings.rpy");
    fwrite(name, 1, sizeof(name), f);
    put32(f, DATA_AT);
    put32(f, (unsigned long)size);

    fclose(f);
    return 1;
}

/* `src` with its substitutions applied, as a screen's text statement or a
 * say would get it. */
static const char *subst(const char *src)
{
    static char out[512];

    mr_expr_substitute(src, out, (int)sizeof(out));
    return out;
}

static void set(const char *name, const char *value)
{
    mr_value v;

    mr_value_set_str(&v, value);
    mr_var_set(name, &v);
}

int main(void)
{
    remove(PAK_NAME);
    if (!write_pak()) {
        printf("could not write %s\n", PAK_NAME);
        return 1;
    }
    if (!mr_vfs_init(PAK_NAME, NULL)) {
        printf("could not mount %s\n", PAK_NAME);
        return 1;
    }

    set("title", "Avril - Nouvelle annee");

    /* The script's own language first. There is no tl/None/ in the archive,
     * so every lookup misses and `!t` has to be a no-op rather than an
     * error -- which is the state a single-language game is always in. */
    printf("-- the original language --\n");
    mr_tl_set_language(NULL);
    checks("plain, untranslated", subst("[title]"),
           "Avril - Nouvelle annee");
    checks("and !t finds nothing to do", subst("[title!t]"),
           "Avril - Nouvelle annee");

    printf("\n-- english --\n");
    mr_tl_set_language("english");
    checks("!t translates the substituted value", subst("[title!t]"),
           "April - New year");

    /* The quirk this was added for, pinned as behaviour rather than left to
     * be rediscovered: without the flag the value is not a key, because the
     * text around it was translated before the interpolation happened. This
     * matches Ren'Py and is the reason `!t` exists at all. */
    checks("without it the value is still not a key", subst("[title]"),
           "Avril - Nouvelle annee");

    printf("\n-- around other text --\n");
    checks("the rest of the string is untouched",
           subst("Chapitre: [title!t] (fin)"),
           "Chapitre: April - New year (fin)");
    checks("two substitutions, one converted",
           subst("[title] / [title!t]"),
           "Avril - Nouvelle annee / April - New year");

    printf("\n-- the flags we do not implement --\n");
    /* Ren'Py would uppercase this one and leave it translated; we apply the
     * `t` and ignore the `u`. Asserted so that implementing `u` later shows
     * up here as a deliberate change rather than as a surprise. */
    checks("t still applies inside a set of flags", subst("[title!tu]"),
           "April - New year");
    checks("a flag that is not t does not translate", subst("[title!s]"),
           "Avril - Nouvelle annee");
    checks("and no flag at all is unchanged", subst("[title!]"),
           "Avril - Nouvelle annee");

    printf("\n-- values that are not the catalogue's --\n");
    set("title", "Court");
    checks("a second entry, to show the lookup is by value",
           subst("[title!t]"), "Short");
    set("title", "Jamais traduit");
    checks("a miss comes back as itself", subst("[title!t]"),
           "Jamais traduit");

    {
        mr_value v;
        v.type = MR_V_INT;
        v.i = 3;
        v.s[0] = '\0';
        v.f = 0.0;
        mr_var_set("num", &v);
    }
    checks("a number with !t is its own text", subst("[num!t]"), "3");
    checks("a name that does not exist stays empty",
           subst("[nosuchvar!t]"), "");

    mr_tl_shutdown();
    mr_vfs_shutdown();
    remove(PAK_NAME);

    printf("\n%s\n", fails ? "** FAILURES **" : "all ok");
    return fails ? 1 : 0;
}

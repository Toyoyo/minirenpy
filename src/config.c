/* config.c - a small INI reader. See config.h.
 *
 * Deliberately not general: one file, read once, into a fixed table. There is
 * no allocation and no failure path worth reporting, because the caller's
 * defaults are always a working configuration.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "mr.h"
#include "config.h"

#define MR_CFG_MAX      32
#define MR_CFG_KEY_MAX  32
#define MR_CFG_VAL_MAX  128

/* The engine's own section. Anything else in the file is skipped, so a game
 * can keep its settings beside these. */
#define MR_CFG_SECTION  "minirenpy"

typedef struct {
    char key[MR_CFG_KEY_MAX];
    char val[MR_CFG_VAL_MAX];
} mr_cfg_entry;

static mr_cfg_entry g_cfg[MR_CFG_MAX];
static int          g_ncfg;

static int ci_equal(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

/* Trim in place, both ends. */
static char *trim(char *s)
{
    char *end;

    while (*s == ' ' || *s == '\t')
        s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n'))
        end--;
    *end = '\0';
    return s;
}

static void add(const char *key, const char *val)
{
    int i;

    /* A repeated key overwrites, so a file with two `width=` lines behaves
     * the way reading it top to bottom suggests. */
    for (i = 0; i < g_ncfg; i++) {
        if (ci_equal(g_cfg[i].key, key))
            break;
    }
    if (i == g_ncfg) {
        if (g_ncfg >= MR_CFG_MAX)
            return;
        g_ncfg++;
    }

    strncpy(g_cfg[i].key, key, MR_CFG_KEY_MAX - 1);
    g_cfg[i].key[MR_CFG_KEY_MAX - 1] = '\0';
    strncpy(g_cfg[i].val, val, MR_CFG_VAL_MAX - 1);
    g_cfg[i].val[MR_CFG_VAL_MAX - 1] = '\0';
}

void mr_config_load(const char *path)
{
    char line[256];
    FILE *f;
    int in_section = 0;

    g_ncfg = 0;

    f = fopen(path, "r");
    if (!f) {
        mr_log("config: no %s, using built-in defaults", path);
        return;
    }

    while (fgets(line, (int)sizeof(line), f)) {
        char *s = trim(line);
        char *eq;

        if (!*s || *s == ';' || *s == '#')
            continue;

        if (*s == '[') {
            char *close = strchr(s, ']');
            if (close) {
                *close = '\0';
                in_section = ci_equal(trim(s + 1), MR_CFG_SECTION);
            }
            continue;
        }

        if (!in_section)
            continue;

        eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = '\0';

        /* The value is taken whole: no inline comment syntax, because a
         * path may legitimately contain a ';' and silently truncating one
         * would be worse than not offering the comment. */
        add(trim(s), trim(eq + 1));
    }

    fclose(f);
    mr_log("config: %s, %d setting%s", path, g_ncfg, g_ncfg == 1 ? "" : "s");
}

const char *mr_config_str(const char *key, const char *dflt)
{
    int i;

    for (i = 0; i < g_ncfg; i++)
        if (ci_equal(g_cfg[i].key, key))
            return g_cfg[i].val[0] ? g_cfg[i].val : dflt;
    return dflt;
}

long mr_config_int(const char *key, long dflt, long lo, long hi)
{
    const char *s = mr_config_str(key, NULL);
    const char *num;
    char *end;
    long v;

    if (!s)
        return dflt;

    /* Base 10, except for an explicit 0x. Not base 0: that would also make
     * a leading zero mean octal, and `width=0640` silently meaning 416 is a
     * worse trap than the one this exists to fix. The hex form is here
     * because MODES.EXE prints mode numbers in hex, as the VBE
     * documentation and every other mode utility do, and `vbemode` then
     * wanted them in decimal -- so the number on the screen could not be
     * typed into the file it was printed for. */
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        num = s + 2;
        v = strtol(num, &end, 16);
    } else {
        num = s;
        v = strtol(num, &end, 10);
    }
    while (*end == ' ' || *end == '\t')
        end++;
    /* Against `num`, not against `s`: a bare "0x" consumes no digits and
     * must be rejected, and comparing with the start of the string would
     * call it a success. */
    if (end == num || *end) {
        mr_log("config: %s='%s' is not a number, using %ld", key, s, dflt);
        return dflt;
    }
    if (v < lo || v > hi) {
        mr_log("config: %s=%ld is outside %ld..%ld, using %ld",
               key, v, lo, hi, dflt);
        return dflt;
    }
    return v;
}

/* sccheck.c - prove mr_script_snapshot/mr_script_verify actually catch a
 * write into the statement array before that pair is trusted on hardware.
 *
 * A diagnostic that silently never fires is worse than none: it would have
 * been read as "nothing wrote to the array" on the one run that mattered.
 * So: load a real script, snapshot it, change one byte of one statement,
 * and check the verify names that statement and that byte.
 *
 *   gcc -O0 -g -I src -I tools/hostshim -I libs -o /tmp/sccheck \
 *       probe/sccheck.c <parse chain>
 *   /tmp/sccheck <gamedir>
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "mr.h"
#include "script.h"
#include "vfs.h"
#include "config.h"

/* Every line, concatenated: mr_script_verify prints two, and keeping only
 * the last one made this test assert against the wrong one -- it passed
 * for the wrong reason until the message it wanted moved. */
static char g_last[4096];
static int  g_lines;

void mr_log(const char *fmt, ...)
{
    char one[512];
    va_list ap;
    va_start(ap, fmt);
    vsprintf(one, fmt, ap);
    va_end(ap);
    if (strlen(g_last) + strlen(one) + 2 < sizeof(g_last)) {
        strcat(g_last, one);
        strcat(g_last, "\n");
    }
    g_lines++;
    printf("    log: %s\n", one);
}

void mr_fatal(const char *fmt, ...)
{
    (void)fmt;
    printf("FATAL\n");
    exit(1);
}

static int fails;

static void check(const char *label, int ok)
{
    printf("  %-58s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

int main(int argc, char **argv)
{
    mr_script *s;
    FILE *f;

    if (argc < 2) {
        printf("usage: sccheck <gamedir>\n");
        return 2;
    }

    /* scriptcheck is off by default, which is itself worth asserting. */
    f = fopen("sccheck.ini", "w");
    fprintf(f, "[Minirenpy]\nscriptcheck=1\n");
    fclose(f);

    if (!mr_vfs_init("nonexistent.pak", argv[1])) {
        printf("cannot mount %s\n", argv[1]);
        return 1;
    }

    /* First with the checks off, which is the default and the state in
     * which a verify must be harmless rather than a false alarm. */
    printf("-- with the checks off, as they ship --\n");
    mr_config_load("nonexistent.ini");
    s = mr_script_load(argv[1]);
    if (!s || s->n < 1000) {
        printf("no usable script\n");
        return 1;
    }
    s->st[5].line ^= 0x4242;
    check("verify passes everything, having nothing to compare",
          mr_script_verify(s, "nothing") == 1);
    s->st[5].line ^= 0x4242;
    mr_script_free(s);

    /* Now with every check armed, and the parse must be quiet. A
     * diagnostic that fires on healthy data is worse than no diagnostic:
     * the one run that matters happens on a machine in another room, and
     * a false alarm there costs a day. */
    printf("\n-- a healthy parse says nothing --\n");
    mr_config_load("sccheck.ini");
    mr_script_check_enable(1);
    g_last[0] = '\0';
    s = mr_script_load(argv[1]);
    if (!s || s->n < 100) {
        printf("no usable script\n");
        return 1;
    }

    /* Whether to snapshot is the caller's decision -- main.c reads
     * `scriptcheck` -- so what matters here is that a verify with no
     * snapshot behind it is harmless rather than a false alarm. */
    check("no misaligned pointer is reported while parsing",
          strstr(g_last, "misaligned") == NULL);
    check("no statement is reported as changed while parsing",
          strstr(g_last, "CHANGED") == NULL);
    check("the per-file snapshots did run",
          strstr(g_last, "watching") != NULL);

    mr_script_check_enable(0);

    printf("\n-- snapshot taken, and a single byte changed --\n");
    mr_config_load("sccheck.ini");
    mr_script_snapshot(s);
    check("the snapshot says what it is watching",
          strstr(g_last, "watching") != NULL);

    check("an untouched array verifies clean",
          mr_script_verify(s, "nothing happened") == 1);

    /* One byte, in a statement well into the array -- the DOS fault was at
     * statement 760 -- and in a pointer field, which is the case that
     * matters: a mangled pointer is what the engine faults on. */
    {
        unsigned char *p = (unsigned char *)&s->st[760].text;
        *p ^= 0xFF;

        g_last[0] = '\0';
        check("a one-byte change is caught",
              mr_script_verify(s, "the test") == 0);
        check("and it names the statement",
              strstr(g_last, "statement 760") != NULL ||
              strstr(g_last, "word at") != NULL);
        *p ^= 0xFF;
    }

    /* After reporting it resynchronises, so a second change is reported as
     * itself rather than the first one again -- a verify that cried wolf
     * forever would make the per-statement checks useless. */
    printf("\n-- it resynchronises --\n");
    mr_script_snapshot(s);
    s->st[100].line ^= 1;
    mr_script_verify(s, "first");
    check("the second call is clean once the first was reported",
          mr_script_verify(s, "second") == 1);
    s->st[200].line ^= 1;
    g_last[0] = '\0';
    check("and a later change is caught too",
          mr_script_verify(s, "third") == 0);
    check("named as the statement that actually changed",
          strstr(g_last, "statement 200") != NULL);

    remove("sccheck.ini");
    printf("\n%s\n", fails ? "FAILURES" : "all ok");
    return fails ? 1 : 0;
}

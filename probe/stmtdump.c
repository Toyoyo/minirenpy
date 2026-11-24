/* stmtdump.c - load a game's script and print statements around an index,
 * plus a sanity sweep over every statement's text pointer.
 *
 * Diagnostic only, for the FreeDOS 1.4 page fault: the crash is inside
 * mr_expr_exec's first read of its argument, during mr_interp_init's init
 * pass, and ESI at the fault held 0x24f. If that is the loop counter, this
 * says what statement 591 is.
 *
 *   gcc -g -std=c89 -Isrc -o /tmp/stmtdump probe/stmtdump.c <parse chain>
 *   ./stmtdump <gamedir|pak> [index]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mr.h"
#include "script.h"
#include "vfs.h"

void mr_log(const char *f, ...)
{
    (void)f;
}

void mr_fatal(const char *f, ...)
{
    (void)f;
    printf("FATAL\n");
    exit(1);
}

static const char *kindname(int k)
{
    switch (k) {
    case MR_ST_INIT:    return "INIT";
    case MR_ST_IMAGE:   return "IMAGE";
    case MR_ST_PYTHON:  return "PYTHON";
    case MR_ST_SAY:     return "SAY";
    case MR_ST_LABEL:   return "LABEL";
    default:            return "other";
    }
}

int main(int argc, char **argv)
{
    mr_script *s;
    int at = argc > 2 ? atoi(argv[2]) : 591;
    int i, lo, hi, bad = 0, ninit = 0;

    if (argc < 2) {
        printf("usage: stmtdump <gamedir> [index]\n");
        return 2;
    }

    if (!mr_vfs_init("nonexistent.pak", argv[1])) {
        printf("cannot mount %s\n", argv[1]);
        return 1;
    }

    s = mr_script_load(argv[1]);
    if (!s) {
        printf("no script\n");
        return 1;
    }
    printf("%d statements\n\n", s->n);

    lo = at - 6; if (lo < 0) lo = 0;
    hi = at + 6; if (hi > s->n) hi = s->n;
    for (i = lo; i < hi; i++)
        printf("%s%5d  %-7s file%d:%d  text=%p %s%.70s%s\n",
               i == at ? "->" : "  ", i, kindname(s->st[i].kind),
               s->st[i].file, s->st[i].line,
               (void *)s->st[i].text,
               s->st[i].text ? "'" : "", s->st[i].text ? s->st[i].text : "NULL",
               s->st[i].text ? "'" : "");

    /* Every statement the init pass will hand to mr_expr_exec, which does
     * not check for NULL. */
    for (i = 0; i < s->n; i++) {
        if (s->st[i].kind != MR_ST_INIT)
            continue;
        ninit++;
        if (!s->st[i].text) {
            printf("\nINIT %d (file%d:%d) has a NULL text\n", i,
                   s->st[i].file, s->st[i].line);
            bad++;
        }
    }
    printf("\n%d INIT statements, %d with no text\n", ninit, bad);

    /* The FreeDOS fault was mr_image_define_anim reading a garbage
     * st->anim. How often is that branch even taken here? */
    {
        int nimage = 0, nanim = 0;
        for (i = 0; i < s->n; i++) {
            if (s->st[i].kind != MR_ST_IMAGE)
                continue;
            nimage++;
            if (s->st[i].anim) {
                nanim++;
                if (nanim <= 3)
                    printf("  IMAGE %d '%s' has anim=%p\n", i, s->st[i].name,
                           (void *)s->st[i].anim);
            }
        }
        printf("%d IMAGE statements, %d of them animated\n", nimage, nanim);
    }

    /* The first few, which are what the crash log's last lines cover. */
    printf("\nthe first five INIT statements, in execution order:\n");
    {
        int shown = 0;
        for (i = 0; i < s->n && shown < 5; i++) {
            if (s->st[i].kind != MR_ST_INIT)
                continue;
            printf("  %5d  text=%p '%.60s'\n", i, (void *)s->st[i].text,
                   s->st[i].text ? s->st[i].text : "NULL");
            shown++;
        }
    }

    return 0;
}

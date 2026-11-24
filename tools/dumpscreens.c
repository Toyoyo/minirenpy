/* dumpscreens.c - run the screen language parser over a .rpy and print the
 * tree, so the parse can be checked against the source by eye and by counts.
 *
 *   gcc -I src -I tools/hostshim -o tools/dumpscreens \
 *       tools/dumpscreens.c src/slang.c src/slparse.c src/lex.c src/vfs.c
 *   ./tools/dumpscreens <game-dir> screens.rpy [-v]
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "mr.h"
#include "lex.h"
#include "slang.h"
#include "slparse.h"
#include "vfs.h"

void mr_log(const char *fmt, ...)
{
    va_list ap;
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

static long g_nodes, g_props, g_unknown;
static int  g_verbose;

static const char *kindname(sl_kind k)
{
    switch (k) {
    case SL_DISPLAYABLE: return "";
    case SL_IF:          return "if";
    case SL_FOR:         return "for";
    case SL_PYTHON:      return "$";
    case SL_DEFAULT:     return "default";
    case SL_USE:         return "use";
    }
    return "?";
}

static void dump(const sl_node *n, int depth)
{
    int i;

    g_nodes++;
    g_props += n->nprops;

    if (n->kind == SL_DISPLAYABLE && n->type[0] && !sl_is_displayable(n->type)) {
        g_unknown++;
        fprintf(stderr, "unknown displayable '%s' (line %d)\n", n->type, n->line);
    }

    if (g_verbose) {
        printf("%*s", depth * 2 + 2, "");
        if (n->kind == SL_DISPLAYABLE)
            printf("%s", n->type[0] ? n->type : "(anon)");
        else
            printf("<%s>", kindname(n->kind));

        if (n->cond) printf(" cond=\"%.40s\"", n->cond);
        if (n->var[0]) printf(" var=%s", n->var);
        if (n->arg)  printf(" arg=\"%.40s\"", n->arg);
        if (n->arg2) printf(" arg2=\"%.20s\"", n->arg2);

        for (i = 0; i < n->nprops; i++)
            printf(" %s=\"%.28s\"", n->props[i].name, n->props[i].expr);
        printf("\n");
    }

    for (i = 0; i < n->nkids; i++)
        dump(n->kids[i], depth + 1);
    if (n->orelse)
        dump(n->orelse, depth);
}

int main(int argc, char **argv)
{
    mr_lines *l;
    int i;
    int nscreens = 0;
    const char *dir, *file;

    if (argc < 3) {
        fprintf(stderr, "usage: dumpscreens <game-dir> <file.rpy> [-v|-s NAME]\n");
        return 2;
    }
    dir = argv[1];
    file = argv[2];

    if (argc > 3 && !strcmp(argv[3], "-v"))
        g_verbose = 1;

    if (!mr_vfs_init(NULL, dir)) {
        fprintf(stderr, "cannot mount %s\n", dir);
        return 1;
    }

    l = mr_lex_file(file);
    if (!l) {
        fprintf(stderr, "cannot read %s\n", file);
        return 1;
    }

    i = 0;
    while (i < l->n) {
        char word[32];
        mr_next_word(l->v[i].text, word, sizeof(word));

        if (!strcmp(word, "screen")) {
            sl_screen sc;
            int indent = l->v[i].indent;
            int only = (argc > 4 && !strcmp(argv[3], "-s"));

            if (mr_sl_parse_screen(l, &i, indent, &sc)) {
                int show = !only || !strcmp(sc.name, argv[4]);
                int saved = g_verbose;

                if (only)
                    g_verbose = show;

                if (show || !only) {
                    printf("screen %s", sc.name);
                    if (sc.nparams) {
                        int k;
                        printf("(");
                        for (k = 0; k < sc.nparams; k++)
                            printf("%s%s", k ? ", " : "", sc.params[k]);
                        printf(")");
                    }
                    if (sc.tag[0]) printf(" tag=%s", sc.tag);
                    printf("\n");
                }
                dump(sc.root, 0);
                g_verbose = saved;
                nscreens++;
                sl_screen_free(&sc);
                continue;
            }
        }
        i++;
    }

    printf("\n=== %d screens, %ld nodes, %ld properties, %ld unknown types ===\n",
           nscreens, g_nodes, g_props, g_unknown);

    mr_lex_free(l);
    mr_vfs_shutdown();
    return 0;
}

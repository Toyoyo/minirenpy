/* dumpscript.c - host-side harness that runs the real lexer and parser over
 * the game's .rpy files and prints the compiled statement array.
 *
 * This exists because parser bugs are the most likely source of a game that
 * runs but misbehaves, and they are far cheaper to find here than on a
 * Windows 3.1 machine. Build with:
 *
 *   gcc -I src -I tools/hostshim -o tools/dumpscript \
 *       tools/dumpscript.c src/lex.c src/parse.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "mr.h"
#include "script.h"
#include "vfs.h"
#include "style.h"
#include "expr.h"

/* The parser only needs these two from the platform layer. */
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

/* Just enough of the builtin vocabulary for the init assignments. */
static int host_call(const char *name, mr_value *argv, const char **argname,
                     int argc, mr_value *result)
{
    (void)argname;
    memset(result, 0, sizeof(*result));

    if (!strcmp(name, "_") && argc > 0) {
        *result = argv[0];
        return 1;
    }
    if (!strcmp(name, "Borders")) {
        char buf[64];
        sprintf(buf, "%d %d %d %d",
                argc > 0 ? (int)argv[0].i : 0, argc > 1 ? (int)argv[1].i : 0,
                argc > 2 ? (int)argv[2].i : 0, argc > 3 ? (int)argv[3].i : 0);
        mr_value_set_str(result, buf);
        return 1;
    }
    return 0;
}

static const char *kindname(mr_st_kind k)
{
    switch (k) {
    case MR_ST_NOP:        return "nop";
    case MR_ST_LABEL:      return "label";
    case MR_ST_SAY:        return "say";
    case MR_ST_SCENE:      return "scene";
    case MR_ST_SHOW:       return "show";
    case MR_ST_HIDE:       return "hide";
    case MR_ST_WITH:       return "with";
    case MR_ST_JUMP:       return "jump";
    case MR_ST_CALL:       return "call";
    case MR_ST_RETURN:     return "return";
    case MR_ST_IF:         return "if";
    case MR_ST_GOTO:       return "goto";
    case MR_ST_MENU:       return "menu";
    case MR_ST_CHOICE:     return "choice";
    case MR_ST_PLAY:       return "play";
    case MR_ST_QUEUE:      return "queue";
    case MR_ST_STOP:       return "stop";
    case MR_ST_WINDOW:     return "window";
    case MR_ST_PAUSE:      return "pause";
    case MR_ST_PYTHON:     return "python";
    case MR_ST_CALLSCREEN: return "callscreen";
    case MR_ST_IMAGE:      return "image";
    case MR_ST_INIT:       return "init";
    case MR_ST_SHOWSCREEN: return "showscreen";
    case MR_ST_HIDESCREEN: return "hidescreen";
    }
    return "?";
}

static const char *transname(mr_trans_kind k)
{
    switch (k) {
    case MR_TRANS_NONE:        return "none";
    case MR_TRANS_DISSOLVE:    return "dissolve";
    case MR_TRANS_FADE:        return "fade";
    case MR_TRANS_MOVE:        return "move";
    case MR_TRANS_MOVEINLEFT:  return "moveinleft";
    case MR_TRANS_MOVEINRIGHT: return "moveinright";
    case MR_TRANS_MOVEOUTLEFT: return "moveoutleft";
    case MR_TRANS_MOVEOUTRIGHT:return "moveoutright";
    }
    return "?";
}

int main(int argc, char **argv)
{
    mr_script *s;
    int i;
    int verbose = 0;
    long counts[32];
    const char *dir;

    if (argc < 2) {
        fprintf(stderr, "usage: dumpscript <game-dir> [-v]\n");
        return 2;
    }
    dir = argv[1];
    if (argc > 2 && !strcmp(argv[2], "-v"))
        verbose = 1;

    /* The scripts are read through the VFS now, so mount the data first.
     * Loose files are fine here; the archive is not needed for parsing. */
    if (!mr_vfs_init(NULL, dir)) {
        fprintf(stderr, "could not mount %s\n", dir);
        return 1;
    }

    s = mr_script_load(dir);
    if (!s) {
        fprintf(stderr, "load failed\n");
        return 1;
    }

    memset(counts, 0, sizeof(counts));
    for (i = 0; i < s->n; i++)
        counts[s->st[i].kind]++;

    printf("=== %d statements, %d labels, %d characters ===\n",
           s->n, s->nlabels, s->nchars);
    for (i = 0; i < 32; i++) {
        if (counts[i])
            printf("  %-12s %ld\n", kindname((mr_st_kind)i), counts[i]);
    }

    printf("\n=== characters ===\n");
    for (i = 0; i < s->nchars; i++) {
        printf("  %-8s who=\"%s\" color=#%06lX\n",
               s->chars[i].var, s->chars[i].who,
               (unsigned long)(s->chars[i].color & 0xFFFFFFUL));
    }

    printf("\n=== screens ===\n");
    for (i = 0; i < s->nscreens; i++) {
        const sl_screen *sc = &s->screens[i];
        printf("  %s (%d top-level node%s)\n", sc->name, sc->root->nkids,
               sc->root->nkids == 1 ? "" : "s");
    }

    /* The gui.* values the style expansions read come from the init-phase
     * assignments, so run those first exactly as the interpreter does. */
    mr_expr_init(host_call);
    for (i = 0; i < s->n; i++) {
        if (s->st[i].kind == MR_ST_INIT && s->st[i].text)
            mr_expr_exec(s->st[i].text);
    }

    /* Style resolution probes: each should come from the style itself or be
     * inherited along the parent chain. */
    mr_style_finalize();
    printf("\n=== style lookups (%d styles) ===\n", mr_style_count());
    {
        static const char *probe[][2] = {
            { "navigation_button_text", "size" },
            { "navigation_button_text", "idle_color" },
            { "button_text",            "idle_color" },
            { "navigation_button_text", "hover_color" },
            { "navigation_button",      "xsize" },
            { "button_text",            "font" },
            { "say_dialogue",           "xpos" },
            { "say_label",              "color" },
            { "choice_button_text",     "size" },
            { "quick_button_text",      "size" },
            { NULL, NULL }
        };
        int k;
        for (k = 0; probe[k][0]; k++) {
            const char *v = mr_style_get(probe[k][0], probe[k][1]);
            printf("  %-24s %-14s -> %s\n", probe[k][0], probe[k][1],
                   v ? v : "(unset)");
        }
    }

    /* Every jump and call must resolve, or the game will stall at runtime. */
    printf("\n=== unresolved control flow ===\n");
    {
        int bad = 0;
        for (i = 0; i < s->n; i++) {
            mr_stmt *st = &s->st[i];
            if (st->kind == MR_ST_JUMP || st->kind == MR_ST_CALL) {
                if (mr_script_find_label(s, st->name) < 0) {
                    printf("  %s to unknown label '%s' (%s:%d)\n",
                           kindname(st->kind), st->name,
                           s->files[st->file], st->line);
                    bad++;
                }
            }
            if ((st->kind == MR_ST_IF || st->kind == MR_ST_GOTO ||
                 st->kind == MR_ST_CHOICE) &&
                (st->target < 0 || st->target > s->n)) {
                printf("  %s with bad target %d (%s:%d)\n",
                       kindname(st->kind), st->target,
                       s->files[st->file], st->line);
                bad++;
            }
        }
        printf("  %d problem(s)\n", bad);
    }

    if (verbose) {
        printf("\n=== statements ===\n");
        for (i = 0; i < s->n; i++) {
            mr_stmt *st = &s->st[i];
            printf("%5d %-11s", i, kindname(st->kind));
            if (st->name[0])
                printf(" name=\"%s\"", st->name);
            if (st->kind == MR_ST_WITH || st->trans.kind != MR_TRANS_NONE)
                printf(" trans=%s/%ldms", transname(st->trans.kind), st->trans.ms);
            if (st->has_place) {
                printf(" place=[");
                if (st->place.has_xalign) printf("xalign %.2f ", st->place.xalign);
                if (st->place.has_yalign) printf("yalign %.2f ", st->place.yalign);
                if (st->place.xoffset) printf("xoff %d ", st->place.xoffset);
                if (st->place.yoffset) printf("yoff %d ", st->place.yoffset);
                printf("]");
            }
            /* The timed steps, which may have come from an inline block or
             * from `at <transform>`. Printed because "the sprite lands in the
             * right place without travelling" is a report that cannot be told
             * apart from "the placement is right" without seeing whether the
             * statement carries any steps at all. */
            if (st->atl) {
                int k;
                printf(" atl=%d%s[", st->atl->n, st->atl->repeat ? "r" : "");
                for (k = 0; k < st->atl->n; k++) {
                    const mr_place *p = &st->atl->v[k].place;
                    printf("%s%ldms", k ? " " : "", st->atl->v[k].ms);
                    if (p->has_xalign)  printf(" xalign %.2f", p->xalign);
                    if (p->has_yalign)  printf(" yalign %.2f", p->yalign);
                    if (p->has_xpos)    printf(" xpos %.2f", p->xpos);
                    if (p->has_ypos)    printf(" ypos %.2f", p->ypos);
                    if (p->has_xoffset) printf(" xoff %d", p->xoffset);
                    if (p->has_yoffset) printf(" yoff %d", p->yoffset);
                    if (p->has_alpha)   printf(" alpha %.2f", p->alpha);
                    if (p->has_xzoom)   printf(" xzoom %.2f", p->xzoom);
                    if (p->has_yzoom)   printf(" yzoom %.2f", p->yzoom);
                    if (p->has_zoom)    printf(" zoom %.2f", p->zoom);
                }
                printf("]");
            }
            if (st->target >= 0)
                printf(" -> %d", st->target);
            if (st->aux)
                printf(" aux=%d", st->aux);
            if (st->text)
                printf(" text=\"%.70s\"", st->text);
            printf("  (%s:%d)\n", s->files[st->file], st->line);
        }
    }

    mr_script_free(s);
    mr_vfs_shutdown();
    return 0;
}

/* dumpsave.c - what is actually inside a .SAV file.
 *
 * A save is a flat little-endian stream with no section table, so the only
 * way to answer "how much history is in this slot" or "how many rollback
 * checkpoints survived" is to walk it from the front. The engine can do that,
 * but loading a save needs the script it was taken from (checkpoints are
 * named by statement id), and the question is usually about a file whose game
 * is not the one currently built. This walks the bytes instead.
 *
 * Build (host):
 *     gcc -I src -o /tmp/dumpsave tools/dumpsave.c src/serial.c
 *
 *     /tmp/dumpsave SAVEP2S6.SAV
 *
 * It reads versions 5 to 9 -- the same range mr_save_load accepts -- because
 * the question "why will this old slot not load" is one of the things it is
 * for. It necessarily repeats the layout written by
 * mr_save_write (save.c) and mr_history_write (history.c): when either of
 * those changes, this has to follow, and the sanity checks below are what
 * catch the omission -- a layout that has drifted does not read plausible
 * counts for long.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mr.h"
#include "serial.h"
#include "expr.h"       /* MR_VSTR_V5/V6: the pre-version-7 string widths */

#define MR_SAVE_MAGIC   0x5653524DUL
#define MR_SAVE_STACK   32
#define THUMB_W 256
#define THUMB_H 144
/* Two size words, then 16bpp pixels. */
#define THUMB_BYTES (8 + THUMB_W * THUMB_H * 2)

/* Fixed-width fields, as mr_save_write spells them. */
#define F_NAME   64
#define F_WHEN   40
#define F_WHO    MR_MAX_NAME
#define F_WHAT   MR_MAX_TEXT
#define F_TLID   (MR_MAX_NAME + 24)
#define F_MUSIC  128

/* serial.c logs a clamped count; nothing here needs the engine's logger. */
void mr_log(const char *fmt, ...) { (void)fmt; }

static void skip_str(FILE *f, int n) { fseek(f, n, SEEK_CUR); }

static void skip_vstr(FILE *f)
{
    long n = (long)mr_get32(f);
    if (n < 0 || n > (1L << 20)) {
        fprintf(stderr, "dumpsave: implausible string length %ld at %ld -- "
                        "the layout here does not match save.c\n",
                n, ftell(f));
        exit(1);
    }
    fseek(f, n, SEEK_CUR);
}

static void read_vstr(FILE *f, char *out, int max)
{
    mr_get_vstr(f, out, max);
}

static void skip_place(FILE *f)
{
    /* mr_put_place writes ten flag/value pairs: twenty words. */
    fseek(f, 20 * 4, SEEK_CUR);
}

static long count_or_die(FILE *f, const char *what, long max)
{
    long n = (long)mr_get32(f);

    if (n < 0 || n > max) {
        fprintf(stderr, "dumpsave: %s count %ld is out of range (max %ld) "
                        "at offset %ld -- stopping\n",
                what, n, max, ftell(f));
        exit(1);
    }
    return n;
}

int main(int argc, char **argv)
{
    FILE *f;
    char buf[MR_MAX_TEXT];
    long ver, i, k, n, nvar, nhist, npoint, kind;
    long when_t;

    if (argc < 2) {
        fprintf(stderr, "usage: dumpsave <file.SAV>\n");
        return 2;
    }

    f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "dumpsave: cannot open %s\n", argv[1]);
        return 1;
    }

    if ((unsigned long)mr_get32(f) != MR_SAVE_MAGIC) {
        fprintf(stderr, "dumpsave: not a save file (bad magic)\n");
        return 1;
    }
    ver = (long)mr_get32(f);
    printf("%s\n", argv[1]);
    printf("  version          %ld\n", ver);
    if (ver < 5 || ver > 9) {
        fprintf(stderr, "dumpsave: versions 5 to 9 only\n");
        return 1;
    }

    mr_get_str(f, buf, F_NAME);
    printf("  slot name        \"%s\"\n", buf);
    mr_get_str(f, buf, F_WHEN);
    printf("  written          %s", buf);
    if (ver >= 8) {
        when_t = (long)mr_get32(f);
        printf("  (epoch %ld)", when_t);
    }
    printf("\n");

    read_vstr(f, buf, sizeof(buf));
    printf("  resume at        %s\n", buf);
    n = count_or_die(f, "call stack", MR_SAVE_STACK);
    printf("  call stack       %ld\n", n);
    for (i = 0; i < n; i++)
        skip_vstr(f);

    skip_str(f, F_WHO);
    skip_str(f, F_WHAT);
    skip_str(f, F_TLID);
    mr_get32(f);                         /* has_dialogue */
    mr_get32(f);                         /* window_shown */

    skip_str(f, MR_MAX_NAME);            /* scene.bg */
    n = count_or_die(f, "scene", 64);
    printf("  scene images     %ld\n", n);
    for (i = 0; i < n; i++) {
        skip_str(f, MR_MAX_NAME);
        skip_str(f, MR_MAX_NAME);
        skip_place(f);
    }
    n = count_or_die(f, "story screens", 64);
    printf("  story screens    %ld\n", n);
    for (i = 0; i < n; i++)
        skip_str(f, MR_MAX_NAME);

    skip_str(f, F_MUSIC);
    mr_get32(f);                         /* music_loop */

    nvar = count_or_die(f, "variables", 100000);
    printf("  variables        %ld\n", nvar);
    for (i = 0; i < nvar; i++) {
        skip_str(f, 64);
        mr_get32(f);                     /* type */
        mr_get32(f);                     /* i */
        mr_get32(f);                     /* f, 16.16 */
        /* Length-prefixed from version 7; a fixed field before that, and the
         * width is the MR_VSTR_MAX of whichever version wrote it. */
        if (ver >= 7)
            skip_vstr(f);
        else
            skip_str(f, ver >= 6 ? MR_VSTR_V6 : MR_VSTR_V5);
    }

    /* The two answers this tool exists for. */
    nhist = count_or_die(f, "history", 100000);
    printf("  history lines    %ld\n", nhist);
    for (i = 0; i < nhist; i++) {
        skip_vstr(f);                    /* who */
        skip_vstr(f);                    /* what */
        mr_get32(f);                     /* colour */
    }

    npoint = count_or_die(f, "rollback points", 1000);
    printf("  rollback points  %ld\n", npoint);
    for (i = 0; i < npoint; i++) {
        long ndelta, nscene;

        read_vstr(f, buf, sizeof(buf));
        n = count_or_die(f, "checkpoint stack", MR_SAVE_STACK);
        for (k = 0; k < n; k++)
            skip_vstr(f);
        if (ver >= 9)
            kind = (long)mr_get32(f);    /* mr_cp_kind; version 9 */
        else
            kind = -1;
        mr_get32(f);                     /* window_shown */
        skip_vstr(f);                    /* music */
        mr_get32(f);                     /* music_loop */
        skip_vstr(f);                    /* scene.bg */
        nscene = count_or_die(f, "checkpoint scene", 64);
        for (k = 0; k < nscene; k++) {
            skip_vstr(f);
            skip_vstr(f);
            skip_place(f);
        }
        ndelta = count_or_die(f, "checkpoint deltas", 100000);
        for (k = 0; k < ndelta; k++) {
            skip_vstr(f);
            mr_get32(f);
            mr_get32(f);
            mr_get32(f);
            mr_get32(f);
            skip_vstr(f);
        }
        printf("    %2ld  %-28s %-13s %2ld images  %3ld deltas\n",
               i, buf,
               kind < 0 ? "(no kind)" :
               kind == 0 ? "say" :
               kind == 1 ? "menu" :
               kind == 2 ? "menu+caption" : "?",
               nscene, ndelta);
    }

    /* The trailing section, and then the thumbnail -- which is what the rest
     * of the file is, so its size is a check that the walk ended where the
     * writer did. */
    {
        long kind = (long)mr_get32(f);
        long ms = (long)mr_get32(f);
        long hard = (long)mr_get32(f);
        long here = ftell(f);
        long end;

        printf("  waiting          kind %ld, %ldms%s\n", kind, ms,
               hard ? ", not clickable" : "");

        fseek(f, 0, SEEK_END);
        end = ftell(f);
        printf("  thumbnail        %ld bytes left (%dx%d at 16bpp is %d)\n",
               end - here, THUMB_W, THUMB_H, THUMB_BYTES);
        if (end - here != (long)THUMB_BYTES)
            printf("  ** the walk did not end where the writer did: either "
                   "this tool is out of step with save.c, or the file is "
                   "truncated **\n");
    }

    fclose(f);
    return 0;
}

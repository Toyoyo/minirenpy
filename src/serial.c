/* serial.c - see serial.h. */

#include <stdio.h>
#include <string.h>

#include "mr.h"
#include "serial.h"

/* --- little-endian primitives --- */

void mr_put32(FILE *f, mr_u32 v)
{
    fputc((int)(v & 0xFF), f);
    fputc((int)((v >> 8) & 0xFF), f);
    fputc((int)((v >> 16) & 0xFF), f);
    fputc((int)((v >> 24) & 0xFF), f);
}

mr_u32 mr_get32(FILE *f)
{
    mr_u32 a = (mr_u32)(fgetc(f) & 0xFF);
    mr_u32 b = (mr_u32)(fgetc(f) & 0xFF);
    mr_u32 c = (mr_u32)(fgetc(f) & 0xFF);
    mr_u32 d = (mr_u32)(fgetc(f) & 0xFF);
    return a | (b << 8) | (c << 16) | (d << 24);
}

/* A double is stored as a scaled integer: the values in play are volumes,
 * text speeds and alignment fractions, none of which need more than this,
 * and it avoids depending on the host and target agreeing on float layout. */
#define FIXED_SCALE 65536.0

void mr_put_fixed(FILE *f, double v)
{
    mr_put32(f, (mr_u32)(long)(v * FIXED_SCALE));
}

double mr_get_fixed(FILE *f)
{
    return (double)(long)(mr_u32)mr_get32(f) / FIXED_SCALE;
}

void mr_put_str(FILE *f, const char *s, int n)
{
    int i;
    for (i = 0; i < n; i++)
        fputc(s && i < (int)strlen(s) ? s[i] : 0, f);
}

void mr_get_str(FILE *f, char *s, int n)
{
    int i;
    for (i = 0; i < n; i++)
        s[i] = (char)fgetc(f);
    s[n - 1] = '\0';
}

/* --- placement, field by field --- */

void mr_put_place(FILE *f, const mr_place *p)
{
    mr_put32(f, (mr_u32)p->has_xalign);   mr_put_fixed(f, p->xalign);
    mr_put32(f, (mr_u32)p->has_yalign);   mr_put_fixed(f, p->yalign);
    mr_put32(f, (mr_u32)p->has_xoffset);  mr_put32(f, (mr_u32)p->xoffset);
    mr_put32(f, (mr_u32)p->has_yoffset);  mr_put32(f, (mr_u32)p->yoffset);
    mr_put32(f, (mr_u32)p->has_xpos);     mr_put_fixed(f, p->xpos);
    mr_put32(f, (mr_u32)p->has_ypos);     mr_put_fixed(f, p->ypos);
    mr_put32(f, (mr_u32)p->xpos_px);      mr_put32(f, (mr_u32)p->ypos_px);
    mr_put32(f, (mr_u32)p->has_xanchor);  mr_put_fixed(f, p->xanchor);
    mr_put32(f, (mr_u32)p->has_yanchor);  mr_put_fixed(f, p->yanchor);
    mr_put32(f, (mr_u32)p->xanchor_px);   mr_put32(f, (mr_u32)p->yanchor_px);
}

void mr_get_place(FILE *f, mr_place *p)
{
    memset(p, 0, sizeof(*p));
    p->has_xalign = (int)mr_get32(f);   p->xalign = (float)mr_get_fixed(f);
    p->has_yalign = (int)mr_get32(f);   p->yalign = (float)mr_get_fixed(f);
    p->has_xoffset = (int)mr_get32(f);  p->xoffset = (int)mr_get32(f);
    p->has_yoffset = (int)mr_get32(f);  p->yoffset = (int)mr_get32(f);
    p->has_xpos = (int)mr_get32(f);     p->xpos = (float)mr_get_fixed(f);
    p->has_ypos = (int)mr_get32(f);     p->ypos = (float)mr_get_fixed(f);
    p->xpos_px = (int)mr_get32(f);      p->ypos_px = (int)mr_get32(f);
    p->has_xanchor = (int)mr_get32(f);  p->xanchor = (float)mr_get_fixed(f);
    p->has_yanchor = (int)mr_get32(f);  p->yanchor = (float)mr_get_fixed(f);
    p->xanchor_px = (int)mr_get32(f);   p->yanchor_px = (int)mr_get32(f);
}

/* The whole placement, including the five properties mr_put_place leaves out.
 *
 * mr_put_place is frozen at the field set it had when the scene section of a
 * save was first written: alpha, the two zooms and the combined zoom were
 * appended later, and save.c reads them with its own `ver >=` gates after
 * calling mr_get_place. Widening mr_put_place would therefore silently move
 * every field after it in every existing slot.
 *
 * A section added from now on has no such history, so it uses this form and
 * gets all of it in one call. */
void mr_put_place_full(FILE *f, const mr_place *p)
{
    mr_put_place(f, p);
    mr_put32(f, (mr_u32)p->has_alpha);   mr_put_fixed(f, p->alpha);
    mr_put32(f, (mr_u32)p->has_xzoom);   mr_put_fixed(f, p->xzoom);
    mr_put32(f, (mr_u32)p->has_yzoom);   mr_put_fixed(f, p->yzoom);
    mr_put32(f, (mr_u32)p->has_zoom);    mr_put_fixed(f, p->zoom);
}

void mr_get_place_full(FILE *f, mr_place *p)
{
    mr_get_place(f, p);                  /* memsets, so the rest is clean */
    p->has_alpha = (int)mr_get32(f);     p->alpha = (float)mr_get_fixed(f);
    p->has_xzoom = (int)mr_get32(f);     p->xzoom = (float)mr_get_fixed(f);
    p->has_yzoom = (int)mr_get32(f);     p->yzoom = (float)mr_get_fixed(f);
    p->has_zoom = (int)mr_get32(f);      p->zoom = (float)mr_get_fixed(f);
}

/* An ATL block, by value.
 *
 * By value rather than as a reference to the statement that owns it, which is
 * how the engine carries it in memory (mr_shown's `atl` points into the
 * script). A file cannot hold that pointer, and naming the statement instead
 * -- a stmtid, as the execution point does -- would make resuming an
 * animation depend on the weakest of the three stmtid forms resolving. A
 * block is six steps of a placement each, so the cost of just writing it down
 * is under a kilobyte and the only entries that carry one are the ones that
 * were actually animating. */
void mr_put_atl(FILE *f, const mr_atl *a)
{
    int i;

    mr_put32(f, (mr_u32)a->n);
    mr_put32(f, (mr_u32)a->repeat);
    for (i = 0; i < a->n && i < MR_MAX_ATL; i++) {
        mr_put32(f, (mr_u32)a->v[i].ms);
        mr_put32(f, (mr_u32)a->v[i].warp);
        mr_put_place_full(f, &a->v[i].place);
    }
}

void mr_get_atl(FILE *f, mr_atl *a)
{
    int i;

    memset(a, 0, sizeof(*a));
    a->n = (int)mr_get32(f);
    a->repeat = (int)mr_get32(f);
    /* Clamped, not rejected: the loop below has to read exactly as many steps
     * as the writer wrote, so a count from a damaged file is bounded rather
     * than used to decide how far to read. */
    if (a->n < 0 || a->n > MR_MAX_ATL) {
        mr_log("save: atl step count %d is not believable", a->n);
        a->n = 0;
        return;
    }
    for (i = 0; i < a->n; i++) {
        a->v[i].ms = (long)mr_get32(f);
        a->v[i].warp = (mr_warper)mr_get32(f);
        mr_get_place_full(f, &a->v[i].place);
    }
}

long mr_get_count(FILE *f, long max, const char *what)
{
    long n = (long)(mr_u32)mr_get32(f);

    /* Read as unsigned and compared as signed: a 0xFFFFFFFF from a truncated
     * file must not come back as -1 and quietly mean "none". */
    if (n < 0 || n > max) {
        mr_log("save: %s count %ld is not believable, clamping to %ld",
               what, n, max);
        n = (n < 0) ? 0 : max;
    }
    return n;
}

void mr_put_vstr(FILE *f, const char *s)
{
    int n = s ? (int)strlen(s) : 0;

    mr_put32(f, (mr_u32)n);
    if (n)
        fwrite(s, 1, (size_t)n, f);
}

void mr_get_vstr(FILE *f, char *s, int max)
{
    int n = (int)mr_get32(f);
    int keep = n;

    if (keep > max - 1)
        keep = max - 1;
    if (keep < 0)
        keep = 0;
    if (keep && fread(s, 1, (size_t)keep, f) != (size_t)keep)
        keep = 0;
    s[keep] = '\0';

    /* Skip anything that did not fit rather than desynchronising. */
    if (n > keep)
        fseek(f, (long)(n - keep), SEEK_CUR);
}

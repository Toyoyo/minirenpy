/* stmtid.c - naming statements so saves survive script edits. See stmtid.h. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mr.h"
#include "stmtid.h"

static const mr_script *g_s;

/* The translation-identifier index: one entry per say statement, sorted by
 * hash. Hashes rather than the strings themselves because a tlid is ~30 bytes
 * and there are thousands of them; a collision is resolved by comparing the
 * strings, which is why the statement index is kept alongside. */
typedef struct {
    mr_u32 h;
    long   pc;
} tl_ent;

static tl_ent *g_idx;
static long    g_nidx;

void mr_stmtid_init(const mr_script *s)
{
    g_s = s;
    mr_stmtid_done();
}

/* FNV-1a, as persist.c uses for the seen-text set. */
static mr_u32 hash32(const char *s)
{
    mr_u32 h = 2166136261UL;
    while (*s) {
        h ^= (mr_u32)(unsigned char)*s++;
        h *= 16777619UL;
    }
    return h;
}

static int ent_cmp(const void *a, const void *b)
{
    mr_u32 ha = ((const tl_ent *)a)->h, hb = ((const tl_ent *)b)->h;
    if (ha < hb) return -1;
    if (ha > hb) return 1;
    return 0;
}

static void build(void)
{
    long i, n = 0;

    if (g_idx || !g_s)
        return;

    for (i = 0; i < g_s->n; i++)
        if (g_s->st[i].tlid)
            n++;
    if (!n)
        return;

    g_idx = (tl_ent *)malloc((size_t)n * sizeof(tl_ent));
    if (!g_idx)
        return;                 /* resolve falls back to a linear scan */

    for (i = 0; i < g_s->n; i++) {
        if (!g_s->st[i].tlid)
            continue;
        g_idx[g_nidx].h = hash32(g_s->st[i].tlid);
        g_idx[g_nidx].pc = i;
        g_nidx++;
    }

    qsort(g_idx, (size_t)g_nidx, sizeof(tl_ent), ent_cmp);
}

void mr_stmtid_done(void)
{
    free(g_idx);
    g_idx = NULL;
    g_nidx = 0;
}

static long find_tlid(const char *tlid)
{
    long lo, hi, i;
    mr_u32 h;

    if (!g_s)
        return -1;

    build();

    if (!g_idx) {
        for (i = 0; i < g_s->n; i++)
            if (g_s->st[i].tlid && !strcmp(g_s->st[i].tlid, tlid))
                return i;
        return -1;
    }

    h = hash32(tlid);

    /* Leftmost entry with this hash. */
    lo = 0;
    hi = g_nidx;
    while (lo < hi) {
        long mid = lo + (hi - lo) / 2;
        if (g_idx[mid].h < h)
            lo = mid + 1;
        else
            hi = mid;
    }

    for (i = lo; i < g_nidx && g_idx[i].h == h; i++) {
        long pc = g_idx[i].pc;
        if (g_s->st[pc].tlid && !strcmp(g_s->st[pc].tlid, tlid))
            return pc;
    }
    return -1;
}

void mr_stmtid_name(long pc, char *out, int outsz)
{
    long k;

    if (!out || outsz < 8)
        return;
    out[0] = '\0';

    if (!g_s)
        return;

    if (pc == g_s->n) {
        strcpy(out, "$end");
        return;
    }

    if (pc < 0 || pc > g_s->n) {
        /* Nothing sensible to say, and nothing that will resolve. */
        sprintf(out, "?%ld", pc);
        return;
    }

    if (g_s->st[pc].tlid && (int)strlen(g_s->st[pc].tlid) < outsz - 2) {
        out[0] = 'T';
        strcpy(out + 1, g_s->st[pc].tlid);
        return;
    }

    /* Just past a say: anchor to it. The window is deliberately short -- a
     * say is followed by at most its `with` before the next statement, and a
     * larger delta would span statements that an edit could add to, which is
     * the weakness this is avoiding in the first place. */
    for (k = pc - 1; k >= 0 && k >= pc - MR_STMTID_NEAR; k--) {
        if (!g_s->st[k].tlid)
            continue;
        if ((int)strlen(g_s->st[k].tlid) < outsz - 16)
            sprintf(out, "T%s+%ld", g_s->st[k].tlid, pc - k);
        break;
    }
    if (out[0])
        return;

    for (k = pc; k >= 0; k--)
        if (g_s->st[k].kind == MR_ST_LABEL)
            break;

    if (k >= 0 && (int)strlen(g_s->st[k].name) < outsz - 16) {
        sprintf(out, "L%s+%ld", g_s->st[k].name, pc - k);
        return;
    }

    /* Outside every label. Execution never reaches such a statement -- they
     * are the declarations -- so this should not arise; if it does, refuse to
     * name it rather than fall back to an index that means nothing later. */
    sprintf(out, "?%ld", pc);
}

long mr_stmtid_resolve(const char *name)
{
    if (!g_s || !name || !name[0])
        return -1;

    if (!strcmp(name, "$end"))
        return g_s->n;

    if (name[0] == 'T') {
        const char *plus = strrchr(name + 1, '+');
        long base, off = 0;

        /* A tlid is `<label>_<hex digest>`, so a '+' in the name can only be
         * the offset separator. */
        if (plus) {
            char id[MR_STMTID_MAX];
            int len = (int)(plus - (name + 1));

            if (len <= 0 || len >= (int)sizeof(id))
                return -1;
            memcpy(id, name + 1, (size_t)len);
            id[len] = '\0';
            off = atol(plus + 1);
            if (off < 0 || off > MR_STMTID_NEAR)
                return -1;
            base = find_tlid(id);
        } else {
            base = find_tlid(name + 1);
        }

        if (base < 0)
            return -1;
        if (base + off > g_s->n)
            return -1;
        return base + off;
    }

    if (name[0] == 'L') {
        const char *plus = strrchr(name + 1, '+');
        char lab[MR_MAX_NAME];
        long off, base;
        int len;

        if (!plus)
            return -1;
        len = (int)(plus - (name + 1));
        if (len <= 0 || len >= MR_MAX_NAME)
            return -1;
        memcpy(lab, name + 1, (size_t)len);
        lab[len] = '\0';

        off = atol(plus + 1);
        if (off < 0)
            return -1;

        base = mr_script_find_label(g_s, lab);
        if (base < 0)
            return -1;
        if (base + off > g_s->n)
            return -1;

        return base + off;
    }

    return -1;
}

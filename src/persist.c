/* persist.c - reading and writing the persistent data. See persist.h.
 *
 * Same shape as a save slot's variable block, and written with the same
 * explicit little-endian primitives for the same reason: the host harnesses
 * are 64-bit and the target is 32.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mr.h"
#include "persist.h"
#include "expr.h"
#include "serial.h"

#define MR_PERSIST_MAGIC   0x5350524DUL   /* "MRPS" */
#define MR_PERSIST_VERSION 1

/* 8.3, like the saves, and beside the executable rather than in the archive,
 * which is read-only. */
#define MR_PERSIST_FILE "PERSIST.SAV"

#define NAME_LEN 64

/* --- seen text ---
 *
 * A sorted array of 32-bit hashes of translation identifiers; see persist.h
 * for why hashes rather than the identifiers themselves. Written as a
 * trailing section of PERSIST.SAV so that the format version does not have
 * to move: an older file simply ends where the section would start, and the
 * reader finds EOF and stops. The save format uses the same trick. */
#define SEEN_TAG 0x4E454553UL      /* "SEEN" */

static mr_u32 *g_seen;
static int     g_nseen;
static int     g_seencap;

static mr_u32 seen_hash(const char *tlid)
{
    mr_u32 h = 2166136261UL;
    for (; *tlid; tlid++) {
        h ^= (mr_u32)(unsigned char)*tlid;
        h *= 16777619UL;
    }
    return h;
}

/* Index of the first entry >= h, i.e. where h is or where it would go. */
static int seen_lower(mr_u32 h)
{
    int lo = 0, hi = g_nseen;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (g_seen[mid] < h)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

int mr_persist_seen(const char *tlid)
{
    mr_u32 h;
    int i;

    if (!tlid || !*tlid)
        return 0;
    h = seen_hash(tlid);
    i = seen_lower(h);
    return i < g_nseen && g_seen[i] == h;
}

void mr_persist_seen_mark(const char *tlid)
{
    mr_u32 h;
    int i;

    if (!tlid || !*tlid)
        return;

    h = seen_hash(tlid);
    i = seen_lower(h);
    if (i < g_nseen && g_seen[i] == h)
        return;                      /* already known */

    if (g_nseen >= g_seencap) {
        int cap = g_seencap ? g_seencap * 2 : 256;
        mr_u32 *p = (mr_u32 *)realloc(g_seen, (size_t)cap * sizeof(mr_u32));
        if (!p)
            return;                  /* forgetting a line is survivable */
        g_seen = p;
        g_seencap = cap;
    }

    memmove(g_seen + i + 1, g_seen + i,
            (size_t)(g_nseen - i) * sizeof(mr_u32));
    g_seen[i] = h;
    g_nseen++;
}

int mr_persist_seen_count(void)
{
    return g_nseen;
}

/* The two families that persist. Anything else belongs to a playthrough and
 * lives in the save slot instead. */
int mr_persist_is_persistent(const char *name)
{
    return !strncmp(name, "persistent.", 11) ||
           !strncmp(name, "_preferences.", 13);
}

int mr_persist_save(void)
{
    FILE *f;
    const char *name;
    const mr_value *v;
    long count = 0;
    long at;
    int i;

    f = fopen(MR_PERSIST_FILE, "wb");
    if (!f) {
        mr_log("persist: cannot write %s", MR_PERSIST_FILE);
        return 0;
    }

    mr_put32(f, MR_PERSIST_MAGIC);
    mr_put32(f, MR_PERSIST_VERSION);

    at = ftell(f);
    mr_put32(f, 0);                /* count, patched below */

    for (i = 0; mr_var_enumerate(i, &name, &v); i++) {
        if (!mr_persist_is_persistent(name))
            continue;
        mr_put_str(f, name, NAME_LEN);
        mr_put32(f, (mr_u32)v->type);
        mr_put32(f, (mr_u32)v->i);
        mr_put_fixed(f, v->f);
        mr_put_str(f, v->s, MR_VSTR_MAX);
        count++;
    }

    /* The seen set, as a trailing section: a reader that predates it stops
     * at what it thinks is EOF, and this reader treats a missing section as
     * "nothing seen yet". */
    mr_put32(f, SEEN_TAG);
    mr_put32(f, (mr_u32)g_nseen);
    for (i = 0; i < g_nseen; i++)
        mr_put32(f, g_seen[i]);

    fseek(f, at, SEEK_SET);
    mr_put32(f, (mr_u32)count);
    fclose(f);

    mr_log("persist: wrote %ld values, %d seen lines", count, g_nseen);
    return 1;
}

int mr_persist_load(void)
{
    FILE *f;
    long count, i;
    int restored = 0;

    f = fopen(MR_PERSIST_FILE, "rb");
    if (!f) {
        /* A first run. Not worth a log line at warning volume. */
        mr_log("persist: no %s yet", MR_PERSIST_FILE);
        return 0;
    }

    if (mr_get32(f) != MR_PERSIST_MAGIC ||
        mr_get32(f) != MR_PERSIST_VERSION) {
        mr_log("persist: %s is not a persistent file of this version",
               MR_PERSIST_FILE);
        fclose(f);
        return 0;
    }

    count = (long)mr_get32(f);
    for (i = 0; i < count; i++) {
        char name[NAME_LEN];
        mr_value v;

        memset(&v, 0, sizeof(v));
        mr_get_str(f, name, NAME_LEN);
        v.type = (int)mr_get32(f);
        v.i = (long)mr_get32(f);
        v.f = mr_get_fixed(f);
        mr_get_str(f, v.s, MR_VSTR_MAX);

        if (feof(f))
            break;
        /* Only the two families, even if the file claims otherwise: a
         * persistent file must not be able to set story variables. */
        if (!mr_persist_is_persistent(name))
            continue;

        /* No journal entry: persistent data is not part of the rollback log,
         * so rolling back must not undo an unlock. */
        mr_var_set_no_journal(name, &v);
        restored++;

        /* Named individually, because "an extra is unlocked that should not
         * be" is otherwise indistinguishable from a bug in whatever reads the
         * flag -- and the file outlives the build that wrote it, so a flag
         * set by an older, buggier binary looks exactly like a live fault.
         * The unlock flags are the ones that matter; the preferences are
         * noise and are counted only. */
        if (strncmp(name, "persistent._", 12))
            mr_log("persist:   %s = %s%ld", name,
                   v.type == MR_V_STR ? "(str) " : "",
                   v.type == MR_V_STR ? (long)strlen(v.s) : v.i);
    }

    /* The trailing seen section, if this file has one. Guarded by a peek:
     * a file written before the section existed just ends here. */
    {
        int c = fgetc(f);

        if (c != EOF) {
            ungetc(c, f);
            if (mr_get32(f) == SEEN_TAG) {
                long n = (long)mr_get32(f);
                long k;

                free(g_seen);
                g_seen = NULL;
                g_nseen = g_seencap = 0;

                if (n > 0 && n < 1000000L) {
                    g_seen = (mr_u32 *)malloc((size_t)n * sizeof(mr_u32));
                    if (g_seen) {
                        g_seencap = (int)n;
                        for (k = 0; k < n && !feof(f); k++)
                            g_seen[k] = mr_get32(f);
                        g_nseen = (int)k;
                    }
                }
                mr_log("persist: %d seen lines", g_nseen);
            }
        }
    }

    fclose(f);
    mr_log("persist: restored %d values", restored);
    return restored;
}

/* --- surviving a load --- */

#define SNAP_MAX 128

static struct {
    char     name[NAME_LEN];
    mr_value v;
} g_snap[SNAP_MAX];
static int g_nsnap;

int mr_persist_snapshot(void)
{
    const char *name;
    const mr_value *v;
    int i;

    g_nsnap = 0;
    for (i = 0; mr_var_enumerate(i, &name, &v); i++) {
        if (!mr_persist_is_persistent(name) || g_nsnap >= SNAP_MAX)
            continue;
        strncpy(g_snap[g_nsnap].name, name, NAME_LEN - 1);
        g_snap[g_nsnap].name[NAME_LEN - 1] = '\0';
        g_snap[g_nsnap].v = *v;
        g_nsnap++;
    }
    return g_nsnap;
}

void mr_persist_restore(void)
{
    int i;
    for (i = 0; i < g_nsnap; i++)
        mr_var_set_no_journal(g_snap[i].name, &g_snap[i].v);
}

void mr_persist_clear(int progress)
{
    /* Collected first: unsetting while enumerating would renumber the store
     * underneath the walk. */
    char doomed[64][NAME_LEN];
    int n = 0;
    const char *name;
    const mr_value *v;
    int i;

    /* Ren'Py's `progress` also drops the seen-text set, which is what makes
     * "reset persistent data" put skipping back to stopping at every line. */
    if (progress) {
        free(g_seen);
        g_seen = NULL;
        g_nseen = g_seencap = 0;
    }

    for (i = 0; mr_var_enumerate(i, &name, &v); i++) {
        if (strncmp(name, "persistent.", 11))
            continue;
        /* Ren'Py keeps the underscore-prefixed fields, `_preferences` above
         * all: resetting progress does not reset the volumes. */
        if (name[11] == '_')
            continue;
        if (n < 64) {
            strncpy(doomed[n], name, NAME_LEN - 1);
            doomed[n][NAME_LEN - 1] = '\0';
            n++;
        }
    }

    for (i = 0; i < n; i++)
        mr_var_unset(doomed[i]);

    mr_log("persist: cleared %d values", n);
}

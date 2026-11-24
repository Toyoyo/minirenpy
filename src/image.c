/* image.c - the image name table and the bounded decoded-pixel cache.
 *
 * Ren'Py resolves `show miwa biwa talk` by looking for an image declared with
 * that exact name, and failing that by treating the name as a filename. We do
 * the same: script `image` statements register explicit entries (including the
 * im.Flip mirrors the game declares), and everything in images/ is registered
 * automatically under its basename.
 *
 * Decoded pixels live in an LRU cache with a fixed byte budget rather than
 * being kept forever, because the 121 images total roughly 190MB at 32bpp.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image.h"
#include "vfs.h"
#include "plat.h"       /* mr_plat_ticks, for the MR_PROF decode timer */

/* How many *names* the registry can hold. Not a cache size -- what a run
 * keeps in memory is MR_CACHE_BUDGET, and that is unaffected by this.
 *
 * Every file under images/ gets a slot from the vfs enumeration whether the
 * script ever shows it or not, every `image` declaration that names something
 * else gets one, and every `gui/...` path a screen mentions gets one the first
 * time it is drawn. Those gui names arrive *last*, so when the table fills it
 * is the menu art that is dropped: Azami overflowed 1024 and its main menu and
 * preferences screens came up black, the backgrounds, frames, sliders and
 * button art all gone while the text drew fine.
 *
 * Azami measures at 792 enumerated files plus 434 declared names plus the gui
 * art, which is how 1024 was reached; 2048 leaves room for a game half again
 * its size. Raising this costs ~376 bytes of static data per slot and nothing
 * at run time. */
#define MR_MAX_IMAGES 2048
#define MR_HASH_SIZE  4096   /* power of two, > 2x MR_MAX_IMAGES */

typedef struct mr_imgdef {
    char  name[64];
    char  file[MR_PATH_MAX];   /* relative to the game directory */
    int   flip;

    /* im.Blur radii, 0 when the name is not a blurred one. Applied once, to
     * the decoded pixels, so a cache hit costs nothing extra -- but a miss
     * pays for it again, which is the thing to know when a game blurs a
     * background it shows repeatedly. */
    float xrad, yrad;

    /* im.FactorScale's factors, applied once to the decoded pixels. Anything
     * at or below zero means "not scaled": mr_stmt is zeroed when a statement
     * is emitted and only image declarations ever carry these, so 0 has to be
     * the no-op rather than 1.0. A negative factor is not meaningful for a
     * manipulator -- it is the transform properties that mirror -- and the
     * parser drops the declaration before it gets here. */
    float xscale, yscale;

    /* Non-NULL when the name is an animated one, in which case `file` and the
     * manipulators above are unused: the frames are named by the anim and each
     * is fetched -- and cached, and LRU'd -- as an image in its own right.
     * Owned here, freed on redefinition and at shutdown. */
    mr_anim *anim;
    /* Scratch for an animated name's interstep cross-fade, allocated on first
     * use at the frame size and kept for as long as the name is defined. It is
     * deliberately *not* counted against MR_CACHE_BUDGET: it is not a cache of
     * anything, it is a frame buffer, and letting the LRU evict it would mean
     * reallocating it every frame of the fade. The cost is one frame-sized
     * surface per animated image that actually cross-fades. */
    mr_surface *blend;

    /* What anim_get last handed out, so the frame loop can ask the question
     * it actually cares about -- "will this look different?" -- instead of
     * "is this animated?". An animation holds each frame for seconds and
     * changes only during a cross-fade or on the single frame where the
     * index flips; the rest of the time it resolves to the very same cached
     * surface, and compositing the stage again produces an identical image.
     * drawn_step is 0..255 mid-cross-fade and -1 when the frame stands
     * alone; drawn_valid is 0 until something has actually been handed out,
     * which has to count as changed or the first frame never draws. */
    int   drawn_frame;
    int   drawn_step;
    int   drawn_valid;

    int   hnext;       /* next index in the hash chain, -1 for end */

    /* Cache state. surf is non-NULL exactly when this image is resident. */
    mr_surface *surf;
    long        bytes;
    int         lru_prev;
    int         lru_next;
    unsigned    pin_gen;   /* frame this entry was last handed out in */
} mr_imgdef;

static mr_imgdef g_img[MR_MAX_IMAGES];
static int       g_nimg;
static int       g_hash[MR_HASH_SIZE];

/* LRU list, most recently used at the head. */
static int  g_lru_head = -1;
static int  g_lru_tail = -1;
static long g_lru_bytes;

/* Bumped once per render pass; entries fetched during the current pass are
 * never evicted, so pointers stay valid for the whole pass. */
static unsigned g_gen = 1;

/* Case-insensitive, because the scripts are not consistent about capitals and
 * the original runs on case-insensitive filesystems. */
static unsigned hash_name(const char *s)
{
    unsigned h = 2166136261u;

    while (*s) {
        unsigned c = (unsigned char)*s++;
        if (c >= 'A' && c <= 'Z')
            c += 32;
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

static int name_eq(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = (unsigned char)*a++;
        int cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb)
            return 0;
    }
    return *a == *b;
}

/* Defined with the LRU bookkeeping below; needed by the redefinition path. */
static void evict(int idx);

/* Everything a definition owns that a redefinition invalidates: the decoded
 * pixels (which carry the old manipulators baked in), and whatever the old
 * definition's animated form allocated. Ren'Py keeps the last definition of a
 * name, so a redefinition overwrites rather than merges -- which means an
 * animated name redefined as a still one has to stop animating, and vice
 * versa. Forgetting half of that is how a name ends up still animating off a
 * definition that no longer says so. */
static void release_def(int idx)
{
    if (g_img[idx].surf)
        evict(idx);
    if (g_img[idx].blend) {
        mr_surface_free(g_img[idx].blend);
        g_img[idx].blend = NULL;
    }
    if (g_img[idx].anim) {
        free(g_img[idx].anim);
        g_img[idx].anim = NULL;
    }
}

static int find_index(const char *name)
{
    int i;

    for (i = g_hash[hash_name(name) & (MR_HASH_SIZE - 1)]; i >= 0;
         i = g_img[i].hnext) {
        if (name_eq(g_img[i].name, name))
            return i;
    }
    return -1;
}

void mr_image_init(const char *game_dir)
{
    int i;

    for (i = 0; i < MR_HASH_SIZE; i++)
        g_hash[i] = -1;
    g_nimg = 0;
    g_lru_head = g_lru_tail = -1;
    g_lru_bytes = 0;

    /* Paths are archive-relative; the VFS resolves them. */
    (void)game_dir;
}

int mr_image_define(const char *name, const char *file, int flip)
{
    return mr_image_define_manip(name, file, flip, 0.0f, 0.0f, 0.0f, 0.0f);
}

int mr_image_define_manip(const char *name, const char *file, int flip,
                          float xrad, float yrad, float xscale, float yscale)
{
    int idx;
    unsigned b;

    idx = find_index(name);
    if (idx >= 0) {
        /* Redefinition: keep the last one, as Ren'Py does. Every manipulator
         * is overwritten, not merged, so redefining a blurred name plainly
         * leaves it unblurred. */
        strncpy(g_img[idx].file, file, sizeof(g_img[idx].file) - 1);
        g_img[idx].file[sizeof(g_img[idx].file) - 1] = '\0';
        g_img[idx].flip = flip;
        g_img[idx].xrad = xrad;
        g_img[idx].yrad = yrad;
        g_img[idx].xscale = xscale;
        g_img[idx].yscale = yscale;
        release_def(idx);
        return 1;
    }

    if (g_nimg >= MR_MAX_IMAGES) {
        /* Said once, with the limit in it, because the drop repeats on every
         * redraw: Azami's log was 1500 lines of 'dropping gui/...' and the
         * one fact worth reading -- that the table had a ceiling and the game
         * had gone past it -- was buried in them. */
        static int told;

        if (!told) {
            told = 1;
            mr_log("image: the image table is full at %d names; "
                   "everything from here on is dropped, starting with '%s'. "
                   "Raise MR_MAX_IMAGES.", MR_MAX_IMAGES, name);
        }
        return 0;
    }

    idx = g_nimg++;
    memset(&g_img[idx], 0, sizeof(g_img[idx]));
    strncpy(g_img[idx].name, name, sizeof(g_img[idx].name) - 1);
    strncpy(g_img[idx].file, file, sizeof(g_img[idx].file) - 1);
    g_img[idx].flip = flip;
    g_img[idx].xrad = xrad;
    g_img[idx].yrad = yrad;
    g_img[idx].xscale = xscale;
    g_img[idx].yscale = yscale;
    g_img[idx].lru_prev = -1;
    g_img[idx].lru_next = -1;

    b = hash_name(name) & (MR_HASH_SIZE - 1);
    g_img[idx].hnext = g_hash[b];
    g_hash[b] = idx;
    return 1;
}

/* The clock the animations run off; see mr_image_set_time. g_now_set exists
 * for one diagnostic: this call sits in the frame loop, one line away from
 * anything that would miss it, and no harness can catch it being dropped --
 * the harnesses supply their own clock, and main.c has no test at all. Its
 * failure mode is every animation frozen on its first frame, which reads as
 * "animations are not implemented" rather than as a missing call. So the
 * registry says so the first time it is asked to resolve an animation with
 * the clock still untouched. */
static long g_now;
static int  g_now_set;

/* Whether this game declares any animated image at all. Lets every per-frame
 * caller skip the whole question with one test instead of a hash lookup per
 * shown image: a game with no animations -- which is every game but one so
 * far -- then pays exactly what it paid before animations existed, both in
 * behaviour and in cycles. On a PIII, thirteen name hashes a frame for an
 * answer that is always no is not free. */
static int  g_any_anim;

void mr_image_set_time(long now_ms)
{
    g_now = now_ms;
    g_now_set = 1;
}

int mr_image_define_anim(const char *name, const mr_anim *anim)
{
    mr_anim *copy;
    int   idx, i;

    if (!anim || anim->n <= 0)
        return 0;

    copy = (mr_anim *)malloc(sizeof(*copy));
    if (!copy) {
        mr_log("image: out of memory defining animation '%s'", name);
        return 0;
    }
    *copy = *anim;

    /* A cross-fade longer than the frame it fades into would never finish,
     * leaving the frame permanently half-way. Ren'Py cannot hit this -- its
     * transition runs on its own clock and the next Child just interrupts it
     * -- but we derive the fade's progress from the frame's own elapsed time,
     * so the two have to be commensurable. Clamped rather than rejected: a
     * fade that exactly fills its frame is the sensible reading. */
    copy->total_ms = 0;
    for (i = 0; i < copy->n; i++) {
        if (copy->v[i].ms < 0)
            copy->v[i].ms = 0;
        if (copy->v[i].trans_ms > copy->v[i].ms)
            copy->v[i].trans_ms = copy->v[i].ms;
        copy->total_ms += copy->v[i].ms;
    }

    /* Every frame has a zero duration, so there is no sequence to play and
     * dividing the clock by the total would divide by zero. Says so rather
     * than silently showing frame 0 forever. */
    if (copy->total_ms <= 0) {
        mr_log("image: animation '%s' has no duration (every frame's pause is "
               "missing or zero); showing its first frame", name);
    }

    idx = find_index(name);
    if (idx >= 0) {
        release_def(idx);
        g_img[idx].flip = 0;
        g_img[idx].xrad = 0.0f;
        g_img[idx].yrad = 0.0f;
        g_img[idx].file[0] = '\0';
        g_img[idx].anim = copy;
        g_any_anim = 1;
        return 1;
    }

    if (g_nimg >= MR_MAX_IMAGES) {
        mr_log("image: table full, dropping animation '%s'", name);
        free(copy);
        return 0;
    }

    idx = g_nimg++;
    memset(&g_img[idx], 0, sizeof(g_img[idx]));
    strncpy(g_img[idx].name, name, sizeof(g_img[idx].name) - 1);
    g_img[idx].anim = copy;
    g_img[idx].lru_prev = -1;
    g_img[idx].lru_next = -1;

    {
        unsigned b = hash_name(name) & (MR_HASH_SIZE - 1);
        g_img[idx].hnext = g_hash[b];
        g_hash[b] = idx;
    }
    g_any_anim = 1;
    return 1;
}

/* --- attribute matching ---
 *
 * See mr_image_choose in image.h for what this is and why. The Ren'Py
 * original is display/image.py:949 (apply_attributes) and :1004
 * (choose_image); the shape is kept, the Python is not.
 */

#define MR_MAX_ATTRS 8      /* tag plus seven; the longest in any game is 5 */
#define MR_ATTR_LEN  20

typedef struct attrset {
    char w[MR_MAX_ATTRS][MR_ATTR_LEN];
    int  n;
} attrset;

static void split_attrs(const char *s, attrset *a)
{
    int k;

    a->n = 0;
    while (*s && a->n < MR_MAX_ATTRS) {
        while (*s == ' ')
            s++;
        if (!*s)
            break;
        k = 0;
        while (*s && *s != ' ') {
            if (k < MR_ATTR_LEN - 1)
                a->w[a->n][k++] = *s;
            s++;
        }
        a->w[a->n][k] = '\0';
        a->n++;
    }
}

/* Membership over [from, n), so the tag can be skipped by passing 1. */
static int attr_in(const attrset *a, int from, const char *w)
{
    int i;

    for (i = from; i < a->n; i++) {
        if (!strcmp(a->w[i], w))
            return 1;
    }
    return 0;
}

static void attr_add(attrset *a, const char *w)
{
    if (attr_in(a, 0, w) || a->n >= MR_MAX_ATTRS)
        return;
    strncpy(a->w[a->n], w, MR_ATTR_LEN - 1);
    a->w[a->n][MR_ATTR_LEN - 1] = '\0';
    a->n++;
}

static void attr_del(attrset *a, const char *w)
{
    int i;

    for (i = 0; i < a->n; i++) {
        if (!strcmp(a->w[i], w)) {
            while (i + 1 < a->n) {
                strcpy(a->w[i], a->w[i + 1]);
                i++;
            }
            a->n--;
            return;
        }
    }
}

int mr_image_choose(const char *name, const char *current,
                    char *out, int outsz)
{
    /* Static rather than automatic: five of these is about 850 bytes, and the
     * Win32s stack is the scarce resource in this program -- the 1024-byte
     * buffers that page-faulted are the standing reminder. Nothing here is
     * reentrant or called from a signal; it is a leaf function on the show
     * path. */
    static attrset want, cur, req, opt, cand;
    int i, k, ok;
    int best = -1, bestlen = -1, nmatch = 0;

    if (!name || !out || outsz <= 0)
        return 0;

    /* "An image that exactly exists" wins outright, before any inheriting
     * (image.py:962-967). That is what makes `show azami pout` drop the
     * `casual` the tag was wearing instead of accumulating it. */
    if (find_index(name) >= 0) {
        strncpy(out, name, (size_t)outsz - 1);
        out[outsz - 1] = '\0';
        return 1;
    }

    /* A name written as a path is a file, not a tag with attributes. */
    if (strchr(name, '/') || strchr(name, '\\'))
        return 0;

    split_attrs(name, &want);
    if (want.n == 0)
        return 0;

    /* The attributes a match *may* have: the ones the tag is wearing now.
     * Ren'Py keeps these in a side table keyed by (layer, tag) and deletes
     * them on hide; the entry on the display list says the same thing, and
     * is gone at the same moment. */
    opt.n = 0;
    if (current && current[0]) {
        split_attrs(current, &cur);
        if (cur.n > 0 && !strcmp(cur.w[0], want.w[0])) {
            for (k = 1; k < cur.n; k++)
                attr_add(&opt, cur.w[k]);
        }
    }

    /* The attributes a match *must* have, and `-attr` which removes one from
     * what was inherited instead of requiring it (image.py:990-997). */
    req.n = 0;
    for (k = 1; k < want.n; k++) {
        if (want.w[k][0] == '-') {
            attr_del(&opt, want.w[k] + 1);
            attr_del(&req, want.w[k] + 1);
        } else {
            attr_add(&req, want.w[k]);
        }
    }

    for (i = 0; i < g_nimg; i++) {
        split_attrs(g_img[i].name, &cand);
        if (cand.n == 0 || strcmp(cand.w[0], want.w[0]))
            continue;

        /* Every attribute the candidate carries has to be one we asked for
         * or one already in force -- otherwise showing `azami` could pick
         * `azami magical`, which is a different character. */
        ok = 1;
        for (k = 1; k < cand.n; k++) {
            if (!attr_in(&req, 0, cand.w[k]) && !attr_in(&opt, 0, cand.w[k])) {
                ok = 0;
                break;
            }
        }
        if (!ok)
            continue;

        /* ...and it has to carry every attribute we did ask for. */
        for (k = 0; k < req.n; k++) {
            if (!attr_in(&cand, 1, req.w[k])) {
                ok = 0;
                break;
            }
        }
        if (!ok)
            continue;

        /* The most specific match wins: `azami casual pout` beats `azami
         * pout` when `casual` is in force (image.py:1040-1050). */
        if (cand.n - 1 > bestlen) {
            bestlen = cand.n - 1;
            best = i;
            nmatch = 1;
        } else if (cand.n - 1 == bestlen) {
            nmatch++;
        }
    }

    if (best < 0)
        return 0;

    /* Ren'Py raises on a tie (image.py:1060). Stopping the game on Win32s
     * over a sprite is the wrong trade, so the first match is taken and the
     * ambiguity is said out loud -- it is a script bug either way. */
    if (nmatch > 1)
        mr_log("image: '%s' matches %d images equally well, using '%s'",
               name, nmatch, g_img[best].name);

    strncpy(out, g_img[best].name, (size_t)outsz - 1);
    out[outsz - 1] = '\0';
    return 1;
}

int mr_image_is_anim(const char *name)
{
    int idx = find_index(name);

    return idx >= 0 && g_img[idx].anim != NULL;
}

int mr_image_any_anim(void)
{
    return g_any_anim;
}

void mr_image_autodefine_dir(void)
{
    char entry[MR_PATH_MAX];
    char name[64];
    int  i, n;
    char *dot;

    /* The archive is the authority on what exists. Every PNG under images/
     * is registered under its basename, which is how Ren'Py resolves a `show`
     * that has no matching `image` statement. */
    n = 0;
    for (i = 0; mr_vfs_enum("images/", ".png", i, entry, sizeof(entry)); i++) {
        const char *base = entry + strlen("images/");

        strncpy(name, base, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        dot = strrchr(name, '.');
        if (dot)
            *dot = '\0';

        /* Explicit `image` statements are registered first and must win. */
        if (find_index(name) < 0) {
            mr_image_define(name, entry, 0);
            n++;
        }
    }

    if (!n)
        mr_log("image: no images/*.png found; is the archive mounted?");
    else
        mr_log("image: auto-registered %d images", n);
}

int mr_image_exists(const char *name)
{
    return find_index(name) >= 0;
}

/* --- LRU bookkeeping --- */

static void lru_unlink(int idx)
{
    if (g_img[idx].lru_prev >= 0)
        g_img[g_img[idx].lru_prev].lru_next = g_img[idx].lru_next;
    else if (g_lru_head == idx)
        g_lru_head = g_img[idx].lru_next;

    if (g_img[idx].lru_next >= 0)
        g_img[g_img[idx].lru_next].lru_prev = g_img[idx].lru_prev;
    else if (g_lru_tail == idx)
        g_lru_tail = g_img[idx].lru_prev;

    g_img[idx].lru_prev = -1;
    g_img[idx].lru_next = -1;
}

static void lru_push_front(int idx)
{
    g_img[idx].lru_prev = -1;
    g_img[idx].lru_next = g_lru_head;
    if (g_lru_head >= 0)
        g_img[g_lru_head].lru_prev = idx;
    g_lru_head = idx;
    if (g_lru_tail < 0)
        g_lru_tail = idx;
}

static void evict(int idx)
{
    lru_unlink(idx);
    g_lru_bytes -= g_img[idx].bytes;
    mr_surface_free(g_img[idx].surf);
    g_img[idx].surf = NULL;
    g_img[idx].bytes = 0;
}

/* Evict from the least recently used end until the new arrival fits, walking
 * past anything the current frame has already handed out. Skipping pinned
 * entries can leave the cache over budget for one frame; that is bounded by
 * the number of images a single scene shows, and is far cheaper than freeing
 * a surface the renderer is still holding. */
static void make_room(long need, int keep)
{
    int victim = g_lru_tail;

    while (g_lru_bytes + need > MR_CACHE_BUDGET && victim >= 0) {
        int prev = g_img[victim].lru_prev;

        if (victim != keep && g_img[victim].pin_gen != g_gen)
            evict(victim);

        victim = prev;
    }
}

void mr_image_begin_frame(void)
{
    g_gen++;
    if (g_gen == 0)         /* wrap: 0 is never a valid pin */
        g_gen = 1;
}

/* Mirror a surface horizontally in place, implementing im.Flip. */
static void flip_h(mr_surface *s)
{
    int y, x, half;
    mr_u32 *row, t;

    half = s->w / 2;
    for (y = 0; y < s->h; y++) {
        row = s->px + (size_t)y * s->pitch;
        for (x = 0; x < half; x++) {
            t = row[x];
            row[x] = row[s->w - 1 - x];
            row[s->w - 1 - x] = t;
        }
    }
}

/* MR_PROF: decoding a PNG is slow enough to dwarf a frame, and it happens
 * inside whichever phase first asks for the image -- the interpreter running
 * a `show`, or the compositor drawing it. Timed separately so it is not
 * mistaken for either. */
static int  g_prof;
static long g_prof_decode;
static int  g_prof_loads;

void mr_image_profile_enable(int on)
{
    g_prof = on;
}

void mr_image_profile(long *ms, int *loads)
{
    *ms = g_prof_decode;
    *loads = g_prof_loads;
    g_prof_decode = 0;
    g_prof_loads = 0;
}

/* Which frame of an animation is showing at `now`, and how far into that
 * frame we are -- which is what the incoming cross-fade's progress is measured
 * against.
 *
 * A repeating animation wraps; a non-repeating one holds its last frame, which
 * is what Ren'Py does when an ATL block runs off the end. */
static int anim_frame_at(const mr_anim *a, long now, long *into)
{
    long t, acc;
    int  i;

    *into = 0;
    if (a->total_ms <= 0)
        return 0;

    t = now;
    if (t < 0)
        t = 0;
    if (a->repeat) {
        t %= a->total_ms;
    } else if (t >= a->total_ms) {
        *into = a->v[a->n - 1].ms;
        return a->n - 1;
    }

    acc = 0;
    for (i = 0; i < a->n; i++) {
        if (t < acc + a->v[i].ms) {
            *into = t - acc;
            return i;
        }
        acc += a->v[i].ms;
    }
    return a->n - 1;
}

/* Resolve an animated name to the pixels showing right now.
 *
 * The frames are fetched through mr_image_get itself, so each one is an
 * ordinary cache entry with its own LRU slot -- no second cache, and no
 * special case in make_room. It also means the pinning works: the frame
 * fetched first cannot be evicted by the fetch of the second, which is the
 * whole reason mr_image_begin_frame exists.
 *
 * Worth knowing on the target: a full-screen animation's frames do not all
 * fit in MR_CACHE_BUDGET at once (five 1280x720 frames are 18MB against a
 * 12MB budget), so a long loop re-decodes a frame per step and evicts whatever
 * else is on the stage to do it. The two frames a cross-fade needs do fit. */
/* Everything about what an animation is showing at `now`, in one place: the
 * frame index, and the cross-fade step 0..255 or -1 when that frame is on
 * screen by itself.
 *
 * One function because two callers need the answer and they must agree
 * exactly. anim_get produces the pixels; mr_image_anim_changed decides
 * whether the frame loop needs to draw at all. If those two ever disagreed
 * the failure is silent and nasty in both directions -- a redraw that never
 * comes freezes the animation, and one that always comes gives back the
 * saving while looking correct. */
static void anim_state_at(const mr_anim *a, long now, int *frame, int *step)
{
    long into;
    int  f, prev;

    f = anim_frame_at(a, now, &into);
    *frame = f;
    *step = -1;

    if (a->v[f].trans_ms <= 0 || into >= a->v[f].trans_ms)
        return;

    prev = f > 0 ? f - 1 : (a->repeat ? a->n - 1 : 0);
    if (prev == f)
        return;

    *step = (int)(into * 255L / a->v[f].trans_ms);
}

int mr_image_anim_changed(const char *name)
{
    int idx = find_index(name);
    int f, step;

    if (idx < 0 || !g_img[idx].anim)
        return 0;
    if (!g_img[idx].drawn_valid)
        return 1;

    anim_state_at(g_img[idx].anim, g_now, &f, &step);
    return f != g_img[idx].drawn_frame || step != g_img[idx].drawn_step;
}

static mr_surface *anim_get(int idx)
{
    const mr_anim *a = g_img[idx].anim;
    mr_surface *cur, *pv;
    int  f, step, prev;

    if (!g_now_set) {
        static int said;
        if (!said) {
            said = 1;
            mr_log("image: animation '%s' resolved before mr_image_set_time; "
                   "nothing is driving the clock, so it will not advance",
                   g_img[idx].name);
        }
    }

    anim_state_at(a, g_now, &f, &step);

    /* Recorded before any of the early returns below, not after the blend:
     * every path out of here has handed out *something*, and the frame loop
     * compares against this to decide whether to come back. A path that
     * returned without recording would leave a stale answer and the
     * animation would stop being redrawn. */
    g_img[idx].drawn_frame = f;
    g_img[idx].drawn_step = step;
    g_img[idx].drawn_valid = 1;

    cur = mr_image_get(a->v[f].file);
    if (!cur || step < 0)
        return cur;

    /* The frame this one is fading from. In a repeating animation the frame
     * before the first is the last, which is right from the second cycle on;
     * on the very first cycle Ren'Py has no previous child and so no
     * transition (atl.py:1348, `old_child is not None`). We cannot tell the
     * cycles apart from a clock alone, so frame 0 of a loop cross-fades in
     * from the final frame instead of appearing. In practice a `show` of an
     * animation comes with its own scene transition over the top of exactly
     * that moment, so it is not a thing you can see -- but it is a real
     * divergence and this is where it lives. anim_state_at already decided
     * there is a fade, so prev differs from f by construction. */
    prev = f > 0 ? f - 1 : (a->repeat ? a->n - 1 : 0);

    pv = mr_image_get(a->v[prev].file);
    if (!pv)
        return cur;

    if (g_img[idx].blend &&
        (g_img[idx].blend->w != cur->w || g_img[idx].blend->h != cur->h)) {
        mr_surface_free(g_img[idx].blend);
        g_img[idx].blend = NULL;
    }
    if (!g_img[idx].blend) {
        g_img[idx].blend = mr_surface_new(cur->w, cur->h);
        if (!g_img[idx].blend)
            return cur;             /* no scratch: cut rather than fail */
    }

    /* The blit path reads has_alpha to decide whether it can memcpy, and
     * mr_surface_new leaves it 0. Without this the carefully alpha-preserving
     * blend above gets drawn as an opaque rectangle, which undoes the whole
     * point of it one level up -- and is invisible to a test that reads the
     * returned surface's pixels instead of compositing it. Set on every call,
     * not just on allocation: it depends on which two frames are in play. */
    g_img[idx].blend->has_alpha = (cur->has_alpha || pv->has_alpha);

    mr_lerp_surface_a(g_img[idx].blend, pv, cur, step);
    return g_img[idx].blend;
}

mr_surface *mr_image_get(const char *name)
{
    int idx;
    mr_surface *s;
    long bytes;

    idx = find_index(name);
    if (idx < 0) {
        /* Screens refer to art by filename ("black.png") while the registry
         * keys on the bare name, so retry without the extension. */
        const char *dot = strrchr(name, '.');
        if (dot && dot > name) {
            char bare[64];
            int n = (int)(dot - name);
            if (n > (int)sizeof(bare) - 1)
                n = (int)sizeof(bare) - 1;
            memcpy(bare, name, (size_t)n);
            bare[n] = '\0';
            idx = find_index(bare);
        }
    }
    if (idx < 0 && strchr(name, '/')) {
        /* A name that looks like a path is loaded straight from the data,
         * which is how screens refer to their art (gui/overlay/...). It is
         * registered so the cache and the LRU handle it like any other. */
        if (mr_image_define(name, name, 0))
            idx = find_index(name);
    }
    if (idx < 0) {
        mr_log("image: unknown image '%s'", name);
        return NULL;
    }

    /* An animated name has no pixels of its own: it stands for whichever of
     * its frames the clock says, and that frame is fetched as an image in its
     * own right. The guard is against a frame naming the animation it belongs
     * to, which the parser cannot currently produce -- frame names are turned
     * into paths and a path never resolves to an animated name -- but which
     * would otherwise recurse until the stack went. */
    if (g_img[idx].anim) {
        static int depth;
        mr_surface *s2;

        if (depth) {
            mr_log("image: animation '%s' contains itself", name);
            return NULL;
        }
        depth = 1;
        s2 = anim_get(idx);
        depth = 0;
        return s2;
    }

    if (g_img[idx].surf) {
        /* Cache hit: promote to the head of the LRU list and pin it for the
         * rest of this frame. */
        lru_unlink(idx);
        lru_push_front(idx);
        g_img[idx].pin_gen = g_gen;
        return g_img[idx].surf;
    }

    if (g_prof) {
        unsigned int t0 = mr_plat_ticks();
        s = mr_png_load(g_img[idx].file);
        g_prof_decode += (long)(mr_plat_ticks() - t0);
        g_prof_loads++;
    } else {
        s = mr_png_load(g_img[idx].file);
    }
    if (!s) {
        mr_log("image: failed to decode '%s' (%s)", name, g_img[idx].file);
        return NULL;
    }

    if (g_img[idx].flip)
        flip_h(s);

    /* im.Blur(im.Flip(...)) is the only nesting the scripts use in this
     * order, and the manipulators are applied in that order here. The blur
     * is timed into the decode total on purpose: it is part of what a cache
     * miss on a blurred name costs, and MR_PROF should say so. */
    if (g_img[idx].xrad > 0.0f || g_img[idx].yrad > 0.0f) {
        if (g_prof) {
            unsigned int t0 = mr_plat_ticks();
            mr_surface_blur(s, g_img[idx].xrad, g_img[idx].yrad);
            g_prof_decode += (long)(mr_plat_ticks() - t0);
        } else {
            mr_surface_blur(s, g_img[idx].xrad, g_img[idx].yrad);
        }
    }

    /* im.FactorScale, last of the three, and once rather than per frame --
     * which is the difference between this and the xzoom transform property.
     * Ren'Py truncates the size (`int(width * self.width)`, im.py:1334-1335)
     * where the transform path rounds, so this does too.
     *
     * Timed into the decode total with the blur, for the same reason: it is
     * part of what a miss on this name costs.
     *
     * The order of the three -- flip, blur, scale -- is the order the scripts
     * nest them in and not something read off the declaration. A declaration
     * that nested them the other way round would be applied in this order
     * anyway; see the note on im.Blur above, which already had this
     * limitation. */
    if (g_img[idx].xscale > 0.0f && g_img[idx].yscale > 0.0f &&
        (g_img[idx].xscale != 1.0f || g_img[idx].yscale != 1.0f)) {
        int nw = (int)((float)s->w * g_img[idx].xscale);
        int nh = (int)((float)s->h * g_img[idx].yscale);
        mr_surface *z;

        if (nw < 1) nw = 1;
        if (nh < 1) nh = 1;

        if (g_prof) {
            unsigned int t0 = mr_plat_ticks();
            z = mr_surface_zoom(s, nw, nh, 0, 0);
            g_prof_decode += (long)(mr_plat_ticks() - t0);
        } else {
            z = mr_surface_zoom(s, nw, nh, 0, 0);
        }

        if (z) {
            mr_surface_free(s);
            s = z;
        } else {
            mr_log("image: could not scale '%s' to %dx%d", name, nw, nh);
        }
    }

    bytes = (long)s->w * (long)s->h * 4L;

    /* A single image larger than the whole budget is still served, it just
     * never stays resident. */
    make_room(bytes, idx);

    g_img[idx].surf = s;
    g_img[idx].bytes = bytes;
    g_img[idx].pin_gen = g_gen;
    g_lru_bytes += bytes;
    lru_push_front(idx);

    return s;
}

/* --- the zoomed-copy cache ----------------------------------------------- */

/* A zoom has to resample the whole sprite, which at 300x600 is 180,000 pixels
 * of bilinear -- cheap next to a 190ms PNG decode, expensive next to a blit.
 * A parked `xzoom 0.5` would otherwise pay it on every frame of every dissolve
 * and every dialogue repaint, so the result is kept.
 *
 * Separate storage from the LRU above, not another entry in it, for two
 * reasons. The key is a name *and* a size, so one name can have several live
 * at once (the same sprite behind a transition snapshot at two different
 * zooms). And a zoomed copy must not be able to evict the image it was made
 * from, nor the other way round -- the renderer holds both at once.
 *
 * Six slots and 8MB. Six is two more than the most zoomed images a frame has
 * been seen to want (a transition doubles the count); over that it still works
 * and merely re-resamples. The budget is on top of MR_CACHE_BUDGET's 12MB,
 * which the ~50MB figure in NOTES has room for -- an `xzoom 2.0 yzoom 2.0` on
 * a full-stage background is 14MB on its own and is served without staying
 * resident, exactly as an oversized image is above. */
#define MR_ZOOM_SLOTS  6
#define MR_ZOOM_BUDGET (8L * 1024L * 1024L)

typedef struct zoom_slot {
    char        name[64];
    int         w, h;          /* the size asked for, which is the key */
    int         flipx, flipy;
    /* Which frame of an animated name this was resampled from, so that a
     * zoomed animation still animates instead of freezing on whichever frame
     * happened to be showing when it was first drawn. Both 0 for a still
     * image, where they never change. */
    int         frame, step;
    mr_surface *surf;
    long        bytes;
    unsigned    pin_gen;
    unsigned    used;          /* for the LRU, see g_zoom_clock */
} zoom_slot;

static zoom_slot g_zoom[MR_ZOOM_SLOTS];
static long      g_zoom_bytes;
static unsigned  g_zoom_clock;

static void zoom_drop(int i)
{
    mr_surface_free(g_zoom[i].surf);
    g_zoom[i].surf = NULL;
    g_zoom_bytes -= g_zoom[i].bytes;
    g_zoom[i].bytes = 0;
    g_zoom[i].name[0] = '\0';
}

static void zoom_flush(void)
{
    int i;

    for (i = 0; i < MR_ZOOM_SLOTS; i++) {
        if (g_zoom[i].surf)
            zoom_drop(i);
    }
    g_zoom_bytes = 0;
}

mr_surface *mr_image_get_zoom(const char *name, int w, int h,
                              int flipx, int flipy)
{
    mr_surface *src, *out;
    int i, victim = -1, idx, frame = 0, step = 0;
    long bytes;

    if (w <= 0 || h <= 0)
        return NULL;

    /* Fetched before the slot lookup, not after: it is a hash lookup and a
     * pointer on a cache hit, it pins the source for this frame, and for an
     * animated name it is what settles which frame the key refers to. */
    src = mr_image_get(name);
    if (!src)
        return NULL;

    idx = find_index(name);
    if (idx >= 0 && g_img[idx].anim) {
        frame = g_img[idx].drawn_frame;
        step = g_img[idx].drawn_step;
    }

    for (i = 0; i < MR_ZOOM_SLOTS; i++) {
        if (g_zoom[i].surf && g_zoom[i].w == w && g_zoom[i].h == h &&
            g_zoom[i].flipx == flipx && g_zoom[i].flipy == flipy &&
            g_zoom[i].frame == frame && g_zoom[i].step == step &&
            name_eq(g_zoom[i].name, name)) {
            g_zoom[i].used = ++g_zoom_clock;
            g_zoom[i].pin_gen = g_gen;
            return g_zoom[i].surf;
        }
    }

    out = mr_surface_zoom(src, w, h, flipx, flipy);
    if (!out)
        return src;           /* out of memory: unzoomed beats nothing */

    bytes = (long)w * (long)h * 4L;

    /* Take a slot: an empty one, else the least recently used one this frame
     * has not already handed out. If every slot is pinned -- more zoomed
     * images in one frame than there are slots -- the least recently used of
     * those is taken anyway. That is safe because the renderer fetches a
     * surface immediately before its own blit and never holds one across
     * another fetch, which is the same rule mr_image_get's eviction relies on
     * and is spelled out at render_move. */
    for (i = 0; i < MR_ZOOM_SLOTS; i++) {
        if (!g_zoom[i].surf) {
            victim = i;
            break;
        }
    }
    if (victim < 0) {
        for (i = 0; i < MR_ZOOM_SLOTS; i++) {
            if (g_zoom[i].pin_gen == g_gen)
                continue;
            if (victim < 0 || g_zoom[i].used < g_zoom[victim].used)
                victim = i;
        }
    }
    if (victim < 0) {
        for (i = 0; i < MR_ZOOM_SLOTS; i++) {
            if (victim < 0 || g_zoom[i].used < g_zoom[victim].used)
                victim = i;
        }
    }
    if (g_zoom[victim].surf)
        zoom_drop(victim);

    /* Still over budget after taking a slot: drop unpinned slots oldest
     * first. A single copy larger than the budget is served and simply never
     * stays resident, the same deal mr_image_get gives an oversized image. */
    while (g_zoom_bytes + bytes > MR_ZOOM_BUDGET) {
        int oldest = -1;

        for (i = 0; i < MR_ZOOM_SLOTS; i++) {
            if (!g_zoom[i].surf || i == victim || g_zoom[i].pin_gen == g_gen)
                continue;
            if (oldest < 0 || g_zoom[i].used < g_zoom[oldest].used)
                oldest = i;
        }
        if (oldest < 0)
            break;
        zoom_drop(oldest);
    }

    strncpy(g_zoom[victim].name, name, sizeof(g_zoom[victim].name) - 1);
    g_zoom[victim].name[sizeof(g_zoom[victim].name) - 1] = '\0';
    g_zoom[victim].w = w;
    g_zoom[victim].h = h;
    g_zoom[victim].flipx = flipx;
    g_zoom[victim].flipy = flipy;
    g_zoom[victim].frame = frame;
    g_zoom[victim].step = step;
    g_zoom[victim].surf = out;
    g_zoom[victim].bytes = bytes;
    g_zoom[victim].used = ++g_zoom_clock;
    g_zoom[victim].pin_gen = g_gen;
    g_zoom_bytes += bytes;

    return out;
}

void mr_image_flush(void)
{
    int i;

    zoom_flush();

    while (g_lru_head >= 0)
        evict(g_lru_head);

    /* A cross-fade scratch is not in the LRU, so the loop above does not
     * reach it. It is still decoded pixels as far as a caller asking to free
     * them is concerned. */
    for (i = 0; i < g_nimg; i++) {
        if (g_img[i].blend) {
            mr_surface_free(g_img[i].blend);
            g_img[i].blend = NULL;
        }
    }
}

void mr_image_stats_get(mr_image_stats *st)
{
    int i;

    memset(st, 0, sizeof(*st));
    st->pixels_budget = MR_CACHE_BUDGET;
    st->zoom_slots = MR_ZOOM_SLOTS;
    st->zoom_budget = MR_ZOOM_BUDGET;

    /* Walking the table rather than reporting g_lru_bytes alone: the two
     * agreeing is itself worth seeing, since a leak in the cache shows up as
     * exactly that disagreement. */
    for (i = 0; i < g_nimg; i++) {
        if (g_img[i].surf) {
            st->pixels_bytes += g_img[i].bytes;
            st->pixels_entries++;
        }
        if (g_img[i].blend) {
            st->blend_bytes += (long)g_img[i].blend->w *
                               (long)g_img[i].blend->h * 4L;
            st->blend_entries++;
        }
    }

    for (i = 0; i < MR_ZOOM_SLOTS; i++) {
        if (g_zoom[i].surf) {
            st->zoom_bytes += g_zoom[i].bytes;
            st->zoom_entries++;
        }
    }
}

void mr_image_shutdown(void)
{
    int i;

    mr_image_flush();
    for (i = 0; i < g_nimg; i++) {
        free(g_img[i].anim);
        g_img[i].anim = NULL;
    }
    g_nimg = 0;
}

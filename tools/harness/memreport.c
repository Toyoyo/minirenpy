/* Where the memory goes, and whether any of it is wasted.
 *
 * Not a pass/fail test -- a report. It boots the engine on the real game,
 * draws the main menu, loads several saves in turn and prints live heap at
 * each step with the biggest holders attributed to their call site, so the
 * shape of the growth can be compared against what was observed on the
 * target: 32MB at the main menu, 41MB after one save, 48MB after a second,
 * then flat however many more are loaded.
 *
 * Every allocation the engine makes is intercepted (-Wl,--wrap, see run.sh).
 * Two caveats, both of which make this a lower bound:
 *
 *   - libpng and zlib are shared libraries on the host, so their internal
 *     allocations are invisible here. On the target they are static and are
 *     real engine memory. Measured since: a massif run over the same walk
 *     puts the total peak at 31.50MB against this report's 31.58MB and shows
 *     no libpng or zlib allocation above its 1% threshold at all -- a row
 *     buffer and an inflate window per decode, freed immediately. So this
 *     caveat is real but worth well under a megabyte, not the open-ended
 *     unknown it reads as.
 *   - the static BSS -- 7.29MB in the current build -- is not heap and does
 *     not appear below. `mrpy.map` gives the segment total; the per-symbol
 *     breakdown needs `wdis` on the object files, because every one of those
 *     arrays is file-scope static and wlink lists only externals. NOTES.md
 *     has the one-liner.
 *
 * The figures to expect are a stage-sized surface at 3686400 bytes each --
 * 1280x720x4 -- of which the engine holds three for the whole session, and
 * the image cache, which is bounded by MR_CACHE_BUDGET.
 *
 * It also prints cache occupancy at each step, which is a different question
 * from where the heap went: a cache under its budget never evicts, so it
 * accumulates every distinct image the session has shown and never gives any
 * back. That, not any per-load allocation, is why live memory steps up when a
 * save is loaded into a scene the run has not visited -- the loads below add
 * 0.00 MB each precisely because the walk-through already decoded both
 * scenes.
 *
 * What it does not visit: the save/load screen and the preferences screen. So
 * the thumbnail cache reads 0.00 MB, and the style and layout caches read
 * well below their real working set -- the preferences screen is what drives
 * those, at 3950 property lookups and 53 measured nodes a frame. Those three
 * numbers are floors, not measurements. Driving the game menu from here would
 * fix it and has not been done.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <execinfo.h>

#include "mr.h"
#include "vfs.h"
#include "image.h"
#include "scene.h"
#include "interp.h"
#include "script.h"
#include "ui.h"
#include "bandcache.h"
#include "save.h"
#include "expr.h"
#include "slrender.h"
#include "style.h"
#include "list.h"
#include "audio.h"

/* --- the usual stubs ------------------------------------------------------ */

static mr_surface *g_st;

mr_surface *mr_stage(void) { return g_st; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); exit(1); }
void mr_log(const char *f, ...) { (void)f; }
void mr_quit(void) {}
void mr_invalidate(void) {}
void (*hostshim_wave_sink)(const short *, int) = NULL;

/* --- allocation tracking -------------------------------------------------- */

void *__real_malloc(size_t n);
void *__real_calloc(size_t a, size_t b);
void *__real_realloc(void *p, size_t n);
void  __real_free(void *p);

#define SITES 512
#define LIVE  200000

/* Two frames, not one: every surface in the engine comes from
 * mr_surface_new, so attributing to the immediate caller lumps the three
 * session-long stage buffers in with every cached image. The grandparent is
 * what separates mr_scene_init from mr_image_get. */
typedef struct { void *pc; void *up; long bytes; long count; } site;

static site g_site[SITES];
static int  g_nsite;

static struct { void *p; long n; int site; } g_live[LIVE];
static int  g_nlive;

static long g_live_bytes, g_peak_bytes;
static int  g_tracking;
/* Peak context. `g_ready` gates the engine queries: note() runs from the
 * very first allocation, before mr_scene_init and mr_image_init have been
 * called, and asking them anything then reads uninitialised statics. */
static int  g_ready, g_peak_in_transition;
static long g_peak_cache, g_peak_trans, g_peak_quiet;

static int site_for(void *pc, void *up)
{
    int i;
    for (i = 0; i < g_nsite; i++)
        if (g_site[i].pc == pc && g_site[i].up == up)
            return i;
    if (g_nsite >= SITES)
        return 0;
    g_site[g_nsite].pc = pc;
    g_site[g_nsite].up = up;
    return g_nsite++;
}

/* The engine function that asked for the memory, and the one that called it.
 * The frames above are this helper, note(), and the __wrap_ entry point, so
 * the asker is the fourth. */
static void caller(void **pc, void **up)
{
    void *bt[8];
    int n = backtrace(bt, 8);
    *pc = n >= 4 ? bt[3] : (void *)0;
    *up = n >= 5 ? bt[4] : (void *)0;
}

static void note(void *p, long n)
{
    int s;

    if (!p || !g_tracking)
        return;
    {
        void *pc, *up;
        caller(&pc, &up);
        s = site_for(pc, up);
    }
    g_site[s].bytes += n;
    g_site[s].count++;

    if (g_nlive < LIVE) {
        g_live[g_nlive].p = p;
        g_live[g_nlive].n = n;
        g_live[g_nlive].site = s;
        g_nlive++;
    }
    g_live_bytes += n;
    if (g_live_bytes > g_peak_bytes) {
        g_peak_bytes = g_live_bytes;
        /* What the engine was doing at the high water mark, which is a
         * different question from how much it was holding. It decides
         * whether making the two transition snapshots lazy would lower the
         * peak or only the idle baseline: if the peak lands inside a
         * transition, the snapshots are live exactly when everything else is
         * at its worst and freeing them between transitions buys nothing
         * where it counts. */
        g_peak_in_transition = g_ready ? mr_scene_in_transition() : 0;
        if (g_ready) {
            mr_image_stats st;
            mr_image_stats_get(&st);
            g_peak_cache = st.pixels_bytes;
        }
    }

    /* The two high water marks separately, because "would freeing the
     * snapshots between transitions lower the peak" is answered by
     * comparing them, not by the overall peak. Lazy snapshots leave the
     * in-transition total exactly as it is and take 7.04MB off every other
     * moment, so the new peak would be the larger of the in-transition total
     * and the rest-minus-7.04. If a transition is already the worst moment,
     * the work buys nothing. */
    if (g_ready) {
        if (mr_scene_in_transition()) {
            if (g_live_bytes > g_peak_trans)
                g_peak_trans = g_live_bytes;
        } else if (g_live_bytes > g_peak_quiet) {
            g_peak_quiet = g_live_bytes;
        }
    }
}

static void forget(void *p)
{
    int i;

    if (!p)
        return;
    for (i = g_nlive - 1; i >= 0; i--) {
        if (g_live[i].p == p) {
            g_site[g_live[i].site].bytes -= g_live[i].n;
            g_live_bytes -= g_live[i].n;
            g_live[i] = g_live[--g_nlive];
            return;
        }
    }
}

void *__wrap_malloc(size_t n)
{
    void *p = __real_malloc(n);
    note(p, (long)n);
    return p;
}

void *__wrap_calloc(size_t a, size_t b)
{
    void *p = __real_calloc(a, b);
    note(p, (long)(a * b));
    return p;
}

void *__wrap_realloc(void *p, size_t n)
{
    void *q;
    forget(p);
    q = __real_realloc(p, n);
    note(q, (long)n);
    return q;
}

void __wrap_free(void *p)
{
    forget(p);
    __real_free(p);
}

/* --- reporting ------------------------------------------------------------ */

/* Resident size of the whole process, which is a different number from the
 * tracked heap and the reason this report and a figure read off the target
 * do not match.
 *
 * Everything above counts bytes the engine asked malloc for. A process-level
 * tool counts those *plus* whatever the allocator committed and has not
 * returned, the code, the static data, the C runtime, and here libpng, zlib
 * and libc. The gap is not an error in either figure; it is two different
 * questions, and comparing them directly is what makes memory accounting
 * come out 15MB short.
 *
 * glibc is not Watcom's clib, so the gap measured here does not transfer to
 * the target as a number -- only as an order of magnitude for how much sits
 * above the heap. */
static long rss_bytes(void)
{
    FILE *f = fopen("/proc/self/statm", "r");
    long total = 0, resident = 0;

    if (!f)
        return 0;
    if (fscanf(f, "%ld %ld", &total, &resident) != 2)
        resident = 0;
    fclose(f);
    return resident * 4096L;        /* statm is in pages */
}

static long g_mark;

static void report(const char *phase)
{
    printf("\n== %-28s live %8.2f MB   (%+.2f MB)   peak %.2f MB\n",
           phase, g_live_bytes / 1048576.0,
           (g_live_bytes - g_mark) / 1048576.0, g_peak_bytes / 1048576.0);
    g_mark = g_live_bytes;
}

/* Through the struct, not by offset: this read ((long *)a)[1] until `up` was
 * added in front of `bytes` and silently started sorting on a pointer. */
static int by_bytes(const void *a, const void *b)
{
    long x = ((const site *)a)->bytes, y = ((const site *)b)->bytes;
    return x < y ? 1 : (x > y ? -1 : 0);
}

/* "mr_surface_new (via mr_image_get)" -- the second name is what makes the
 * line useful. backtrace_symbols wants an array, so it gets one. */
static void print_site(const site *s, long bytes, long count)
{
    void *pcs[2];
    char **nm;
    const char *a, *b;

    pcs[0] = s->pc;
    pcs[1] = s->up;
    nm = backtrace_symbols(pcs, 2);
    a = nm ? nm[0] : "?";
    b = nm ? nm[1] : "?";
    if (strchr(a, '(')) a = strchr(a, '(');
    if (strchr(b, '(')) b = strchr(b, '(');

    if (count >= 0)
        printf("   %8.2f MB %7ld  %s via %s\n", bytes / 1048576.0, count, a, b);
    else
        printf("   %8.2f MB          %s via %s\n", bytes / 1048576.0, a, b);
    free(nm);
}

/* Every live block over `min` bytes, largest first. The big ones are few
 * enough to name individually, and that is what distinguishes "three
 * full-stage surfaces held for the session" from "a cache that grew". */
static void big_blocks(long min)
{
    static site big[LIVE];               /* reusing site: pc + bytes */
    int i, n = 0;
    long shown = 0;

    for (i = 0; i < g_nlive; i++) {
        if (g_live[i].n < min || n >= LIVE)
            continue;
        big[n].pc = g_site[g_live[i].site].pc;
        big[n].up = g_site[g_live[i].site].up;
        big[n].bytes = g_live[i].n;
        big[n].count = 1;
        n++;
    }
    qsort(big, (size_t)n, sizeof(big[0]), by_bytes);

    printf("   individual blocks over %ldKB:\n", min / 1024);
    for (i = 0; i < n; i++) {
        shown += big[i].bytes;
        if (i < 24)
            print_site(&big[i], big[i].bytes, -1);
    }
    printf("   (%.2f MB in %d such blocks)\n", shown / 1048576.0, n);
}

static void sites(int top)
{
    site s[SITES];
    int i, n = 0;

    for (i = 0; i < g_nsite; i++)
        if (g_site[i].bytes > 0)
            s[n++] = g_site[i];
    qsort(s, (size_t)n, sizeof(s[0]), by_bytes);

    printf("   %-10s %7s  %s\n", "live", "allocs", "call site");
    for (i = 0; i < n && i < top; i++)
        print_site(&s[i], s[i].bytes, s[i].count);
}

/* --- cache occupancy ------------------------------------------------------
 *
 * The allocation report above says where the heap went; this says how much of
 * it is a cache and how full that cache is. The distinction decides whether
 * flushing on a save load would save anything: a cache *at* its budget is
 * already evicting, so the outgoing context's pixels are being reclaimed by
 * the incoming one and a flush changes only when. A cache *under* its budget
 * never evicts at all -- make_room's loop is gated on exceeding the budget --
 * so everything decoded for a context the player has left stays resident for
 * the rest of the run, and a flush is the only thing that frees it.
 *
 * The percentage is the number to read. Nothing else here distinguishes the
 * two cases.
 */
static void caches(void)
{
    mr_image_stats im;
    long thumb, shot;
    int nthumb;

    mr_image_stats_get(&im);
    mr_save_cache_stats(&thumb, &nthumb, &shot);

    printf("   caches: images %6.2f/%ld MB (%3ld%%, %d)   "
           "zoom %5.2f/%ld MB (%3ld%%, %d/%d)\n",
           im.pixels_bytes / 1048576.0, im.pixels_budget / 1048576L,
           im.pixels_budget ? im.pixels_bytes * 100L / im.pixels_budget : 0L,
           im.pixels_entries,
           im.zoom_bytes / 1048576.0, im.zoom_budget / 1048576L,
           im.zoom_budget ? im.zoom_bytes * 100L / im.zoom_budget : 0L,
           im.zoom_entries, im.zoom_slots);
    printf("           blend scratch %.2f MB (%d)   thumbnails %.2f MB (%d)"
           "   screenshot %.2f MB\n",
           im.blend_bytes / 1048576.0, im.blend_entries,
           thumb / 1048576.0, nthumb, shot / 1048576.0);

    /* The dialogue band strip (bandcache.h). Reported two ways on purpose:
     * what it is holding right now, which is 0 unless this walk happened to
     * stop on a line of dialogue, and what it *would* cost for this game --
     * which is the figure worth knowing, because it is a function of
     * gui.textbox_height and so is per-game rather than a constant. */
    {
        int bx, by, bw, bh;

        if (mr_ui_dialogue_rect(&bx, &by, &bw, &bh))
            printf("           band strip %.2f MB held, %.2f MB for this"
                   " game's %dpx window\n",
                   mr_band_cache_bytes() / 1048576.0,
                   (double)MR_STAGE_W * bh * 4 / 1048576.0, bh);
        else
            printf("           band strip %.2f MB held (no dialogue window"
                   " up to price one against)\n",
                   mr_band_cache_bytes() / 1048576.0);
    }
}

/* --- driving the engine --------------------------------------------------- */

static long g_t;

static void step(int n)
{
    int i;
    for (i = 0; i < n; i++) {
        g_t += 33;
        mr_interp_update(g_t);
        mr_sl_dump_next();           /* keep the dump buffer from accruing */
        mr_interp_render(g_st, g_t);
        mr_ui_draw(g_st);
        mr_interp_render_overlay(g_st);
    }
}

/* --- the soak -------------------------------------------------------------
 *
 * Walk forward through the script, drawing as we go so that the images each
 * statement shows are actually decoded -- advancing without rendering moves
 * the program counter and fills no cache, which would make the whole report
 * read as though the game used no memory.
 *
 * Choices are taken in rotation rather than always the first, so a long run
 * covers more than one branch. That makes the walk non-exhaustive by nature:
 * this is a memory report, not a coverage tool, and the figure it is after is
 * the high water mark of a session that has been everywhere it can get to.
 *
 * It stops early, and says so, if the program counter stops moving. The
 * rhythm minigame wants input this cannot synthesise, and a report that
 * silently spun for ten minutes at one statement would read as a completed
 * full-script run.
 */
#define SOAK_SLOTS 6

static long g_hi_images, g_hi_zoom;
static int  g_hi_entries, g_soak_menus, g_soak_renders;
static mr_script *g_script;      /* for naming the statement a stall is on */

static void soak_note_cache(void)
{
    mr_image_stats im;

    mr_image_stats_get(&im);
    if (im.pixels_bytes > g_hi_images) {
        g_hi_images = im.pixels_bytes;
        g_hi_entries = im.pixels_entries;
    }
    if (im.zoom_bytes > g_hi_zoom)
        g_hi_zoom = im.zoom_bytes;
}

static long soak(long budget, long every, char slots[SOAK_SLOTS][MR_SLOT_NAME],
                 int *nwritten)
{
    long steps = 0, last_pc = -1, stuck = 0;

    while (steps < budget && !mr_interp_finished()) {
        const char *texts[32];
        mr_gamestate g;
        int count = 0;

        g_t += 33;
        mr_interp_update(g_t);

        /* Only composite when something outside the dialogue box changed,
         * which is what main.c does. Rendering every line instead costs
         * three times the run time and decodes nothing extra: a line typing
         * itself out touches no image. MR_REDRAW_DIALOGUE promises exactly
         * that. */
        if (mr_interp_redraw_kind() == MR_REDRAW_ALL) {
            mr_interp_render(g_st, g_t);
            mr_ui_draw(g_st);
            mr_interp_render_overlay(g_st);
            g_soak_renders++;
        }
        mr_sl_dump_next();           /* keep the dump buffer from accruing */
        soak_note_cache();

        if (mr_interp_menu(&count, texts, 32) && count > 0) {
            int row = g_soak_menus++ % count;
            int tries = 0;

            while (!mr_interp_menu_sensitive(row) && tries++ < count)
                row = (row + 1) % count;
            mr_interp_choose(row);
        } else {
            mr_interp_advance();
        }

        mr_interp_capture(&g);
        if (g.pc == last_pc) {
            if (++stuck > 3000) {
                /* Naming the statement, not just the index. "stuck at pc
                 * 6266" says nothing about whether the walk hit a wait it
                 * should have clicked through or something it genuinely
                 * cannot drive, and that is the difference between a bug
                 * here and a limit of the approach. */
                const mr_stmt *st = (g.pc >= 0 && g.pc < g_script->n)
                                    ? &g_script->st[g.pc] : NULL;

                /* The common case is not a stall at all: Miwa's last
                 * statement is a `return`, which drops back to the main
                 * menu and sits there, and mr_interp_finished() does not
                 * become true for it. Calling that "stuck" made a complete
                 * playthrough read as a broken walk. */
                if (st && st->kind == MR_ST_RETURN &&
                    mr_var_get_int("main_menu", 0)) {
                    printf("\n   (reached the end of the script after %ld "
                           "steps and returned to the main menu)\n", steps);
                    break;
                }

                printf("\n   (pc %ld has not moved in 3000 steps, stopping "
                       "after %ld)\n", g.pc, steps);
                if (st)
                    printf("    it is statement kind %d at %s:%d%s%s, "
                           "waiting=%d\n", (int)st->kind,
                           (st->file >= 0 && st->file < g_script->nfiles)
                               ? g_script->files[st->file] : "?",
                           st->line, st->name[0] ? ", name " : "",
                           st->name[0] ? st->name : "",
                           mr_interp_waiting());
                break;
            }
        } else {
            stuck = 0;
            last_pc = g.pc;
        }

        steps++;
        if (every > 0 && steps % every == 0 && *nwritten < SOAK_SLOTS) {
            sprintf(slots[*nwritten], "7-%d", *nwritten + 1);
            if (mr_save_write(slots[*nwritten], g_st))
                (*nwritten)++;
        }
    }
    return steps;
}

int main(int argc, char **argv)
{
    mr_script *s;
    /* Its own saves, on page 7 so they cannot collide with a real one or with
     * another harness (seamtest uses page 8, langtest page 9).
     *
     * It used to load 1-1 to 1-3 and print "(no save in slot 1-1 --
     * skipped)" for each, which is what happens in any directory that has no
     * Miwa saves in it -- i.e. almost always, since the harnesses run in a
     * scratch directory. The half of the report that matters was quietly
     * absent, and the load figures in the header comment could not be
     * reproduced. It writes its own now, spread through the walk. */
    char slots[SOAK_SLOTS][MR_SLOT_NAME];
    int  nslots = 0;
    long budget, every, walked;
    int i;

    if (argc < 3) {
        fprintf(stderr,
                "usage: memreport <pak> <game-dir> [statements|full]\n"
                "  statements  how far to walk the script (default 400)\n"
                "  full        to the end, or until the pc stops moving\n");
        return 2;
    }
    setbuf(stdout, NULL);

    /* Default is short so that a suite run stays quick. `full` is the one
     * that answers "what does a whole playthrough cost", and takes minutes
     * rather than seconds. */
    budget = 400;
    if (argc >= 4) {
        budget = strcmp(argv[3], "full") ? atol(argv[3]) : 1000000L;
        if (budget < 1)
            budget = 400;
    }

    g_tracking = 1;

    mr_vfs_init(argv[1], argv[2]);
    mr_image_init(argv[2]);
    mr_scene_init();
    report("vfs + images + scene");
    sites(6);

    s = mr_script_load(argv[2]);
    g_script = s;
    mr_interp_init(s, argv[2]);
    report("script parsed");
    sites(6);

    mr_ui_init(argv[2]);
    mr_audio_init(argv[2]);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);
    /* `label start` is Ren'Py's entry point, not this game's invention, so
     * this much is not pinned to one script. */
    g_ready = 1;
    mr_interp_start("start");
    step(10);
    report("script start drawn");
    sites(10);
    caches();

    /* The saves have to be spread through the walk, not bunched: 30
     * statements apart they shared a background and most sprites, so loading
     * either added 0.00 MB and the report appeared to show that a load costs
     * nothing -- true only of a load back into the scene already on the
     * stage, which is not the case anyone is worried about.
     *
     * `every` is an estimate, since how far the walk gets is not known until
     * it has got there. Falling short of SOAK_SLOTS saves is fine and says
     * so below.
     *
     * Bounded by the script's own length, not by the budget alone: in `full`
     * mode the budget is a sentinel of a million, so budget/7 was an
     * interval of 142857 and the full run wrote *zero* saves -- the half of
     * the report the whole exercise is about, silently absent, exactly the
     * failure the page-7 rewrite was supposed to have fixed. The statement
     * count is the honest upper bound on how far a walk can get. */
    every = budget / (SOAK_SLOTS + 1);
    if (every > s->n / (SOAK_SLOTS + 1))
        every = s->n / (SOAK_SLOTS + 1);
    if (every < 1)
        every = 1;

    walked = soak(budget, every, slots, &nslots);
    printf("\n   walked %ld statements of %d in the script, %d composites, "
           "%d choices taken, %d saves written\n",
           walked, s->n, g_soak_renders, g_soak_menus, nslots);
    report("walk finished");
    sites(10);
    caches();

    mr_save_flush();

    /* Each save in turn, then the first ones again: the repeat is what shows
     * whether a load adds memory or merely re-uses what the walk already
     * decoded. */
    for (i = 0; i < nslots * 2; i++) {
        const char *slot = slots[i % nslots];
        char label[64];

        if (!mr_save_load(slot)) {
            printf("\n(slot %s would not load -- skipped)\n", slot);
            continue;
        }
        step(20);
        sprintf(label, "loaded %s%s", slot, i >= nslots ? " again" : "");
        report(label);
        caches();
    }
    if (nslots)
        sites(10);

    /* The save screen, which is where the list pool and the thumbnail cache
     * actually get worked.
     *
     * Without this the report said "2 lists created" and "thumbnails 0.00
     * MB", and both were taken for the engine's real behaviour rather than
     * for an artefact of a walk that never opens a menu. The save screen
     * builds a list per `for` per draw -- `range(cols*rows)` and
     * `range(1,10)` in both games' templates -- so it cycles the 32-slot
     * pool in a handful of frames, and it decodes a thumbnail per occupied
     * slot. Twenty draws is enough to reach the steady state of both. */
    mr_interp_show_screen("save");
    step(20);
    report("save screen drawn");
    caches();
    {
        long created, slot;
        int reached, pool;

        mr_list_stats(&created, &reached, &pool, &slot);
        printf("   list pool: %d/%d slots reached, %ld lists created, "
               "%.2f MB of %.2f MB touched\n",
               reached, pool, created, (double)reached * slot / 1048576.0,
               (double)pool * slot / 1048576.0);
    }

    /* The flush experiment: what a flush on load would actually buy.
     *
     * Freeing is the easy half. The half that decides it is what the very
     * next composite asks straight back -- the textbox, the button frames,
     * the sprites still on the stage -- because those are decoded again
     * immediately and cost a PNG decode each for no lasting saving. The
     * difference is the only figure worth quoting. */
    {
        long before = g_live_bytes, freed, back;

        mr_image_flush();
        freed = before - g_live_bytes;
        report("after mr_image_flush");
        caches();

        step(1);
        back = g_live_bytes - (before - freed);
        report("one frame redrawn");
        caches();

        printf("\n   flush freed %.2f MB, the next frame took back %.2f MB, "
               "%.2f MB still free\n",
               freed / 1048576.0, back / 1048576.0,
               (freed - back) / 1048576.0);

        /* And then keep playing, which is the part that decides it.
         *
         * Calling the figure above a "saving" was wrong, and the only reason
         * it looked like one is that the first run of this report walked 200
         * statements and never filled the cache. At 400 it saturates, and a
         * flush against a saturated cache is a loan: the next few scenes
         * decode straight back into the space. The question is not what a
         * flush frees, it is whether the memory stays free. */
        {
            long after = g_live_bytes;

            soak(60, 0, slots, &nslots);
            report("60 statements after the flush");
            caches();
            printf("   the cache took back %.2f MB of the %.2f MB in 60 "
                   "statements; %.2f MB of the flush survives\n",
                   (g_live_bytes - after) / 1048576.0,
                   freed / 1048576.0,
                   (before - g_live_bytes) / 1048576.0);
        }
    }

    for (i = 0; i < nslots; i++)
        mr_save_delete(slots[i]);

    printf("\n--- summary -----------------------------------------------\n");
    printf("statements walked  %8ld  of %d\n", walked, s->n);
    printf("heap live at end   %8.2f MB\n", g_live_bytes / 1048576.0);
    printf("heap peak          %8.2f MB\n", g_peak_bytes / 1048576.0);
    printf("  at the peak:     %s, image cache %.2f MB\n",
           g_peak_in_transition ? "a transition was running"
                                : "no transition running",
           g_peak_cache / 1048576.0);
    {
        long snaps = 2L * MR_STAGE_W * MR_STAGE_H * 4L;
        long lazy = g_peak_quiet - snaps;

        if (lazy < g_peak_trans)
            lazy = g_peak_trans;
        printf("peak in transition %8.2f MB\n", g_peak_trans / 1048576.0);
        printf("peak outside one   %8.2f MB\n", g_peak_quiet / 1048576.0);
        printf("  so lazily allocating g_from/g_to (%.2f MB) would put the\n"
               "  peak at %.2f MB, a saving of %.2f MB\n",
               snaps / 1048576.0, lazy / 1048576.0,
               (g_peak_bytes - lazy) / 1048576.0);
    }
    printf("live blocks        %8d\n", g_nlive);
    /* The high water mark across the whole walk, which is the figure the
     * budget has to cover -- the per-step lines only sample it. */
    printf("image cache high   %8.2f MB  (%d entries, %ld%% of budget)\n",
           g_hi_images / 1048576.0, g_hi_entries,
           g_hi_images * 100L / MR_CACHE_BUDGET);
    printf("zoom cache high    %8.2f MB\n", g_hi_zoom / 1048576.0);
    printf("image cache budget %8.2f MB\n", MR_CACHE_BUDGET / 1048576.0);
    printf("a stage surface    %8.2f MB  (%dx%dx4)\n",
           MR_STAGE_W * MR_STAGE_H * 4 / 1048576.0, MR_STAGE_W, MR_STAGE_H);
    /* Not a constant -- it grows with every fixed-size array added, and this
     * read 6.42 MB while the build was at 7.29. Re-read it from the Watcom
     * map rather than trusting the number here; NOTES.md has the one-liner
     * that breaks it down per symbol. */
    printf("static BSS (target)    7.29 MB  (mrpy.map, _BSS, "
           "not counted above)\n");

    /* Why this report and a figure read off the target do not add up, laid
     * out rather than left as a 15MB discrepancy for someone to re-derive.
     * The sizes here are what the engine actually allocates at the window
     * size the target's log reports (640x360, 24bpp path). */
    {
        long buf = 640L * 360L * 4L;            /* present.c g_buf  */
        long dib = ((640L * 3 + 3) & ~3L) * 360L; /* present.c g_out */

        printf("\n   what this report structurally cannot see:\n");
        printf("       present.c g_buf  %6.2f MB  window-sized 32bpp "
               "surface\n", buf / 1048576.0);
        printf("       present.c g_out  %6.2f MB  the output DIB\n",
               dib / 1048576.0);
        printf("         (both at 640x360 per the target's log; neither\n"
               "          present.c nor main.c is linked by any harness)\n");
        printf("       save thumbnails  %6.2f MB  12 slots, once the player "
               "browses saves\n", 12.0 * 256 * 144 * 4 / 1048576.0);
        printf("       the minigame and the music room: never entered by "
               "this walk\n");
        printf("       the allocator's committed-but-unused arena, which no\n"
               "          in-process byte count can see\n");
        printf("\n   tracked heap %.2f MB against process RSS %.2f MB: "
               "%.2f MB of this\n   process sits above the heap "
               "(code, libc, libpng, allocator slack).\n"
               "   glibc is not Watcom's clib, so read that as an order of\n"
               "   magnitude, not a figure to subtract from the target's.\n",
               g_live_bytes / 1048576.0, rss_bytes() / 1048576.0,
               (rss_bytes() - g_live_bytes) / 1048576.0);
    }

    /* Static caches. Part of that BSS figure, invisible to the allocation
     * tracking above, and still real memory on the target. Occupancy against
     * capacity is the useful part: the layout cache stops caching when it
     * fills, silently, and this is the only place that would show it. */
    {
        long sb, mb;
        int sn, scap, mhi, mcap;

        mr_style_cache_stats(&sb, &sn, &scap);
        mr_sl_measure_cache_stats(&mb, &mhi, &mcap);
        printf("\n   static caches (inside that BSS, not on the heap):\n");
        printf("       style resolution %6.2f MB  %d/%d slots used\n",
               sb / 1048576.0, sn, scap);
        {
            long created, slot;
            int reached, pool;

            mr_list_stats(&created, &reached, &pool, &slot);
            printf("       list pool        %6.2f MB  %d/%d slots reached, "
                   "%ld lists created\n",
                   (double)slot * pool / 1048576.0, reached, pool, created);
        }
        printf("       screen layout    %6.2f MB  %d/%d high water%s\n",
               mb / 1048576.0, mhi, mcap,
               mhi >= mcap ? "  <-- FULL, it stopped caching" : "");
    }
    printf("\n");
    big_blocks(256L * 1024L);
    return 0;
}

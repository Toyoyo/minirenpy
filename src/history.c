/* history.c - the text history and the rollback log. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mr.h"
#include "history.h"
#include "persist.h"
#include "expr.h"
#include "scene.h"
#include "serial.h"
#include "stmtid.h"

/* --- text history ---------------------------------------------------------
 *
 * A ring, because the oldest entry is dropped once it is full. The history
 * screen reads it newest first.
 */

typedef struct {
    char   who[MR_MAX_NAME];
    char   what[MR_HISTORY_TEXT];
    mr_u32 color;
} hist_entry;

static hist_entry g_hist[MR_HISTORY_MAX];
static int        g_hist_n;      /* how many are filled, up to MAX */
static int        g_hist_head;   /* where the next one goes */

void mr_history_reset(void)
{
    g_hist_n = 0;
    g_hist_head = 0;
}

void mr_history_add(const char *who, const char *what, mr_u32 color)
{
    hist_entry *e = &g_hist[g_hist_head];

    memset(e, 0, sizeof(*e));
    if (who)
        strncpy(e->who, who, sizeof(e->who) - 1);
    if (what)
        strncpy(e->what, what, sizeof(e->what) - 1);
    e->color = color;

    g_hist_head = (g_hist_head + 1) % MR_HISTORY_MAX;
    if (g_hist_n < MR_HISTORY_MAX)
        g_hist_n++;
}

/* Drop the newest `n` entries, for a rollback. */
void mr_history_pop(int n)
{
    while (n-- > 0 && g_hist_n > 0) {
        g_hist_head = (g_hist_head - 1 + MR_HISTORY_MAX) % MR_HISTORY_MAX;
        g_hist_n--;
    }
}

int mr_history_count(void)
{
    return g_hist_n;
}

int mr_history_get(int i, const char **who, const char **what, mr_u32 *color)
{
    int idx;

    if (i < 0 || i >= g_hist_n)
        return 0;

    /* Newest first: step back from the head. */
    idx = (g_hist_head - 1 - i + MR_HISTORY_MAX * 2) % MR_HISTORY_MAX;
    *who = g_hist[idx].who;
    *what = g_hist[idx].what;
    *color = g_hist[idx].color;
    return 1;
}

/* --- rollback -------------------------------------------------------------
 *
 * A ring of checkpoints, each holding where execution was and what the scene
 * looked like, plus a slice of a shared ring of variable deltas: the values
 * those variables held when the checkpoint was taken.
 *
 * Rolling back one step means undoing every delta recorded since the previous
 * checkpoint, newest first, and restoring that checkpoint's scene and
 * execution point. Undoing newest-first matters when a variable was written
 * more than once.
 */

#define MR_ROLLBACK_MAX 100      /* config.hard_rollback_limit */
#define MR_DELTA_RING   768

typedef struct {
    char     name[64];
    mr_value old;
    int      existed;
} rb_delta;

typedef struct {
    long         pc;
    long         sp;
    long         stack[MR_SAVE_STACK];
    mr_scenelist scene;

    /* The window and the music belong to the checkpoint as much as the scene
     * does. `window hide` before a title card left the say window hidden, so
     * rolling back to the line before it redrew the scene and then displayed
     * no text at all. */
    int          window_shown;
    char         music[128];
    int          music_loop;

    long         delta_first;   /* absolute index into the delta ring */
    long         delta_last;
    int          used;

    /* Set when a rollback stepped *over* this point: the slice of
     * g_fwd_deltas holding the values its variables had here, so that
     * rolling forward onto it can put them back. See mr_rollforward. */
    long         fwd_first, fwd_last;

    /* Whether that slice was ever written. An *empty* slice is ordinary --
     * most steps change no variables, a line of dialogue least of all -- so
     * emptiness cannot be used to tell "nothing to record" from "never
     * recorded". This can, and the difference is a bug: a forward step onto a
     * point that was never captured moves the pc and the scene and leaves the
     * variables behind. That shipped once, as a title card drawing another
     * chapter's title; see mr_interp_rollback. Cleared with the rest of the
     * point when a checkpoint overwrites it, which is correct -- a rewritten
     * point has no recorded future.
     *
     * Not in the save, and it needs no reader: a log read from a file arrives
     * with no forward steps at all (mr_history_read opens with
     * mr_rollback_reset), so the only points a rollforward can ever target
     * are ones this session stepped over and therefore captured. That is what
     * makes the check below safe to leave on in a shipped build -- it cannot
     * cry wolf at a loaded slot. */
    int          fwd_captured;

    /* The option taken at this point, **plus one**; 0 when this is not an
     * answered menu. Ren'Py's checkpoint carries the interaction's return
     * value and for a menu that is the choice (menuexports.py:432).
     *
     * Offset by one so that zero means "none": two places memset an rb_point
     * and option 0 is a real answer, so a plain -1 sentinel would have to be
     * re-established at both -- and silently become "the player chose the
     * first option" at any third site added later. */
    int          answer1;

    /* What kind of statement this was taken at; see mr_cp_kind. MR_CP_SAY is
     * zero, so a point from an older save -- which could only be a say --
     * reads correctly. */
    int          kind;

    /* 1 when this point came out of a file and the `step_start` of each of
     * its animating entries holds a count of ms rather than a timestamp; see
     * the note on the parked block table below. A point taken in this process
     * leaves it 0, because its timestamps are already on this clock. */
    int          anim_relative;
} rb_point;

static rb_point g_points[MR_ROLLBACK_MAX];
static int      g_np;          /* how many are filled */

/* One line per rollback and per rollforward, for diagnosing a report that
 * will not reproduce. Off by default -- a held-down Page Up would otherwise
 * fill a shipped log.txt -- and turned on the way the other diagnostics are,
 * by an environment variable read in main.c. See mr_rollback_trace. */
static int      g_trace;

void mr_rollback_trace(int on)
{
    g_trace = on ? 1 : 0;
    if (g_trace)
        mr_log("rollback: tracing every step; `over pc -1` means nothing was "
               "recorded for the way forward");
}
static int      g_head;        /* where the next one goes */

/* --- the transform blocks the checkpoints were animating ----------------- */

/* A checkpoint's scene entries carry `atl` pointers into the script, and a
 * file cannot hold a pointer -- the same problem save.c has for the live
 * scene, and the reason a rolled-back checkpoint used to come back frozen
 * after a load.
 *
 * It is not solved the same way, because a hundred checkpoints storing a
 * block each would be most of a megabyte of BSS to park on read. They do not
 * need to: a hundred checkpoints of one scene almost always share the *same*
 * block, and the pointer says so for free. Every `atl` in the ring points
 * either into the script's statement array or into scene.c's parked pool,
 * both stable for the life of the process, so pointer identity is exact. The
 * log therefore writes a table of distinct blocks once and each entry stores
 * an index into it.
 *
 * Sixteen is generous for that -- it is distinct *blocks* across the whole
 * ring, not animating entries. Past it an entry writes "no block" and loads
 * as it did before this existed, which is the one behaviour already known to
 * be survivable.
 */
#define MR_CP_ATL_MAX 16

static mr_atl g_cp_atl[MR_CP_ATL_MAX];   /* blocks parked from a file */
static int    g_cp_natl;

/* The index of `a` in a writer's table, adding it if there is room, or -1.
 *
 * Called twice over the same checkpoints in the same order -- once to build
 * the table and once to write the references -- so it has to be a pure
 * function of that walk. It is: a hit returns the existing slot and an
 * overflow adds nothing, so the second pass sees exactly the table the first
 * one wrote. */
static int atl_index(const mr_atl **tab, int *ntab, const mr_atl *a)
{
    int i;

    for (i = 0; i < *ntab; i++) {
        if (tab[i] == a)
            return i;
    }
    if (*ntab >= MR_CP_ATL_MAX)
        return -1;
    tab[*ntab] = a;
    return (*ntab)++;
}

/* Read one checkpoint entry's reference into the parked table and arm the
 * entry from it. Returns 1 if an animation was installed.
 *
 * Without this a checkpoint from a file came back with a placement and
 * nothing to move it on, so rolling back onto a line an animation had played
 * over left the sprite frozen part way through it -- the live-scene bug
 * save.c fixes, surviving in the rollback log.
 */
static int read_cp_atl(FILE *f, mr_shown *s)
{
    long idx1 = (long)mr_get32(f);
    const mr_atl *a;
    mr_place base;
    long span = 1;
    int step, t;

    if (idx1 <= 0)
        return 0;

    /* A non-zero index means the writer put a step and a placement here, so
     * both are read on the strength of the index alone. Validating first and
     * reading second would leave those two fields in the stream on a bad
     * index, and every checkpoint after this one would be read from the wrong
     * offset -- far worse than a lost animation, and reachable only from a
     * damaged file, which is exactly when it would be hardest to recognise. */
    step = (int)mr_get32(f);
    mr_get_place_full(f, &base);

    if (idx1 > g_cp_natl) {
        mr_log("save: a checkpoint names transform %ld of %d; that entry "
               "will load frozen", idx1, g_cp_natl);
        return 0;
    }

    a = &g_cp_atl[idx1 - 1];
    if (step < 0 || step > a->n)
        step = a->n;

    /* How long "it is already over" has to be: every step from here to the
     * end, plus a millisecond, so the first atl_update runs the block out in
     * one go. That is the behaviour an in-session rollback gets from a stale
     * absolute timestamp, arrived at without needing this process's clock --
     * which the reader does not have, and which on a freshly booted machine
     * could be smaller than a ninety-second step.
     *
     * A `repeat` block laps instead and starts a fresh cycle from now, which
     * is why atl_base had to be written: the lap restarts from it, and a
     * zeroed one would lose the `at center` the block began at.
     *
     * mr_scene_resume_anim turns this into now - span. */
    for (t = step; t < a->n; t++)
        span += a->v[t].ms;

    s->atl = a;
    s->atl_base = base;
    s->step = step;
    s->step_start = span;
    /* Irrelevant to the outcome -- a span that large completes the step at
     * t == 1, whatever it started from -- so it is set to what it would hold
     * after that first completed step rather than written to the file. */
    s->step_from = s->place;
    return 1;
}

/* Is there anything left of this entry's animation to record? A block that
 * has run out and does not repeat has nothing to resume, and the placement
 * already in the checkpoint is where it finished. */
static int atl_live(const mr_shown *s)
{
    return s->atl && (s->step < s->atl->n || s->atl->repeat);
}

static rb_delta g_deltas[MR_DELTA_RING];
static long     g_delta_next;  /* absolute; wraps through the ring */

static int      g_blocked;

/* --- rollforward ---------------------------------------------------------
 *
 * Ren'Py binds PageDown and the wheel down to `rollforward` (00keymap.rpy:66)
 * and it is the other half of rollback: having gone back, go forward again,
 * and do nothing at all once there is nothing to return to.
 *
 * The checkpoints are not duplicated. Rolling back used to mark the points it
 * passed `used = 0`; now it leaves them in the ring and counts how many lie
 * ahead of g_head, so a forward step is a cursor move rather than a copy.
 * That matters here: an rb_point is about 5.6KB, mostly the scene list, and a
 * parallel stack of a hundred of them would be over half a megabyte on a
 * machine with sixteen.
 *
 * What *is* copied is the values. The delta log records the value a variable
 * held *before* a write, which is what rolling back needs and is useless
 * going forward, so the unwind pauses at the point it is stepping over and
 * captures the values as they are there. Those are the only new bytes.
 *
 * The forward log is deliberately much smaller than the backward one. It is
 * for a player who overshot by a few lines, not a second history; when a step
 * does not fit, rolling forward refuses rather than restoring something
 * half-applied, which is the same thing Ren'Py does at the end of its log.
 */
#define MR_FWD_DELTAS 128

static rb_delta g_fwd_deltas[MR_FWD_DELTAS];
static long     g_fwd_next;    /* absolute; wraps through the ring */

/* How many checkpoints ahead of g_head are still valid to return to. */
static int      g_fwd;

void mr_rollback_reset(void)
{
    g_np = 0;
    g_head = 0;
    g_delta_next = 0;
    g_blocked = 0;
    g_fwd = 0;
    g_fwd_next = 0;
}

void mr_rollback_block(void)
{
    /* renpy.block_rollback(): everything before here becomes unreachable. */
    g_np = 0;
    g_head = 0;
    g_delta_next = 0;
    g_fwd = 0;
    g_fwd_next = 0;
}

void mr_rollback_journal(const char *name, const mr_value *old, int existed)
{
    rb_delta *d;
    long i;

    if (g_blocked || g_np == 0)
        return;
    if (!name || (int)strlen(name) >= (int)sizeof(d->name))
        return;

    /* The persistent data is not part of the rollback log. In Ren'Py it is a
     * separate object from the store the log snapshots, so rolling back past
     * `$ persistent.ending = True` does not re-lock the extra it unlocked --
     * and an unlock the player has earned should not be taken away by a
     * rollback. The preferences are in the same position. */
    if (mr_persist_is_persistent(name))
        return;

    /* Only the first write since the checkpoint matters: that is the value
     * the checkpoint should be restored to. */
    {
        rb_point *p = &g_points[(g_head - 1 + MR_ROLLBACK_MAX) %
                                MR_ROLLBACK_MAX];
        for (i = p->delta_first; i < g_delta_next; i++) {
            if (!strcmp(g_deltas[i % MR_DELTA_RING].name, name))
                return;
        }
    }

    d = &g_deltas[g_delta_next % MR_DELTA_RING];
    memset(d, 0, sizeof(*d));
    strncpy(d->name, name, sizeof(d->name) - 1);
    d->existed = existed;
    if (existed && old)
        d->old = *old;
    g_delta_next++;
}

void mr_rollback_checkpoint(const mr_gamestate *g, long pc, mr_cp_kind kind)
{
    rb_point *p;
    int i;
    int same = 0;
    int keep_answer1 = 0;

    if (g_blocked)
        return;

    /* Close the previous checkpoint's delta slice. */
    if (g_np > 0) {
        rb_point *prev = &g_points[(g_head - 1 + MR_ROLLBACK_MAX) %
                                   MR_ROLLBACK_MAX];
        prev->delta_last = g_delta_next;
    }

    /* Does this checkpoint land on a point we rolled back over?
     *
     * After rolling back, the caller resumes at the target's statement and
     * re-running it takes a fresh checkpoint in that same slot. That is the
     * *same* statement, so the points beyond it are still a future worth
     * keeping. Anything else means the player went somewhere new and the
     * future they had is gone -- which is what makes rolling back, choosing
     * differently and then pressing PageDown do nothing instead of jumping
     * into the abandoned branch.
     *
     * Compared on pc, which is the statement index: the one thing that says
     * "this is that line again".
     *
     * Ren'Py has a third condition here that this deliberately does not: the
     * future survives only `if keep_rollback or self.rolled_forward`
     * (rollback.py:787), so clicking forward over ground you had rolled back
     * discards it and only the rollforward key replays it. It cannot be
     * ported as it stands, because it asks a question this checkpoint is too
     * early to answer. Ren'Py checkpoints a say *after* its interaction
     * (character.py:944), by which time it knows whether the player clicked
     * or pressed the key; say_checkpoint runs when the line is displayed,
     * before any input. Gating on it here made every rollback destroy its own
     * forward steps -- the re-run's checkpoint fired immediately, with no
     * press to have happened yet -- which seamtest caught at once.
     *
     * So the engine stays more generous than Ren'Py for lines: click forward
     * and the future is still there. Moving the say checkpoint to dismissal
     * time is the honest fix and is a change to rollback itself, not to this.
     * Menus are unaffected -- mr_interp_choose clears the log for any click,
     * including a click on the same option, which is what Ren'Py does.
     *
     * `same` is kept separately from the decision because the answer has to
     * be carried over whenever this is the same statement, even when the
     * future is being dropped: the point is still that menu, and a rollback
     * onto it later must still know what was chosen there. */
    same = (g_points[g_head].used && g_points[g_head].pc == pc);
    if (same)
        keep_answer1 = g_points[g_head].answer1;

    if (g_fwd > 0 && same)
        g_fwd--;
    else
        g_fwd = 0;

    /* The forward slice is cleared with the rest of the point. It is always
     * written by the rollback that creates the forward step and consumed by
     * the step itself, so there is never a live one to preserve -- and
     * keeping a stale slice around would mean re-applying values from an
     * abandoned branch. */
    p = &g_points[g_head];
    memset(p, 0, sizeof(*p));
    p->answer1 = keep_answer1;
    p->kind = (int)kind;
    p->pc = pc;
    p->sp = g->sp;
    for (i = 0; i < MR_SAVE_STACK; i++)
        p->stack[i] = g->stack[i];
    p->scene = g->scene;

    /* Only the screens the script showed.
     *
     * Here rather than in the caller, deliberately. A checkpoint is normally
     * taken with nothing but the story on screen, but the language switch
     * takes one from inside the preferences screen -- it has just discarded
     * the log and the line the player is looking at needs a checkpoint back.
     * Recording the lot put the open game menu into the story's scene, and
     * rolling back to that line later brought the menu up again with the
     * dialogue box drawn over it.
     *
     * mr_save_write has the same filter for the same reason (save.c:350),
     * applied at *its* call site -- and that is exactly why this one was
     * missing for so long: the knowledge lived in one path and the other
     * never learned it. A log that cannot record an interface screen is a
     * stronger guarantee than every caller remembering to strip them, so the
     * rule lives with the log.
     *
     * `show screen title` chapter cards are part of the story and stay. */
    {
        int src, keep;
        for (src = 0, keep = 0; src < p->scene.nscreens; src++) {
            if (!p->scene.screen_story[src])
                continue;
            if (keep != src) {
                strcpy(p->scene.screens[keep], p->scene.screens[src]);
                p->scene.screen_story[keep] = p->scene.screen_story[src];
                p->scene.screen_z[keep] = p->scene.screen_z[src];
            }
            keep++;
        }
        p->scene.nscreens = keep;
    }

    p->window_shown = g->window_shown;
    strncpy(p->music, g->music, sizeof(p->music) - 1);
    p->music_loop = g->music_loop;
    p->delta_first = g_delta_next;
    p->delta_last = g_delta_next;
    p->used = 1;

    g_head = (g_head + 1) % MR_ROLLBACK_MAX;
    if (g_np < MR_ROLLBACK_MAX)
        g_np++;
}

/* The newest point is the one the player is looking at, which for a menu is
 * the menu's own checkpoint. */
static rb_point *newest(void)
{
    if (g_np <= 0)
        return NULL;
    return &g_points[(g_head - 1 + MR_ROLLBACK_MAX) % MR_ROLLBACK_MAX];
}

void mr_rollback_resume_at(long pc)
{
    rb_point *p = newest();

    if (!p || !p->used || p->pc != pc)
        return;

    /* Only the head moves. The point keeps its contents, including its
     * answer and its delta slice, so mr_rollback_checkpoint's `same` test
     * recognises the re-run and carries the answer over -- the same path an
     * in-session rollback's re-run takes.
     *
     * g_delta_next is deliberately left where it is: on this path the deltas
     * past this point are the ones the load itself journalled while putting
     * the variables back, and rewinding over them would hand a later
     * rollback values from before the load. */
    g_head = (g_head - 1 + MR_ROLLBACK_MAX) % MR_ROLLBACK_MAX;
    g_np--;
}

void mr_rollback_answer(int n)
{
    rb_point *p = newest();

    if (!p || !p->used || n < 0)
        return;
    p->answer1 = n + 1;
}

int mr_rollback_answer_here(void)
{
    rb_point *p = newest();

    if (!p || !p->used || p->answer1 <= 0)
        return -1;
    return p->answer1 - 1;
}

int mr_rollback_depth(void)
{
    /* The newest checkpoint is where we already are, so it is not a step. */
    return g_np > 0 ? g_np - 1 : 0;
}

/* Record, on the point being stepped over, the values its variables hold
 * right now -- which is what they held there, because the caller has already
 * unwound to it.
 *
 * `first`..`last` is the delta slice about to be undone: the writes that
 * happened between the target and this point. Those names are exactly the
 * ones whose values differ between the two, so they are the only ones worth
 * saving. A name may appear more than once across the slice; the forward log
 * takes each only once, since the live value is the same whichever delta
 * pointed at it.
 *
 * On overflow the slice is marked empty and the forward chain is cut. A
 * partially captured step would restore some variables and not others, which
 * is worse than not going forward at all.
 */
static void capture_forward(rb_point *over, long first, long last)
{
    long i, j;

    over->fwd_first = g_fwd_next;
    over->fwd_last = g_fwd_next;

    for (i = first; i < last; i++) {
        rb_delta *d = &g_deltas[i % MR_DELTA_RING];
        mr_value v;
        rb_delta *f;

        for (j = over->fwd_first; j < g_fwd_next; j++) {
            if (!strcmp(g_fwd_deltas[j % MR_FWD_DELTAS].name, d->name))
                break;
        }
        if (j < g_fwd_next)
            continue;                     /* already captured */

        if (g_fwd_next - over->fwd_first >= MR_FWD_DELTAS) {
            over->fwd_last = over->fwd_first;   /* empty: refuse the step */
            g_fwd_next = over->fwd_first;
            g_fwd = 0;
            mr_log("rollforward: step too large to record, forward log cut");
            return;
        }

        f = &g_fwd_deltas[g_fwd_next % MR_FWD_DELTAS];
        memset(f, 0, sizeof(*f));
        strncpy(f->name, d->name, sizeof(f->name) - 1);
        f->existed = mr_var_get(d->name, &v);
        if (f->existed)
            f->old = v;
        g_fwd_next++;
    }
    over->fwd_last = g_fwd_next;
    over->fwd_captured = 1;
}

/* Step back one checkpoint.
 *
 * The deltas recorded between two checkpoints describe the step from the
 * earlier to the later, so returning to checkpoint B means undoing everything
 * recorded from B onwards -- newest first, so repeated writes to one variable
 * unwind in order.
 *
 * `on_checkpoint` says whether the player is looking at the newest checkpoint
 * right now. While a line of dialogue is up, they are, so going back a line
 * means skipping past it to the one before. Sitting at a `renpy.pause()`
 * under a title card they are not: the newest checkpoint is the last line
 * shown, and that is where back should go.
 *
 * The checkpoints passed over are dropped, because the caller resumes at the
 * target's statement and re-running it takes a fresh one. Without that,
 * rolling back twice in a row would stand still.
 */
int mr_rollback_back(mr_gamestate *out, long *pc, long here)
{
    rb_point *target;
    long i, floor_;
    int idx, k, drop;
    int on_checkpoint;
    rb_point *top = newest();

    /* Is the newest checkpoint the statement the player is looking at? */
    on_checkpoint = (top && top->used && top->pc == here);
    drop = on_checkpoint ? 2 : 1;

    if (g_np < drop)
        return 0;
    if (on_checkpoint && mr_rollback_depth() <= 0)
        return 0;

    idx = (g_head - drop + MR_ROLLBACK_MAX * 2) % MR_ROLLBACK_MAX;
    target = &g_points[idx];
    if (!target->used)
        return 0;

    /* Anything the delta ring has already overwritten cannot be undone; stop
     * there rather than reading a recycled entry. */
    floor_ = target->delta_first;
    if (floor_ < g_delta_next - MR_DELTA_RING)
        floor_ = g_delta_next - MR_DELTA_RING;

    /* The unwind, in two stages when a point is being stepped over.
     *
     * `over` is that point -- the line the player was reading. Rolling
     * forward has to land on it, which means knowing what its variables held
     * *there*, and the delta log only records values from before each write.
     * So: unwind as far as `over` first, which leaves the live store holding
     * exactly its values, capture them, and only then unwind the rest.
     *
     * Capturing after a full unwind to the target instead would record the
     * target's own values and rolling forward would change nothing. */
    {
        rb_point *over = (drop == 2)
            ? &g_points[(g_head - 1 + MR_ROLLBACK_MAX) % MR_ROLLBACK_MAX]
            : NULL;
        long mid = over ? over->delta_first : floor_;

        if (mid < floor_)
            mid = floor_;

        /* One line per rollback, naming what it is about to undo and what it
         * will record for the way forward. Off unless asked for: this is one
         * line per keypress, and holding Page Up fills a shipped log with
         * them.
         *
         * This exists because a report arrived that could not be reproduced:
         * a title card rolled back to with the *previous* chapter's title on
         * it, live, "not always", and after a few rollforwards. The save sent
         * with it proved the store had already lost the assignment while the
         * log still held the delta for it -- so the fault is in a step, and
         * which step is the one thing neither the harness nor the file can
         * say. A step is one keypress, so this is one line per keypress.
         *
         * `over` is the point whose values are being recorded for the way
         * back forward; -1 means none was, which is the case worth staring
         * at if a rollforward ever restores too little. */
        if (g_trace)
            mr_log("rollback: to pc %ld (deltas %ld..%ld), over pc %ld, "
                   "fwd now %d", target->pc, floor_, g_delta_next,
                   over ? over->pc : -1, g_fwd + drop);
        for (i = g_delta_next - 1; i >= mid; i--) {
            rb_delta *d = &g_deltas[i % MR_DELTA_RING];
            if (d->existed)
                mr_var_set_no_journal(d->name, &d->old);
            else
                mr_var_unset(d->name);
        }

        if (over)
            capture_forward(over, floor_, mid);

        for (i = mid - 1; i >= floor_; i--) {
            rb_delta *d = &g_deltas[i % MR_DELTA_RING];
            if (d->existed)
                mr_var_set_no_journal(d->name, &d->old);
            else
                mr_var_unset(d->name);
        }
    }
    g_delta_next = target->delta_first;

    memset(out, 0, sizeof(*out));
    out->sp = target->sp;
    for (k = 0; k < MR_SAVE_STACK; k++)
        out->stack[k] = target->stack[k];
    out->scene = target->scene;
    out->window_shown = target->window_shown;
    strncpy(out->music, target->music, sizeof(out->music) - 1);
    out->music_loop = target->music_loop;
    /* Both directions carry it: a rollforward installs a stored point the
     * same way a rollback does, so a forward step into the part of the log
     * that came out of the file needs the same rebase. */
    out->anim_relative = target->anim_relative;
    *pc = target->pc;

    /* The checkpoints passed over stay in the ring, marked used, and become
     * the future: g_head moves back over them and g_fwd counts them. They
     * used to be cleared here, which is what made this a one-way trip.
     *
     * They are still "dropped" in the sense the comment above describes --
     * out of g_np, so a second rollback steps past them rather than standing
     * still -- and the caller's re-run overwrites the target's slot, which is
     * why the re-run is what decrements g_fwd rather than this. */
    g_head = idx;
    g_np -= drop;
    if (g_np < 0)
        g_np = 0;
    g_fwd += drop;
    if (g_fwd > MR_ROLLBACK_MAX)
        g_fwd = MR_ROLLBACK_MAX;

    /* The history entries those checkpoints wrote go with them; the one for
     * the resumed line is written again when it re-runs.
     *
     * Counted rather than assumed to be `drop`. That held while every
     * checkpoint was a say, and a menu checkpoint writes no history entry --
     * so popping one per dropped point would have eaten a line of dialogue
     * out of the history screen for every menu crossed. */
    {
        int h = 0;
        for (k = 0; k < drop; k++) {
            rb_point *d = &g_points[(idx + k) % MR_ROLLBACK_MAX];
            /* Only the points that put a line in the history wrote one: a
             * say, and a menu that opened with a caption. A bare menu and a
             * pause did not, and popping one for them would eat a line of
             * dialogue out of the history screen for every one crossed.
             *
             * Written as the positive test rather than `!= MR_CP_MENU`: the
             * next kind to be added is far more likely to be another
             * non-dialogue stop than another one that speaks, and as an
             * exclusion list this silently counted MR_CP_PAUSE. */
            if (d->used && (d->kind == MR_CP_SAY ||
                            d->kind == MR_CP_MENU_CAPTION))
                h++;
        }
        mr_history_pop(h);
    }

    return 1;
}

int mr_rollforward_depth(void)
{
    return g_fwd;
}

void mr_rollforward_clear(void)
{
    g_fwd = 0;
    g_fwd_next = 0;
}

/* Step forward one checkpoint: the inverse of mr_rollback_back.
 *
 * Re-applies the values captured when that point was stepped over, then
 * hands back its execution point and scene for the caller to install exactly
 * as it installs a rollback's -- same function, same music handling. The
 * statement it names then re-runs, which redisplays the line and takes a
 * fresh checkpoint, as going back does.
 */
int mr_rollforward(mr_gamestate *out, long *pc)
{
    rb_point *target;
    long i;
    int k;

    if (g_fwd <= 0)
        return 0;

    target = &g_points[g_head];
    if (!target->used) {
        g_fwd = 0;
        return 0;
    }

    /* Anything the forward ring has recycled cannot be re-applied. Refuse
     * the whole step rather than applying part of it. */
    if (target->fwd_last - target->fwd_first > MR_FWD_DELTAS ||
        target->fwd_first < g_fwd_next - MR_FWD_DELTAS) {
        mr_log("rollforward: the recorded values are gone, not stepping");
        g_fwd = 0;
        return 0;
    }

    for (i = target->fwd_first; i < target->fwd_last; i++) {
        rb_delta *d = &g_fwd_deltas[i % MR_FWD_DELTAS];
        if (d->existed)
            mr_var_set_no_journal(d->name, &d->old);
        else
            mr_var_unset(d->name);
    }

    /* Paired with the rollback line above: the `over pc` there and the `pc`
     * here are the same point for a step that went back and came forward
     * again, and the counts have to agree. Also off by default. */
    if (g_trace)
        mr_log("rollforward: to pc %ld, %ld variable%s restored", target->pc,
               target->fwd_last - target->fwd_first,
               (target->fwd_last - target->fwd_first) == 1 ? "" : "s");

    /* This one is *not* gated, because it cannot happen and is silent when it
     * does not.
     *
     * A forward step onto a point whose values were never recorded moves the
     * pc and the scene and leaves the variables where they were. It shipped
     * once -- two rollbacks inside one frame left a step half finished, and a
     * title card drew another chapter's title -- and the reports took three
     * rounds to pin down because nothing in the engine said a word.
     *
     * Emptiness is not the test: most steps change no variables and have an
     * empty slice legitimately. Having never been captured is the test. If
     * this line ever appears in a log, the step was begun twice before it
     * finished and mr_interp_rollback's guard has a hole. */
    if (!target->fwd_captured)
        mr_log("rollforward: to pc %ld, but no values were ever recorded for "
               "this step -- the variables are now from the wrong point",
               target->pc);

    memset(out, 0, sizeof(*out));
    out->sp = target->sp;
    for (k = 0; k < MR_SAVE_STACK; k++)
        out->stack[k] = target->stack[k];
    out->scene = target->scene;
    out->window_shown = target->window_shown;
    strncpy(out->music, target->music, sizeof(out->music) - 1);
    out->music_loop = target->music_loop;
    /* Both directions carry it: a rollforward installs a stored point the
     * same way a rollback does, so a forward step into the part of the log
     * that came out of the file needs the same rebase. */
    out->anim_relative = target->anim_relative;
    *pc = target->pc;

    /* The delta log resumes where this point began, so a rollback from here
     * unwinds the right slice. */
    g_delta_next = target->delta_first;

    /* g_head, g_np and g_fwd are deliberately left alone, which mirrors what
     * mr_rollback_back does: the caller resumes at this point's statement,
     * that statement re-runs, and *its* checkpoint lands in this slot, sees
     * the matching pc, and does the bookkeeping -- head forward one, g_np up
     * one, g_fwd down one.
     *
     * Advancing head here as well was the bug behind "rollforward moves one
     * line and then stops": the re-run's checkpoint landed in the *next*
     * forward point's slot instead of this one, overwrote it, and the pc no
     * longer matched, so the whole remaining future was discarded as a
     * branch. One step worked; the second had nothing left to go to.
     *
     * The resumed statement writes its own history entry too, exactly as it
     * does after a rollback, so nothing is added here. */
    return 1;
}

void mr_history_shutdown(void)
{
    mr_history_reset();
    mr_rollback_reset();
}

/* --- saving and loading the log -------------------------------------------
 *
 * Ren'Py stores the rollback log in the save, which is why rollback still
 * works after loading. Written here rather than in save.c so the layout stays
 * next to the structures it describes.
 *
 * Only the most recent checkpoints go in: each carries a display list, and
 * writing all hundred would dwarf the rest of the file for no practical gain.
 * Strings are length-prefixed, since a history line is typically a quarter of
 * its maximum.
 */

#define MR_SAVED_POINTS 16

void mr_history_write(FILE *f)
{
    int i, n;
    const mr_atl *tab[MR_CP_ATL_MAX];
    int ntab;
    int dropped;

    n = g_hist_n;
    mr_put32(f, (mr_u32)n);
    for (i = n - 1; i >= 0; i--) {          /* oldest first, so reading appends */
        const char *who, *what;
        mr_u32 color;
        if (!mr_history_get(i, &who, &what, &color))
            break;
        mr_put_vstr(f, who);
        mr_put_vstr(f, what);
        mr_put32(f, color);
    }

    /* Rollback: the newest MR_SAVED_POINTS checkpoints, oldest first. */
    n = g_np < MR_SAVED_POINTS ? g_np : MR_SAVED_POINTS;
    mr_put32(f, (mr_u32)n);

    /* Version 15: the distinct transform blocks those checkpoints were
     * animating, written once and referred to by index. Before the points,
     * so the reader has them parked by the time it meets a reference.
     *
     * The same walk as the loop below, in the same order, which is what makes
     * the indices agree between the two passes. */
    ntab = 0;
    dropped = 0;
    for (i = n - 1; i >= 0; i--) {
        rb_point *p = &g_points[(g_head - 1 - i + MR_ROLLBACK_MAX * 2) %
                                MR_ROLLBACK_MAX];
        int k;

        for (k = 0; k < p->scene.n; k++) {
            if (atl_live(&p->scene.v[k]) &&
                atl_index(tab, &ntab, p->scene.v[k].atl) < 0)
                dropped++;
        }
    }

    /* Said out loud, because the consequence is invisible: those entries load
     * as they did before version 15 -- frozen where the checkpoint caught
     * them -- and nothing about the save or the load looks wrong. Counted in
     * the first pass and logged once, rather than per entry, since a full
     * table reports every animating entry it meets afterwards.
     *
     * Not expected to fire. It needs more than MR_CP_ATL_MAX *distinct*
     * blocks across the sixteen saved checkpoints, and a scene normally
     * animates one or two. The case to watch is a busy Azami scene, where
     * `linear 0.5` appears at 97 separate statements: each is its own block,
     * so sixteen consecutive checkpoints could each name a different one. */
    if (dropped)
        mr_log("save: %d animating entries past the %d-block checkpoint "
               "table; they will load frozen", dropped, MR_CP_ATL_MAX);

    mr_put32(f, (mr_u32)ntab);
    for (i = 0; i < ntab; i++)
        mr_put_atl(f, tab[i]);

    for (i = n - 1; i >= 0; i--) {
        rb_point *p = &g_points[(g_head - 1 - i + MR_ROLLBACK_MAX * 2) %
                                MR_ROLLBACK_MAX];
        long d;
        int k, ndelta = 0;

        /* Named, not indexed, for the reason save.c is: a checkpoint index
         * outlives the script that gave it meaning. A checkpoint is always
         * taken at a say statement (interp.c, say_checkpoint), so its own
         * name is a translation identifier and survives an edit anywhere
         * else in the file -- the strongest anchor stmtid.h has. */
        {
            char id[MR_STMTID_MAX];

            mr_stmtid_name(p->pc, id, sizeof(id));
            mr_put_vstr(f, id);

            mr_put32(f, (mr_u32)p->sp);
            for (k = 0; k < p->sp && k < MR_SAVE_STACK; k++) {
                mr_stmtid_name(p->stack[k], id, sizeof(id));
                mr_put_vstr(f, id);
            }
        }

        /* What kind of statement this point was taken at. Version 9; see the
         * note on mr_cp_kind. Without it a loaded log is all says, and
         * rolling back over a menu pops a history line the menu never
         * wrote. */
        mr_put32(f, (mr_u32)p->kind);

        mr_put32(f, (mr_u32)p->window_shown);
        mr_put_vstr(f, p->music);
        mr_put32(f, (mr_u32)p->music_loop);

        mr_put_vstr(f, p->scene.bg);
        mr_put32(f, (mr_u32)p->scene.n);
        for (k = 0; k < p->scene.n; k++) {
            mr_put_vstr(f, p->scene.v[k].name);
            mr_put_vstr(f, p->scene.v[k].tag);
            mr_put_place(f, &p->scene.v[k].place);
            mr_put32(f, (mr_u32)p->scene.v[k].has_place);
            mr_put32(f, (mr_u32)p->scene.v[k].place.has_alpha);
            mr_put_fixed(f, p->scene.v[k].place.alpha);
            mr_put32(f, (mr_u32)p->scene.v[k].place.has_xzoom);
            mr_put_fixed(f, p->scene.v[k].place.xzoom);
            mr_put32(f, (mr_u32)p->scene.v[k].place.has_yzoom);
            mr_put_fixed(f, p->scene.v[k].place.yzoom);
            mr_put32(f, (mr_u32)p->scene.v[k].place.has_zoom);
            mr_put_fixed(f, p->scene.v[k].place.zoom);

            /* Version 15: which block it was running, offset by one so zero
             * means none -- the reason answer1 is offset the same way.
             *
             * No elapsed time goes with it, unlike the live scene's section
             * in save.c. A restored checkpoint's animation is meant to be
             * *over*: that is what an in-session rollback does, because
             * step_start is an absolute timestamp the clock has long passed,
             * and it is what Ren'Py does for the same reason -- the
             * SceneListEntry's show_time rolls back too, so the transform's
             * st is large and the ATL renders past its end. mr_scene_resume_anim
             * is handed a span that guarantees the same thing without needing
             * to know this process's clock; see the reader. */
            {
                const mr_shown *s = &p->scene.v[k];
                int live = atl_live(s);
                int idx = live ? atl_index(tab, &ntab, s->atl) : -1;

                mr_put32(f, (mr_u32)(idx + 1));
                if (idx >= 0) {
                    mr_put32(f, (mr_u32)s->step);
                    /* Only `repeat` reads this -- it is where the cycle
                     * starts again from -- but a lap is exactly the case that
                     * would otherwise restart from an all-zero placement and
                     * lose the `at center` the block began at. */
                    mr_put_place_full(f, &s->atl_base);
                }
            }
        }

        /* The screens this point had up. Version 16.
         *
         * Without them a checkpoint from a file restored its background and
         * its sprites and nothing else, so rolling back to a chapter title
         * card -- whose entire content is `show screen title` over a `scene
         * black` -- drew the black background alone. In-session rollback was
         * unaffected: the live point holds the list and mr_scene_restore puts
         * it back. Only the trip through the file lost it. Reported from
         * play, after the pause itself became a checkpoint and gave the
         * player a way to get there.
         *
         * Everything in this list is the story's own. Interface screens are
         * dropped when the checkpoint is taken (see mr_rollback_checkpoint),
         * which is why no flag is written for it and why the reader can set
         * screen_story unconditionally.
         *
         * The zorder goes with it. The live display list's section in save.c
         * does not store one and reloads every screen at 0; that is a
         * separate gap and not one to copy. */
        mr_put32(f, (mr_u32)p->scene.nscreens);
        for (k = 0; k < p->scene.nscreens; k++) {
            mr_put_vstr(f, p->scene.screens[k]);
            mr_put32(f, (mr_u32)p->scene.screen_z[k]);
        }

        /* This checkpoint's slice of the delta ring. The newest checkpoint
         * has no closed slice yet, so it runs to the current position. */
        {
            long last = (i == 0) ? g_delta_next : p->delta_last;
            long first = p->delta_first;
            if (first < g_delta_next - MR_DELTA_RING)
                first = g_delta_next - MR_DELTA_RING;
            for (d = first; d < last; d++)
                ndelta++;
            mr_put32(f, (mr_u32)ndelta);
            for (d = first; d < last; d++) {
                rb_delta *dd = &g_deltas[d % MR_DELTA_RING];
                mr_put_vstr(f, dd->name);
                mr_put32(f, (mr_u32)dd->existed);
                mr_put32(f, (mr_u32)dd->old.type);
                mr_put32(f, (mr_u32)dd->old.i);
                mr_put_fixed(f, dd->old.f);
                mr_put_vstr(f, dd->old.s);
            }
        }
    }
}

void mr_history_read(FILE *f, int version)
{
    long n, i;

    mr_history_reset();
    mr_rollback_reset();

    /* Clamped: the table holds MR_HISTORY_MAX, so a larger count is a
       corrupt file and looping on it would never return. */
    n = mr_get_count(f, MR_HISTORY_MAX, "history");
    for (i = 0; i < n; i++) {
        char who[MR_MAX_NAME], what[MR_HISTORY_TEXT];
        mr_u32 color;
        mr_get_vstr(f, who, sizeof(who));
        mr_get_vstr(f, what, sizeof(what));
        color = mr_get32(f);
        mr_history_add(who, what, color);
    }

    n = mr_get_count(f, MR_ROLLBACK_MAX, "rollback points");

    /* Version 15: the block table, parked before anything refers to it. */
    g_cp_natl = 0;
    if (version >= 15) {
        long nt = mr_get_count(f, MR_CP_ATL_MAX, "checkpoint transforms");
        long t;

        for (t = 0; t < nt; t++)
            mr_get_atl(f, &g_cp_atl[t]);
        g_cp_natl = (int)nt;
    }

    for (i = 0; i < n && i < MR_ROLLBACK_MAX; i++) {
        rb_point *p = &g_points[g_head];
        long nd, d;
        int k;

        memset(p, 0, sizeof(*p));
        {
            char id[MR_STMTID_MAX];

            mr_get_vstr(f, id, sizeof(id));
            p->pc = mr_stmtid_resolve(id);

            p->sp = (long)mr_get32(f);
            if (p->sp < 0 || p->sp > MR_SAVE_STACK)
                p->sp = 0;
            for (k = 0; k < p->sp; k++) {
                mr_get_vstr(f, id, sizeof(id));
                p->stack[k] = mr_stmtid_resolve(id);
                if (p->stack[k] < 0)
                    p->pc = -1;     /* unreachable; dropped below */
            }
        }

        /* Version 9 onwards. An older file has no such word, and the memset
         * above has already left this MR_CP_SAY -- the reading every save
         * before version 9 got whether it wanted it or not. A kind from a
         * corrupt file is clamped rather than trusted: it indexes nothing,
         * but it decides how much history a rollback pops, and an arbitrary
         * value there is an arbitrary number of lines. */
        if (version >= 9) {
            long kind = (long)mr_get32(f);
            if (kind < MR_CP_SAY || kind > MR_CP_PAUSE)
                kind = MR_CP_SAY;
            p->kind = (int)kind;
        }

        p->window_shown = (int)mr_get32(f);
        mr_get_vstr(f, p->music, sizeof(p->music));
        p->music_loop = (int)mr_get32(f);

        mr_get_vstr(f, p->scene.bg, MR_MAX_NAME);
        p->scene.n = (int)mr_get32(f);
        if (p->scene.n < 0 || p->scene.n > MR_MAX_SHOWN)
            p->scene.n = 0;
        for (k = 0; k < p->scene.n; k++) {
            mr_get_vstr(f, p->scene.v[k].name, MR_MAX_NAME);
            mr_get_vstr(f, p->scene.v[k].tag, MR_MAX_NAME);
            mr_get_place(f, &p->scene.v[k].place);
            /* Version 10. An older checkpoint predates the field and was
             * drawn as though every image carried a placement; see the
             * version note in save.c. */
            p->scene.v[k].has_place = (version >= 10) ? (int)mr_get32(f) : 1;
            /* Version 11: the Transform alpha. Absent in an older file, and
             * absent means opaque -- Ren'Py's default for the property. */
            if (version >= 11) {
                p->scene.v[k].place.has_alpha = (int)mr_get32(f);
                p->scene.v[k].place.alpha = (float)mr_get_fixed(f);
            }
            /* Version 12: the Transform xzoom/yzoom. Absent means the image's
             * own size, Ren'Py's 1.0 default for both. */
            if (version >= 12) {
                p->scene.v[k].place.has_xzoom = (int)mr_get32(f);
                p->scene.v[k].place.xzoom = (float)mr_get_fixed(f);
                p->scene.v[k].place.has_yzoom = (int)mr_get32(f);
                p->scene.v[k].place.yzoom = (float)mr_get_fixed(f);
            }
            /* Version 13: the combined zoom, which multiplies into both. */
            if (version >= 13) {
                p->scene.v[k].place.has_zoom = (int)mr_get32(f);
                p->scene.v[k].place.zoom = (float)mr_get_fixed(f);
            }

            /* Cleared first, so a slot that has no block -- an older file,
             * an entry that was not animating, a table overflow -- is left
             * not animating rather than holding whatever this ring slot had
             * before. A stale absolute timestamp against a NULL atl is
             * harmless only by luck. */
            p->scene.v[k].atl = NULL;
            p->scene.v[k].step = 0;
            p->scene.v[k].step_start = 0;

            /* Version 15: the block it was running, by index into the table
             * read above. Without this a checkpoint from a file came back
             * with a placement and nothing to move it on, so rolling back
             * onto a line an animation had played over left the sprite frozen
             * part way through it -- the bug the section before this one
             * fixed for the live scene, surviving in the rollback log. */
            if (version >= 15 && read_cp_atl(f, &p->scene.v[k]))
                p->anim_relative = 1;
        }

        /* Version 16: the screens this point had up. An older slot has none,
         * which is what it was able to say -- the list it carried in memory
         * was simply never written -- so a title card in such a slot still
         * rolls back to a bare background. Nothing can be derived to fix
         * that: the screen a `show screen` named is not recoverable from the
         * statement the checkpoint resumes at.
         *
         * p->scene is memset above, so nscreens stays 0 for those. */
        if (version >= 16) {
            /* Clamped rather than trusted, and through mr_get_count so a
             * count a corrupt file made up is clamped to the maximum instead
             * of to zero: the records that follow it are still in the stream
             * and have to be consumed, or everything after this point in the
             * file reads as garbage. */
            p->scene.nscreens = (int)mr_get_count(f, MR_MAX_SCREENS,
                                                  "checkpoint screens");
            for (k = 0; k < p->scene.nscreens; k++) {
                mr_get_vstr(f, p->scene.screens[k], MR_MAX_NAME);
                p->scene.screen_z[k] = (int)mr_get32(f);
                /* Only the story's own are ever written; see the writer. */
                p->scene.screen_story[k] = 1;
            }
        }

        p->delta_first = g_delta_next;
        nd = mr_get_count(f, MR_DELTA_RING, "rollback deltas");
        for (d = 0; d < nd; d++) {
            rb_delta *dd = &g_deltas[g_delta_next % MR_DELTA_RING];
            memset(dd, 0, sizeof(*dd));
            mr_get_vstr(f, dd->name, sizeof(dd->name));
            dd->existed = (int)mr_get32(f);
            dd->old.type = (mr_vtype)mr_get32(f);
            dd->old.i = (long)mr_get32(f);
            dd->old.f = mr_get_fixed(f);
            mr_get_vstr(f, dd->old.s, MR_VSTR_MAX);
            g_delta_next++;
        }
        p->delta_last = g_delta_next;
        p->used = 1;

        g_head = (g_head + 1) % MR_ROLLBACK_MAX;
        if (g_np < MR_ROLLBACK_MAX)
            g_np++;
    }

    /* Drop the checkpoints whose statements the script no longer has.
     *
     * The newest contiguous run is kept. Each checkpoint carries its own
     * slice of the delta ring, so one is a valid rollback target on its own
     * and the run does not have to reach all the way back -- the log written
     * to a save is only ever the newest MR_SAVED_POINTS anyway, so the oldest
     * kept checkpoint never has a predecessor. Stopping at the first
     * unresolved one means rollback works from the load point back as far as
     * the edit, rather than walking into a statement that has moved.
     *
     * The log is written oldest first and read in that order, so the run to
     * keep is a suffix of g_points[0..g_np-1]; it is moved down to the front
     * because the ring expects the filled entries to start at 0 after a
     * reset. */
    {
        int keep = 0, i2;

        for (i2 = g_np - 1; i2 >= 0; i2--) {
            if (g_points[i2].pc < 0)
                break;
            keep++;
        }

        if (keep < g_np) {
            mr_log("rollback: %d of %d checkpoints predate a script change",
                   g_np - keep, g_np);
            if (keep > 0)
                memmove(g_points, g_points + (g_np - keep),
                        (size_t)keep * sizeof(g_points[0]));
            g_np = keep;
            g_head = keep % MR_ROLLBACK_MAX;
        }
    }
}

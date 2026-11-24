/* save.c - reading and writing save slots.
 *
 * The file is a flat binary record written with explicit 32-bit fields, never
 * by dumping a struct: the host harnesses are 64-bit and the target is 32, and
 * a save written by one has to be readable by the other.
 *
 * Saves live beside the executable rather than in the archive, which is
 * read-only. Their names are 8.3 because the target filesystem is FAT.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mr.h"
#include "save.h"
#include "expr.h"
#include "interp.h"
#include "audio.h"
#include "serial.h"
#include "history.h"
#include "persist.h"
#include "stmtid.h"

#define MR_SAVE_MAGIC   0x5653524DUL   /* "MRSV" */

/* 5: the execution point is stored as statement *names* rather than indices,
 * so a save survives an edit to the scripts. See stmtid.h. Version 4 and
 * earlier hold indices, and there is no honest way to migrate them -- the
 * index means nothing without the script that produced it -- so they are
 * rejected by the version check like any other older format.
 *
 * 6: MR_VSTR_MAX grew from 128 to 256, and variable strings were a
 * fixed-width field, so the records got wider.
 *
 * 7: variable strings are length-prefixed. A fixed-width field made a save's
 * size track MR_VSTR_MAX -- a variable holding "start" wrote the whole field
 * -- which is what kept the cap small. Now the cap is 1024 and a save is
 * *smaller* than it was at 128, because almost every string is short.
 *
 * 5 and 6 are still read. Throwing away a player's saves over a field's
 * encoding would not be acceptable, and the difference is confined to one
 * loop. Version 4 and earlier hold raw statement indices and cannot be
 * migrated at all.
 *
 * 8: the header carries the save's time as a raw epoch alongside the
 * preformatted `when` string. Ren'Py formats a slot's time at *display*
 * time from the file's mtime (00action_file.rpy:269), which is what lets
 * FileTime honour the `format` it is given and lets the date follow the
 * player's current language. We formatted once, at save time, and ignored
 * the format argument entirely.
 *
 * The `when` string stays in the record rather than being replaced: it is
 * what versions 5 to 7 have, and a slot written then can still show its
 * date. Only the epoch is new, and only a version 8 reader looks for it.
 *
 * Version 9 adds one word to each rollback checkpoint: the kind of statement
 * it was taken at. That word decides how many history lines a rollback pops,
 * and leaving it out of the file meant a loaded game thought every
 * checkpoint was a line of dialogue -- so rolling back over a menu ate a
 * real line out of the history screen. A version 8 slot has no such word and
 * reads back as all-says, which is what it already did; the cost is confined
 * to the menus in that one old slot's last sixteen checkpoints.
 *
 * Version 10 adds one word to each shown image, in the display list and in
 * every rollback checkpoint: whether the statement that showed it named a
 * placement. mr_place_resolve needs it to tell `show kalea` (bottom centre,
 * config.default_transform) from `show kalea: yoffset 40` (x at 0), and
 * until now the field did not exist and the draw path passed a literal 1.
 *
 * A version 9 or older slot has no such word. It reads back as 1 -- the
 * behaviour that wrote it -- rather than as 0, because those saves were
 * taken by games whose every show carried a placement, so 1 is both what
 * they meant and what they were drawn with. Reading them as 0 would move
 * sprites that the player saved in the right place.
 *
 * Version 11 adds two more words per shown image: the Transform `alpha` and
 * whether one was given. Written beside has_place rather than inside
 * mr_put_place, which has no version to consult -- the same shape version 10
 * used, and the reason it is worth copying is that mr_get_place is also the
 * rollback checkpoint reader.
 *
 * A version 10 or older slot has no alpha. It reads back as "none given",
 * which draws opaque -- Ren'Py's default for the property and what those
 * saves were drawn with, since the engine ignored the property entirely.
 *
 * Version 12 adds four more, the same way: the Transform `xzoom` and `yzoom`
 * and whether each was given. An older slot has neither, which reads back as
 * "none given" and draws at the image's own size -- Ren'Py's 1.0 default, and
 * again what those saves were drawn with.
 *
 * Version 13 adds two more for the combined `zoom`, which multiplies into
 * both axes and is separate state from the two above. Same rule for an older
 * slot: none given, which is 1.0.
 *
 * Version 16 adds the screen list to each rollback checkpoint: a count, and
 * for each screen its name and its zorder. The live display list has carried
 * its screens since version 5 -- that is what makes a save taken on a title
 * card come back with the card -- but a checkpoint carried only the
 * background and the shown images, so rolling back *to* a title card from a
 * loaded game drew the `scene black` under it and nothing else. In-session
 * rollback was always right; only the trip through the file lost the screens.
 *
 * An older slot has no such list and its checkpoints read back with none,
 * which is what they recorded. Nothing can be derived for them: the name a
 * `show screen` gave is not recoverable from the statement a checkpoint
 * resumes at. A title card in a slot written before this still rolls back to
 * a bare background. */
#define MR_SAVE_VERSION 16

/* The version if we can read it, 0 if not. Returned rather than compared so
 * the reader can adapt to the older layout. */
static mr_u32 version_ok(mr_u32 v)
{
    return (v >= 5 && v <= MR_SAVE_VERSION) ? v : 0;
}

/* Ren'Py's config.thumbnail_width/height. Read from the script at save time
 * so another game's settings are honoured; these are the fallbacks. */
#define THUMB_W 256
#define THUMB_H 144

/* --- slot names -----------------------------------------------------------
 *
 * "1-1" .. "9-6" -> SAVEP1S1.SAV, "auto-3" -> SAVEA3.SAV,
 * "quick-2" -> SAVEQ2.SAV. All within 8.3.
 */
static int slot_path(const char *slot, char *out, int outsz)
{
    const char *dash;
    char page[8];
    int plen;

    if (!slot || !*slot || outsz < 16)
        return 0;

    dash = strchr(slot, '-');
    if (!dash || !dash[1])
        return 0;

    plen = (int)(dash - slot);
    if (plen <= 0 || plen >= (int)sizeof(page))
        return 0;
    memcpy(page, slot, (size_t)plen);
    page[plen] = '\0';

    if (!strcmp(page, "auto"))
        sprintf(out, "SAVEA%.2s.SAV", dash + 1);
    else if (!strcmp(page, "quick"))
        sprintf(out, "SAVEQ%.2s.SAV", dash + 1);
    else
        sprintf(out, "SAVEP%.1sS%.1s.SAV", page, dash + 1);

    return 1;
}

/* --- thumbnail ------------------------------------------------------------
 *
 * Stored as raw 16-bit 5-6-5 pixels. The stage is 32bpp, but a 256x144
 * thumbnail at 16bpp is 72KB against 147KB, it is only ever shown small, and
 * it avoids needing an encoder on a machine this slow.
 */

static void thumb_write(FILE *f, const mr_surface *stage)
{
    int x, y;

    mr_put32(f, THUMB_W);
    mr_put32(f, THUMB_H);

    if (!stage) {
        for (y = 0; y < THUMB_W * THUMB_H; y++) {
            fputc(0, f);
            fputc(0, f);
        }
        return;
    }

    /* Point sampled: the stage is 5x this size, and the result is displayed
     * at 276px wide. */
    for (y = 0; y < THUMB_H; y++) {
        const mr_u32 *row = stage->px +
            (size_t)(y * stage->h / THUMB_H) * stage->pitch;
        for (x = 0; x < THUMB_W; x++) {
            mr_u32 p = row[x * stage->w / THUMB_W];
            unsigned v = (unsigned)((MR_R(p) >> 3) << 11) |
                         (unsigned)((MR_G(p) >> 2) << 5) |
                         (unsigned)(MR_B(p) >> 3);
            fputc((int)(v & 0xFF), f);
            fputc((int)((v >> 8) & 0xFF), f);
        }
    }
}

static mr_surface *thumb_read(FILE *f)
{
    mr_surface *s;
    int w, h, x, y;

    w = (int)mr_get32(f);
    h = (int)mr_get32(f);
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024)
        return NULL;

    s = mr_surface_new(w, h);
    if (!s)
        return NULL;

    for (y = 0; y < h; y++) {
        mr_u32 *row = s->px + (size_t)y * s->pitch;
        for (x = 0; x < w; x++) {
            unsigned lo = (unsigned)(fgetc(f) & 0xFF);
            unsigned hi = (unsigned)(fgetc(f) & 0xFF);
            unsigned v = lo | (hi << 8);
            unsigned r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
            row[x] = MR_RGB((r << 3) | (r >> 2),
                            (g << 2) | (g >> 4),
                            (b << 3) | (b >> 2));
        }
    }
    s->has_alpha = 0;
    return s;
}

/* --- the pending screenshot -----------------------------------------------
 *
 * A save taken from the menu should show the scene the player left, not the
 * menu they are looking at. Ren'Py resolves this by taking the screenshot on
 * entry to the game menu (_enter_menu, and only when not already in a menu),
 * and that is what this holds until a slot is written.
 */
static mr_surface *g_shot;
static char        g_shot_name[64];

void mr_save_screenshot(const mr_surface *stage)
{
    mr_value v;
    int x, y;

    /* The label is captured here too, and for the same reason the picture is:
     * by the time a slot is clicked we are inside the save screen, whose
     * `title` parameter ("Sauvegarde") shadows the story's chapter title.
     *
     * `save_name` is Ren'Py's: the slot label is `extra_info=save_name`
     * (00action_file.rpy:415), defaulting to "" (00library.rpy:128), and it
     * is the *only* variable Ren'Py looks at.
     *
     * The fall-back to `title` is **a deliberate divergence, not Ren'Py**.
     * `title` means nothing to Ren'Py; this game happens to set it for its
     * chapter cards (`$ title = "Avril 2020"`, script.rpy:5908) and never
     * sets save_name, so under real Ren'Py its slots show a timestamp and
     * nothing else. Ours show the chapter, which is the nicer behaviour and
     * is why it is kept.
     *
     * Harmless for other games: one that sets save_name never reaches the
     * fall-back. The case to be aware of is a game that uses `title` for
     * something that is not a chapter name -- its slots would show that
     * string. Keeping the order this way round means save_name always wins.
     */
    g_shot_name[0] = '\0';
    if ((mr_var_get("save_name", &v) && v.type == MR_V_STR && v.s[0]) ||
        (mr_var_get("title", &v) && v.type == MR_V_STR && v.s[0])) {
        strncpy(g_shot_name, v.s, sizeof(g_shot_name) - 1);
        g_shot_name[sizeof(g_shot_name) - 1] = '\0';
    }

    if (!stage)
        return;

    if (!g_shot) {
        g_shot = mr_surface_new(THUMB_W, THUMB_H);
        if (!g_shot)
            return;
    }

    for (y = 0; y < THUMB_H; y++) {
        const mr_u32 *src = stage->px +
            (size_t)(y * stage->h / THUMB_H) * stage->pitch;
        mr_u32 *dst = g_shot->px + (size_t)y * g_shot->pitch;
        for (x = 0; x < THUMB_W; x++)
            dst[x] = src[x * stage->w / THUMB_W];
    }
    g_shot->has_alpha = 0;
}

/* --- the header, which the save screen reads without loading the rest --- */

typedef struct {
    char name[64];      /* what the save screen shows */
    char when[40];      /* formatted local time, as versions 5-7 stored it */
    long when_t;        /* the same moment as an epoch; 0 before version 8 */
} save_header;

static int read_header(const char *slot, save_header *h)
{
    char path[32];
    FILE *f;
    mr_u32 ver;

    if (!slot_path(slot, path, sizeof(path)))
        return 0;

    f = fopen(path, "rb");
    if (!f)
        return 0;

    if (mr_get32(f) != MR_SAVE_MAGIC || !(ver = version_ok(mr_get32(f)))) {
        fclose(f);
        return 0;
    }
    mr_get_str(f, h->name, sizeof(h->name));
    mr_get_str(f, h->when, sizeof(h->when));
    h->when_t = (ver >= 8) ? (long)(mr_u32)mr_get32(f) : 0;
    fclose(f);
    return 1;
}

int mr_save_exists(const char *slot)
{
    save_header h;
    return read_header(slot, &h);
}

/* --- writing --- */

int mr_save_write(const char *slot, const mr_surface *stage)
{
    char path[32];
    FILE *f;
    mr_gamestate g;
    save_header h;
    int i;

    if (!slot_path(slot, path, sizeof(path))) {
        mr_log("save: bad slot name '%s'", slot ? slot : "(null)");
        return 0;
    }

    f = fopen(path, "wb");
    if (!f) {
        mr_log("save: cannot write %s", path);
        return 0;
    }

    mr_interp_capture(&g);

    /* The name shown in the slot is the chapter title the story last set,
     * captured on the way into the menu. */
    strncpy(h.name, g_shot_name, sizeof(h.name) - 1);
    h.name[sizeof(h.name) - 1] = '\0';
    {
        time_t t = time(NULL);
        struct tm *lt = localtime(&t);

        h.when_t = (long)t;

        /* Still written, for versions 5-7 readers and because a save file is
         * easier to look at with a readable date in it. What the screen
         * shows now comes from when_t -- see FileTime. */
        if (lt)
            strftime(h.when, sizeof(h.when), "%d/%m/%Y %H:%M", lt);
        else
            h.when[0] = '\0';
    }

    mr_put32(f, MR_SAVE_MAGIC);
    mr_put32(f, MR_SAVE_VERSION);
    mr_put_str(f, h.name, sizeof(h.name));
    mr_put_str(f, h.when, sizeof(h.when));
    mr_put32(f, (mr_u32)h.when_t);

    /* Execution point, as names. Only the live part of the call stack is
     * written: the slots above sp are not return addresses, so naming them
     * would be naming whatever happened to be left there. */
    {
        char id[MR_STMTID_MAX];

        mr_stmtid_name(g.pc, id, sizeof(id));
        mr_put_vstr(f, id);

        mr_put32(f, (mr_u32)g.sp);
        for (i = 0; i < g.sp && i < MR_SAVE_STACK; i++) {
            mr_stmtid_name(g.stack[i], id, sizeof(id));
            mr_put_vstr(f, id);
        }
    }

    mr_put_str(f, g.who, sizeof(g.who));
    mr_put_str(f, g.what, sizeof(g.what));
    mr_put_str(f, g.tlid, sizeof(g.tlid));
    mr_put32(f, (mr_u32)g.has_dialogue);
    mr_put32(f, (mr_u32)g.window_shown);

    /* Scene. */
    mr_put_str(f, g.scene.bg, MR_MAX_NAME);
    mr_put32(f, (mr_u32)g.scene.n);
    for (i = 0; i < g.scene.n; i++) {
        mr_put_str(f, g.scene.v[i].name, MR_MAX_NAME);
        mr_put_str(f, g.scene.v[i].tag, MR_MAX_NAME);
        mr_put_place(f, &g.scene.v[i].place);
        mr_put32(f, (mr_u32)g.scene.v[i].has_place);
        mr_put32(f, (mr_u32)g.scene.v[i].place.has_alpha);
        mr_put_fixed(f, g.scene.v[i].place.alpha);
        mr_put32(f, (mr_u32)g.scene.v[i].place.has_xzoom);
        mr_put_fixed(f, g.scene.v[i].place.xzoom);
        mr_put32(f, (mr_u32)g.scene.v[i].place.has_yzoom);
        mr_put_fixed(f, g.scene.v[i].place.yzoom);
        mr_put32(f, (mr_u32)g.scene.v[i].place.has_zoom);
        mr_put_fixed(f, g.scene.v[i].place.zoom);
    }
    /* Only the screens the script showed. A save is always taken with the
     * game menu up, so writing the lot would restore the save menu over the
     * loaded game -- but a `show screen title` chapter card is part of the
     * story and has to come back. */
    {
        int nstory = 0;
        for (i = 0; i < g.scene.nscreens; i++)
            if (g.scene.screen_story[i])
                nstory++;
        mr_put32(f, (mr_u32)nstory);
        for (i = 0; i < g.scene.nscreens; i++)
            if (g.scene.screen_story[i])
                mr_put_str(f, g.scene.screens[i], MR_MAX_NAME);
    }

    /* Audio. */
    mr_put_str(f, g.music, sizeof(g.music));
    mr_put32(f, (mr_u32)g.music_loop);

    /* Variables. */
    {
        const char *name;
        const mr_value *v;
        long count = 0;
        long at;

        at = ftell(f);
        mr_put32(f, 0);                    /* patched below */

        for (i = 0; mr_var_enumerate(i, &name, &v); i++) {
            /* The persistent data is not part of a playthrough, so it is not
             * part of a save: loading an old slot must not restore the
             * volumes you had then, or re-lock an extra since unlocked. */
            if (mr_persist_is_persistent(name))
                continue;
            mr_put_str(f, name, 64);
            mr_put32(f, (mr_u32)v->type);
            mr_put32(f, (mr_u32)v->i);
            mr_put_fixed(f, v->f);
            mr_put_vstr(f, v->s);
            count++;
        }

        fseek(f, at, SEEK_SET);
        mr_put32(f, (mr_u32)count);
        fseek(f, 0, SEEK_END);
    }

    /* The text history and the rollback log, so the player can look back and
     * roll back after loading, as Ren'Py allows. */
    mr_history_write(f);

    /* Trailing section: what the story was waiting for. Appended rather than
     * folded into the body so that saves written before it existed still
     * load -- they simply end here, and read back as MR_WAIT_NONE, which is
     * what the old code assumed of every save anyway. */
    mr_put32(f, (mr_u32)g.wait_kind);
    mr_put32(f, (mr_u32)g.wait_ms);
    mr_put32(f, (mr_u32)g.hard_wait);

    /* Version 14: the animations the scene was running.
     *
     * One record per display list entry, in list order, so the reader can put
     * each back where it came from without naming it. An entry that is not
     * animating -- most of them -- costs a single zero.
     *
     * The time goes out as ms elapsed into the current step rather than the
     * absolute step_start the engine keeps, because the clock a save was
     * written on does not exist by the time it is read. The step index is
     * written too rather than derived from the elapsed time: the two together
     * say exactly where the block was, and a sum of step durations would be
     * one more thing to get wrong.
     *
     * Also appended rather than folded into the scene section above, which
     * would have moved every field after it in every existing slot. */
    for (i = 0; i < g.scene.n; i++) {
        const mr_shown *s = &g.scene.v[i];
        /* A block that has run out and does not repeat has nothing left to
         * resume, and the placement in the scene section is already where it
         * finished. */
        int live = s->atl && (s->step < s->atl->n || s->atl->repeat);

        mr_put32(f, (mr_u32)live);
        if (!live)
            continue;
        mr_put32(f, (mr_u32)s->step);
        mr_put32(f, (mr_u32)(g.now - s->step_start));
        mr_put_place_full(f, &s->atl_base);
        mr_put_place_full(f, &s->step_from);
        mr_put_atl(f, s->atl);
    }

    /* Prefer the shot taken on entry to the menu; the live stage at this
     * moment is the save screen itself. */
    thumb_write(f, g_shot ? g_shot : stage);

    fclose(f);
    mr_save_flush();
    mr_log("save: wrote %s (%s)", path, slot);
    return 1;
}

/* --- reading --- */

int mr_save_load(const char *slot)
{
    char path[32];
    FILE *f;
    mr_gamestate g;
    save_header h;
    long count;
    mr_u32 ver;
    int i;

    if (!slot_path(slot, path, sizeof(path)))
        return 0;

    f = fopen(path, "rb");
    if (!f)
        return 0;

    if (mr_get32(f) != MR_SAVE_MAGIC || !(ver = version_ok(mr_get32(f)))) {
        mr_log("save: %s is not a save of this version", path);
        fclose(f);
        return 0;
    }

    memset(&g, 0, sizeof(g));
    mr_get_str(f, h.name, sizeof(h.name));
    mr_get_str(f, h.when, sizeof(h.when));
    if (ver >= 8)
        (void)mr_get32(f);          /* when_t; the loader has no use for it */

    /* The execution point. A name that no longer resolves means the statement
     * this save sat at is gone from the script, and there is nothing useful to
     * do but say so: resuming at a guessed index is how the desynced-save bug
     * behaved, and it is worse than refusing. */
    {
        char id[MR_STMTID_MAX];

        mr_get_vstr(f, id, sizeof(id));
        g.pc = mr_stmtid_resolve(id);
        if (g.pc < 0) {
            mr_log("save: %s refers to '%s', which this script has no longer",
                   path, id);
            mr_stmtid_done();
            fclose(f);
            return 0;
        }

        g.sp = (long)(mr_u32)mr_get32(f);
        if (g.sp < 0 || g.sp > MR_SAVE_STACK) {
            mr_log("save: %s has a bad call depth %ld", path, g.sp);
            mr_stmtid_done();
            fclose(f);
            return 0;
        }
        for (i = 0; i < g.sp; i++) {
            mr_get_vstr(f, id, sizeof(id));
            g.stack[i] = mr_stmtid_resolve(id);
            if (g.stack[i] < 0) {
                mr_log("save: %s returns to '%s', which is gone", path, id);
                mr_stmtid_done();
                fclose(f);
                return 0;
            }
        }
    }

    mr_get_str(f, g.who, sizeof(g.who));
    mr_get_str(f, g.what, sizeof(g.what));
    mr_get_str(f, g.tlid, sizeof(g.tlid));
    g.has_dialogue = (int)mr_get32(f);
    g.window_shown = (int)mr_get32(f);

    mr_get_str(f, g.scene.bg, MR_MAX_NAME);
    g.scene.n = (int)mr_get32(f);
    if (g.scene.n < 0 || g.scene.n > MR_MAX_SHOWN)
        g.scene.n = 0;
    for (i = 0; i < g.scene.n; i++) {
        mr_get_str(f, g.scene.v[i].name, MR_MAX_NAME);
        mr_get_str(f, g.scene.v[i].tag, MR_MAX_NAME);
        mr_get_place(f, &g.scene.v[i].place);
        /* See the version 10 note above: an older slot predates the field and
         * was drawn as though every image carried a placement. */
        g.scene.v[i].has_place = (ver >= 10) ? (int)mr_get32(f) : 1;
        /* Version 11. An older slot has no alpha and draws opaque, which is
         * both Ren'Py's default and what it was drawn with. */
        if (ver >= 11) {
            g.scene.v[i].place.has_alpha = (int)mr_get32(f);
            g.scene.v[i].place.alpha = (float)mr_get_fixed(f);
        }
        /* Version 12, the same deal: no zoom means the image's own size. */
        if (ver >= 12) {
            g.scene.v[i].place.has_xzoom = (int)mr_get32(f);
            g.scene.v[i].place.xzoom = (float)mr_get_fixed(f);
            g.scene.v[i].place.has_yzoom = (int)mr_get32(f);
            g.scene.v[i].place.yzoom = (float)mr_get_fixed(f);
        }
        /* Version 13: the combined zoom. */
        if (ver >= 13) {
            g.scene.v[i].place.has_zoom = (int)mr_get32(f);
            g.scene.v[i].place.zoom = (float)mr_get_fixed(f);
        }

        /* The animation is not part of this section: the mr_atl is a pointer
         * into the parsed script and a file cannot hold one. `g` is memset
         * above, so an entry starts out not animating and stays that way
         * unless the version 14 section at the end of the file puts a block
         * back -- see the loop after the wait section. */
        g.scene.v[i].atl = NULL;
    }
    g.scene.nscreens = (int)mr_get32(f);
    if (g.scene.nscreens < 0 || g.scene.nscreens > MR_MAX_SCREENS)
        g.scene.nscreens = 0;
    for (i = 0; i < g.scene.nscreens; i++) {
        mr_get_str(f, g.scene.screens[i], MR_MAX_NAME);
        g.scene.screen_story[i] = 1;
    }

    mr_get_str(f, g.music, sizeof(g.music));
    g.music_loop = (int)mr_get32(f);

    /* Start from the init-time baseline so nothing from the state being
     * abandoned survives into the loaded game. A `config.rollback_enabled =
     * False` from the ending once outlived a load and silently disabled
     * rollback in the restored game. */
    mr_persist_snapshot();
    mr_var_reset_to_baseline();
    mr_persist_restore();

    /* MR_MAX_VARS is private to expr.c; this only has to be a bound that
       ends the loop, not the exact table size. */
    count = mr_get_count(f, 4096, "variables");
    for (i = 0; i < count; i++) {
        char name[64];
        mr_value v;

        memset(&v, 0, sizeof(v));
        mr_get_str(f, name, sizeof(name));
        v.type = (mr_vtype)mr_get32(f);
        v.i = (long)(mr_u32)mr_get32(f);
        v.f = mr_get_fixed(f);
        /* Length-prefixed since 7; a fixed-width field before that, whose
         * width is the MR_VSTR_MAX of the version that wrote it. */
        if (ver >= 7)
            mr_get_vstr(f, v.s, MR_VSTR_MAX);
        else
            mr_get_str(f, v.s, ver >= 6 ? MR_VSTR_V6 : MR_VSTR_V5);

        /* Skipped on the way in as well as on the way out: a slot written
         * before persistent data was separated still carries it, and must
         * not be allowed to reach back into the player's settings. */
        if (name[0] && !mr_persist_is_persistent(name))
            mr_var_set(name, &v);
    }

    mr_history_read(f, (int)ver);

    /* The trailing wait section, if this save has one. An older file ends
     * after the history, and not waiting is the right reading of it. */
    {
        int c = fgetc(f);
        if (c != EOF) {
            ungetc(c, f);
            g.wait_kind = (int)mr_get32(f);
            g.wait_ms = (long)mr_get32(f);
            g.hard_wait = (int)mr_get32(f);
        }
    }

    /* The animations, version 14 on. An older slot has none, and its entries
     * stay as the scene section left them -- the placement the animation had
     * reached, with nothing to carry it further, which is how every save
     * before this one loaded. */
    if (ver >= 14) {
        for (i = 0; i < g.scene.n; i++) {
            mr_atl a;

            if (!mr_get32(f))
                continue;
            g.scene.v[i].step = (int)mr_get32(f);
            /* Held as an elapsed time until mr_interp_restore adds the clock;
             * see mr_scene_resume_anim. */
            g.scene.v[i].step_start = (long)mr_get32(f);
            mr_get_place_full(f, &g.scene.v[i].atl_base);
            mr_get_place_full(f, &g.scene.v[i].step_from);
            mr_get_atl(f, &a);

            /* An empty block would be read as "animating" by
             * mr_scene_animating and then advance nothing, so the engine
             * would redraw every frame for the rest of the session. */
            if (a.n <= 0) {
                g.scene.v[i].step = 0;
                g.scene.v[i].step_start = 0;
                continue;
            }
            if (g.scene.v[i].step < 0 || g.scene.v[i].step > a.n)
                g.scene.v[i].step = a.n;
            g.scene.v[i].atl = mr_scene_park_atl(i, &a);
            g.anim_relative = 1;
        }
    }

    fclose(f);
    mr_stmtid_done();

    /* The screens in the file are the story's own -- a chapter title card --
     * and are restored. This used to clear the lot, which did keep the save
     * menu from coming back but also dropped `show screen title`, so a save
     * taken on a title card loaded to a bare black screen.
     *
     * A save written before that distinction existed recorded whatever was
     * on display, which was the menu. Ren'Py marks the game-menu family with
     * `tag menu`, so those are dropped on the way in; anything the script
     * shows has no such tag. */
    {
        int keep = 0;
        for (i = 0; i < g.scene.nscreens; i++) {
            const sl_screen *sc = mr_interp_find_screen(g.scene.screens[i]);
            if (sc && !strcmp(sc->tag, "menu"))
                continue;
            if (keep != i) {
                strcpy(g.scene.screens[keep], g.scene.screens[i]);
                g.scene.screen_story[keep] = g.scene.screen_story[i];
            }
            keep++;
        }
        g.scene.nscreens = keep;
    }

    mr_interp_restore(&g);
    mr_log("save: loaded %s (%s)", path, slot);
    return 1;
}

void mr_save_cycle(const char *page, int count)
{
    int i;

    if (!page || !*page || count < 2)
        return;

    for (i = count - 1; i >= 1; i--) {
        char slot[MR_SLOT_NAME];
        char from[32], to[32];

        sprintf(slot, "%.8s-%d", page, i);
        if (!slot_path(slot, from, sizeof(from)))
            continue;
        sprintf(slot, "%.8s-%d", page, i + 1);
        if (!slot_path(slot, to, sizeof(to)))
            continue;

        /* The destination has to go first: renaming onto an existing name
         * fails on FAT rather than replacing it, which would have left the
         * oldest save in place and quietly stopped the rotation one slot in. */
        remove(to);
        rename(from, to);       /* a missing `from` simply fails, which is the
                                 * "does nothing" Ren'Py documents */
    }

    mr_save_flush();
}

void mr_save_delete(const char *slot)
{
    char path[32];

    if (!slot_path(slot, path, sizeof(path)))
        return;
    remove(path);
    mr_save_flush();
}

/* --- what the save screen asks for ---------------------------------------
 *
 * The screen calls FileTime, FileSaveName and FileScreenshot for every slot
 * on the page, every time it is laid out. Re-reading six files per frame
 * would be absurd, so results are cached.
 *
 * The cache evicts least-recently-used. It used to only ever *fill*: when
 * full, slot_lookup returned NULL and nothing but a save or a delete cleared
 * it. Six slots to a page, so the first page filled half of it, the second
 * filled it, and every page visited after that reported no name, no date and
 * no thumbnail for all six slots -- a blank page, with the slots still
 * clickable, because FileLoadable goes through mr_save_exists which reads the
 * file rather than the cache.
 *
 * It was reported as a two-game problem, with one game's saves on page 1 and
 * the other's on page 2, and it is nothing of the kind: one game and three
 * pages is enough. savetest covers it.
 *
 * The size is a memory decision, not a speed one. A thumbnail is 256x144
 * ARGB, 147KB, so twelve of them is 1.7MB held for the rest of the run once
 * the player has browsed their saves -- which is why this does not simply
 * grow until nothing is ever evicted. It does have to be at least one page's
 * worth, or a single layout pass would evict entries it is still using:
 * gui.file_slot_cols x gui.file_slot_rows is 6 in both games' templates and
 * 12 leaves room for a game that shows twice that.
 */

#define SLOT_CACHE 12

typedef struct {
    char          slot[MR_SLOT_NAME];
    int           present;
    save_header   h;
    mr_surface   *thumb;
    int           thumb_tried;
    unsigned long used;        /* for the LRU; 0 means never */
} slot_ent;

static slot_ent     g_slots[SLOT_CACHE];
static int          g_nslots;
static unsigned long g_slot_clock;

static slot_ent *slot_lookup(const char *slot)
{
    int i, victim;

    for (i = 0; i < g_nslots; i++) {
        if (!strcmp(g_slots[i].slot, slot)) {
            g_slots[i].used = ++g_slot_clock;
            return &g_slots[i];
        }
    }

    if (g_nslots < SLOT_CACHE) {
        victim = g_nslots++;
    } else {
        victim = 0;
        for (i = 1; i < g_nslots; i++) {
            if (g_slots[i].used < g_slots[victim].used)
                victim = i;
        }
        /* The thumbnail is the cache's only owned allocation. Callers use the
         * surface within the call that fetched it -- slrender.c's
         * screen_image measures or blits it straight away and asks again next
         * pass -- so no pointer outlives an eviction. */
        mr_surface_free(g_slots[victim].thumb);
    }

    memset(&g_slots[victim], 0, sizeof(g_slots[victim]));
    strncpy(g_slots[victim].slot, slot, MR_SLOT_NAME - 1);
    g_slots[victim].present = read_header(slot, &g_slots[victim].h);
    g_slots[victim].used = ++g_slot_clock;
    return &g_slots[victim];
}

void mr_save_flush(void)
{
    int i;

    for (i = 0; i < g_nslots; i++)
        mr_surface_free(g_slots[i].thumb);
    memset(g_slots, 0, sizeof(g_slots));
    g_nslots = 0;
}

/* For memreport. The slot cache is bounded by entry count, not by bytes, so
 * what it costs is only visible as a measurement: twelve thumbnails is 1.7MB
 * held for the rest of the run. g_shot is the retained screenshot the save
 * screen keeps so a thumbnail shows the game rather than the menu -- already
 * downscaled to THUMB_W x THUMB_H when it is taken, so 147KB and not the
 * stage surface this comment used to claim. */
void mr_save_cache_stats(long *bytes, int *entries, long *shot_bytes)
{
    int i;

    *bytes = 0;
    *entries = 0;
    for (i = 0; i < g_nslots; i++) {
        if (g_slots[i].thumb) {
            *bytes += (long)g_slots[i].thumb->w *
                      (long)g_slots[i].thumb->h * 4L;
            (*entries)++;
        }
    }
    *shot_bytes = g_shot ? (long)g_shot->w * (long)g_shot->h * 4L : 0L;
}

void mr_save_shutdown(void)
{
    mr_save_flush();
    mr_surface_free(g_shot);
    g_shot = NULL;
}

const char *mr_save_time(const char *slot)
{
    slot_ent *e = slot_lookup(slot);
    return (e && e->present) ? e->h.when : NULL;
}

long mr_save_time_t(const char *slot)
{
    slot_ent *e = slot_lookup(slot);
    return (e && e->present) ? e->h.when_t : 0;
}

const char *mr_save_name(const char *slot)
{
    slot_ent *e = slot_lookup(slot);
    return (e && e->present) ? e->h.name : NULL;
}

mr_surface *mr_save_thumbnail(const char *slot)
{
    slot_ent *e = slot_lookup(slot);
    char path[32];
    FILE *f;

    if (!e || !e->present)
        return NULL;
    if (e->thumb_tried)
        return e->thumb;

    e->thumb_tried = 1;

    if (!slot_path(slot, path, sizeof(path)))
        return NULL;
    f = fopen(path, "rb");
    if (!f)
        return NULL;

    /* The thumbnail is last, so seek to it from the end: its size is fixed
     * by the two dimensions written just before the pixels. */
    if (fseek(f, -(long)(THUMB_W * THUMB_H * 2 + 8), SEEK_END) == 0)
        e->thumb = thumb_read(f);

    fclose(f);
    return e->thumb;
}

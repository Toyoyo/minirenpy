/* A save slot's text stays inside the slot.
 *
 * The slot shows a timestamp and, under it, the save's label. Both come from
 * the save rather than from the screen, so both are as long as the data
 * makes them: the label is whatever `save_name` held (up to the 63 bytes the
 * header stores), and the timestamp is a translated date whose length
 * depends on the language and on which weekday and month it falls on --
 * "Vendredi 11 Septembre 2026, 16:09" is wider than "Jeudi 01 Octobre 2026,
 * 13:15" by two words' worth.
 *
 * Ren'Py bounds them with the button: `gui.button_properties("slot_button")`
 * sets `xsize = gui.slot_button_width` (276) and the padding from
 * `gui.slot_button_borders` (10 each side), so the text lays out in 256px and
 * wraps. The slots are 10px apart (`gui.slot_spacing`), so anything that
 * overflows lands on the neighbouring save's thumbnail.
 *
 * The invariant tested here is the one that matters and does not depend on
 * our font metrics matching Ren'Py's glyph for glyph: **changing one slot's
 * text may not change any pixel outside that slot.** Where our line breaks
 * fall is our own business; painting on the next save is not.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "mr.h"
#include "tl.h"
#include "save.h"
#include "expr.h"
#include "interp.h"
#include "persist.h"
#include "vfs.h"
#include "script.h"
#include "image.h"
#include "scene.h"
#include "slrender.h"
#include "ui.h"

static mr_surface *g_st;
mr_surface *mr_stage(void) { return g_st; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); }
void mr_log(const char *f, ...) { (void)f; }
void mr_quit(void) {}
void mr_invalidate(void) {}
void hostshim_wave_sink(const short *d, int n) { (void)d; (void)n; }

static long T;
static int  fails;

static void check(const char *label, int ok)
{
    printf("%-54s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

/* The catalogue's name for the month it is right now.
 *
 * FileTime(1, ...) below formats the slot's own timestamp, which is the
 * moment this test wrote it, so the month in the output is whatever month
 * the machine is in. This used to be asserted as "contains one of the
 * twelve French month names", which is Miwa's language -- it fails on an
 * English-written game, and it would also have passed on a *wrong* month,
 * since any of the twelve satisfied it.
 *
 * The keys are English because Ren'Py's keys are: they come from
 * renpy/common/00action_file.rpy, so "{#month}September" is the lookup
 * whatever the game is written in. datefmt.c carries the same table for
 * the same reason. The tag comes back attached to the translation and is
 * stripped here, as datefmt.c's past_tag does.
 */
static const char *this_months_name(void)
{
    static const char *keys[12] = {
        "{#month}January", "{#month}February", "{#month}March",
        "{#month}April", "{#month}May", "{#month}June",
        "{#month}July", "{#month}August", "{#month}September",
        "{#month}October", "{#month}November", "{#month}December"
    };
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    const char *s = mr_tl_string(keys[lt ? lt->tm_mon : 0]);

    if (s && s[0] == '{' && s[1] == '#') {
        const char *e = strchr(s, '}');
        if (e)
            return e + 1;
    }
    return s;
}

static void draw(void)
{
    mr_interp_update(T); T += 33;
    mr_sl_focus_reset();
    mr_interp_render(g_st, T);
    mr_ui_draw(g_st);
}

static int find_action(const char *want, int skip)
{
    int id;
    for (id = 0; id < 200; id++) {
        const char *a = mr_sl_focus_action(id);
        if (a && strstr(a, want) && skip-- <= 0)
            return id;
    }
    return -1;
}

/* The rectangle a focus id occupies, found by hit-testing the stage. Using
 * the real hit map rather than any layout number means the box tested is the
 * one the player can click, whatever the styles worked out to. */
static int focus_box(int id, int *x0, int *y0, int *x1, int *y1)
{
    int x, y, found = 0;

    *x0 = MR_STAGE_W; *y0 = MR_STAGE_H; *x1 = -1; *y1 = -1;
    for (y = 0; y < MR_STAGE_H; y += 2) {
        for (x = 0; x < MR_STAGE_W; x += 2) {
            if (mr_sl_hit(x, y) != id)
                continue;
            found = 1;
            if (x < *x0) *x0 = x;
            if (x > *x1) *x1 = x;
            if (y < *y0) *y0 = y;
            if (y > *y1) *y1 = y;
        }
    }
    return found;
}

/* Write slot 1-1 with `label` as its name. The label travels the way the
 * game's does: save_name is read by mr_save_screenshot on the way into the
 * menu, not at the moment the slot is clicked. */
static void write_slot(const char *label)
{
    mr_value v;

    mr_value_set_str(&v, label);
    mr_var_set("save_name", &v);
    mr_save_screenshot(g_st);
    mr_save_delete("1-1");
    mr_save_write("1-1", g_st);
    mr_save_flush();
}

/* MR_DUMP=1 writes the frame to /tmp/slotfit.ppm to look at. */
static void dump(const char *path)
{
    FILE *f;
    int x, y;

    if (!getenv("MR_DUMP"))
        return;
    f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", MR_STAGE_W, MR_STAGE_H);
    for (y = 0; y < MR_STAGE_H; y++) {
        mr_u32 *row = g_st->px + (size_t)y * g_st->pitch;
        for (x = 0; x < MR_STAGE_W; x++) {
            unsigned char px[3];
            px[0] = (unsigned char)MR_R(row[x]);
            px[1] = (unsigned char)MR_G(row[x]);
            px[2] = (unsigned char)MR_B(row[x]);
            fwrite(px, 1, 3, f);
        }
    }
    fclose(f);
    printf("   wrote %s\n", path);
}

static mr_u32 *snapshot(void)
{
    size_t n = (size_t)g_st->pitch * MR_STAGE_H;
    mr_u32 *p = (mr_u32 *)malloc(n * sizeof(mr_u32));
    memcpy(p, g_st->px, n * sizeof(mr_u32));
    return p;
}

/* Pixels that differ between two frames, outside the box. */
static int differs_outside(const mr_u32 *a, const mr_u32 *b,
                           int x0, int y0, int x1, int y1,
                           int *fx, int *fy)
{
    int x, y, n = 0;

    for (y = 0; y < MR_STAGE_H; y++) {
        for (x = 0; x < MR_STAGE_W; x++) {
            size_t i = (size_t)y * g_st->pitch + x;
            if (x >= x0 && x <= x1 && y >= y0 && y <= y1)
                continue;
            if ((a[i] & 0xFFFFFF) != (b[i] & 0xFFFFFF)) {
                if (!n) { *fx = x; *fy = y; }
                n++;
            }
        }
    }
    return n;
}

int main(int argc, char **argv)
{
    mr_script *s;
    const char *game;
    int slot1, x0, y0, x1, y1;
    mr_u32 *shortshot, *longshot;

    setbuf(stdout, NULL);
    if (argc < 3) {
        fprintf(stderr, "usage: slotfittest <pak> <game-dir>\n");
        return 2;
    }
    game = argv[2];
    remove("PERSIST.SAV");

    mr_vfs_init(argv[1], game);
    mr_image_init(game);
    mr_scene_init();
    s = mr_script_load(game);
    mr_interp_init(s, game);
    mr_ui_init(game);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);

    mr_interp_start("start");
    mr_interp_main_menu();

    /* A short label first, and the slot's own rectangle. */
    write_slot("Avril 2020");
    mr_interp_show_screen("save");
    draw();

    slot1 = find_action("FileAction", 0);
    check("the save screen has slot buttons", slot1 >= 0);
    if (slot1 < 0)
        return 1;

    check("slot 1 has a rectangle", focus_box(slot1, &x0, &y0, &x1, &y1));
    printf("   slot 1 occupies %d,%d .. %d,%d  (%dx%d)\n",
           x0, y0, x1, y1, x1 - x0 + 1, y1 - y0 + 1);

    /* gui.slot_button_width is 276 and the slots are 10 apart, so a box much
     * wider than that would mean the hit map, not the text, is already
     * wrong -- and every comparison below would be measuring nothing. */
    check("  and it is about one slot wide", x1 - x0 + 1 <= 300);

    /* The slot's date, end to end: written as an epoch, formatted now with
     * the format the screen asks for, through the catalogue. Before this the
     * engine stored a preformatted "01/10/2026 16:47" and ignored `format`
     * entirely. */
    {
        mr_value v;
        memset(&v, 0, sizeof(v));
        mr_expr_eval("FileTime(1, format=_(\"{#file_time}%A %d %B %Y, %H:%M\"),"
                     " empty=_(\"emplacement vide\"))", &v);
        printf("   slot 1 date: %s\n", v.type == MR_V_STR ? v.s : "(not a string)");
        check("the date is formatted, not a stored stamp",
              v.type == MR_V_STR && strchr(v.s, '/') == NULL);
        /* The length test is not padding: strstr finds the empty string in
         * anything, so a catalogue answer of "" would make the assertion
         * below pass against any output at all. */
        check("  the catalogue has a name for this month",
              strlen(this_months_name()) >= 3);
        check("  and the date carries that name",
              v.type == MR_V_STR && strstr(v.s, this_months_name()) != NULL);
        check("  and no tag left in it",
              v.type == MR_V_STR && strchr(v.s, '{') == NULL);
    }

    draw();
    shortshot = snapshot();

    /* --- a label far too long for the slot --- */
    {
        int diff, fx = -1, fy = -1;

        /* The worst case the header can hold: 63 bytes, the widest glyph.
         * A realistic sentence wraps to two lines and fits; this is about
         * what the *format* permits, not what this game happens to set. */
        write_slot("WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW"
                   "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW");
        draw();
        draw();
        longshot = snapshot();
        dump("/tmp/slotfit.ppm");

        diff = differs_outside(shortshot, longshot, x0, y0, x1, y1, &fx, &fy);
        if (diff)
            printf("   %d pixels changed outside the slot, first at %d,%d\n",
                   diff, fx, fy);
        check("a long label changes nothing outside its own slot", diff == 0);

        /* And it really did change something *inside*, or the label never
         * reached the screen and the test above is vacuous. */
        {
            int inside = 0, x, y;
            for (y = y0; y <= y1; y++)
                for (x = x0; x <= x1; x++) {
                    size_t i = (size_t)y * g_st->pitch + x;
                    if ((shortshot[i] & 0xFFFFFF) != (longshot[i] & 0xFFFFFF))
                        inside++;
                }
            printf("   %d pixels changed inside the slot\n", inside);
            check("  and the long label did reach the screen", inside > 50);
        }
        free(longshot);
    }

    free(shortshot);
    mr_save_delete("1-1");
    remove("PERSIST.SAV");
    printf(fails ? "\n%d FAILED\n" : "\nall good\n", fails);
    return fails ? 1 : 0;
}

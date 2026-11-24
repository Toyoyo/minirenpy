/* The confirm dialog, driven the way a player drives it: find the button in
 * the real focus list, click it, find "Oui"/"Non" in the dialog that appears,
 * click that.
 *
 * Three things have to hold, and each was broken at some point while this was
 * written: the dialog draws *over* the screen that opened it rather than
 * replacing it; it is modal, so a click where a button of that screen sits
 * does nothing; and `action yes_action` runs the function Confirm was handed,
 * which is a name, not a call.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "mr.h"
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
#include "tl.h"

static mr_surface *g_st;
mr_surface *mr_stage(void) { return g_st; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); }
void mr_log(const char *f, ...) { (void)f; }
static int g_quit;
void mr_quit(void) { g_quit++; }
void mr_invalidate(void) {}
void hostshim_wave_sink(const short *d, int n) { (void)d; (void)n; }

static long T;
static int  fails;

static void draw(void)
{
    mr_interp_update(T); T += 33;
    mr_sl_focus_reset();
    mr_interp_render(g_st, T);
    mr_ui_draw(g_st);
}

static void check(const char *label, int ok)
{
    printf("%-46s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

/* The horizontal extent of the confirm frame on the stage, found by its
 * border rule (#0066CC, from gui/frame.png). Returns 0 if no frame is drawn.
 * The widest blue row is the frame's top or bottom edge; the sides are only
 * 3px, so scanning every row and keeping the longest run finds the edge
 * without needing to know where the frame landed. */
static int frame_extent(int *x0, int *x1, int *ytop)
{
    int y, best = 0;

    *ytop = 0;
    for (y = 0; y < MR_STAGE_H; y++) {
        const mr_u32 *row = g_st->px + (size_t)y * g_st->pitch;
        int x = 0;
        while (x < MR_STAGE_W) {
            int s;
            if ((row[x] & 0xFFFFFF) != 0x0066CCUL) { x++; continue; }
            s = x;
            while (x < MR_STAGE_W && (row[x] & 0xFFFFFF) == 0x0066CCUL)
                x++;
            if (x - s > best) {
                best = x - s; *x0 = s; *x1 = x - 1; *ytop = y;
            }
        }
    }
    return best;
}

/* How many horizontal bands of ink the frame contains.
 *
 * The frame's interior is solid black (gui/frame.png is a rule over black),
 * so inside it any non-black pixel is this dialog's own content and the lit
 * load screen behind the dialog cannot leak into the count. A one-line
 * message gives two bands -- the message and the button row -- and a
 * two-line message gives three.
 *
 * This is the question a width alone cannot answer: with the newline
 * dropped, the frame is still sized from the longest line and still passes
 * every width check, while the text runs together into one line that
 * overflows and is clipped at the border.
 */
static int ink_bands(void)
{
    int x0 = 0, x1 = 0, ytop = 0, ybot, y, bands = 0, inband = 0;

    if (!frame_extent(&x0, &x1, &ytop))
        return 0;

    for (ybot = MR_STAGE_H - 1; ybot > ytop; ybot--)
        if ((g_st->px[(size_t)ybot * g_st->pitch + x0] & 0xFFFFFF)
            == 0x0066CCUL)
            break;

    for (y = ytop + 5; y < ybot - 4; y++) {
        const mr_u32 *row = g_st->px + (size_t)y * g_st->pitch;
        int x, lit = 0;
        for (x = x0 + 5; x <= x1 - 5; x++) {
            if ((row[x] & 0xFFFFFF) != 0x000000UL) { lit = 1; break; }
        }
        if (lit && !inband)
            bands++;
        inband = lit;
    }
    return bands;
}

/* The focus entry whose action mentions `want`, or -1. */
/* Is slot 1-1's thumbnail the colour the stage was when it was written? The
 * thumbnail is point-sampled from the stage, so a flat fill comes back flat.
 * mr_save_flush is what makes a re-read see the new file rather than the
 * cached header from before the overwrite. */
static int thumb_is(mr_u32 want)
{
    mr_surface *th;
    mr_u32 px;
    int ok;

    mr_save_flush();
    th = mr_save_thumbnail("1-1");
    if (!th)
        return 0;
    px = th->px[(size_t)(th->h / 2) * th->pitch + th->w / 2] & 0xFFFFFF;

    /* Compared loosely, because a thumbnail is stored as 5-6-5: a channel
     * comes back within 8 of what went in, never equal to it. An exact test
     * failed on a correct save, which is the sort of thing that gets a real
     * fix reverted. */
    ok = labs((long)MR_R(px) - (long)MR_R(want)) <= 8 &&
         labs((long)MR_G(px) - (long)MR_G(want)) <= 8 &&
         labs((long)MR_B(px) - (long)MR_B(want)) <= 8;
    if (!ok)
        printf("   thumbnail %06lx, wanted about %06lx\n",
               (unsigned long)px, (unsigned long)want);
    return ok;
}

/* The slot label a save is written with, which mr_save_screenshot picks up
 * from `save_name`. Gives each save something to tell the others apart by. */
static void set_save_name(const char *s)
{
    mr_value v;
    mr_value_set_str(&v, s);
    mr_var_set("save_name", &v);
}

static int find_action(const char *want)
{
    int id;
    for (id = 0; id < 200; id++) {
        const char *a = mr_sl_focus_action(id);
        if (a && strstr(a, want))
            return id;
    }
    return -1;
}

static int click(int id)
{
    const char *a = mr_sl_focus_action(id);
    if (!a)
        return 0;
    mr_sl_focus_scope(id);
    mr_interp_action(a);
    mr_sl_focus_scope_end();
    return 1;
}

int main(int argc, char **argv)
{
    mr_script *s;
    const char *game;
    int reset_btn, yes, no, id;

    setbuf(stdout, NULL);
    if (argc < 2) {
        fprintf(stderr, "usage: confirmtest <pak> <game-dir>\n");
        return 2;
    }
    remove("PERSIST.SAV");

    /* Given the pak, so the tl/None catalogue loads and the prompts carry
     * the French the player actually sees. Without it mr_tl_string returns
     * its input and every message here is the untranslated English -- which
     * is how a frame sized wrong for the long French string passed a whole
     * suite. mr_vfs_enum cannot walk a loose directory on a host build, so
     * the pak is the only way to get a catalogue. */
    if (argc > 2) {
        mr_vfs_init(argv[1], argv[2]);
        game = argv[2];
    } else {
        mr_vfs_init(NULL, argv[1]);
        game = argv[1];
    }
    mr_image_init(game);
    mr_scene_init();
    s = mr_script_load(game);
    mr_interp_init(s, game);
    mr_ui_init(game);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);

    /* The init phase, which is where `define gui.* = ...` runs and where the
     * styles are finalised against those values. Without it the screens still
     * lay out, but every gui.* is None -- the confirm frame loses its borders
     * and its padding, and nothing says so. main.c does this before opening
     * the menu, so a harness has to as well. */
    mr_interp_start("start");
    mr_interp_main_menu();
    mr_interp_show_screen("preferences");
    draw();

    reset_btn = find_action("Confirm");
    check("found the reset button", reset_btn >= 0);
    if (reset_btn < 0)
        return 1;

    /* Something to lose, and a preference off its default. */
    mr_expr_exec("persistent.watashi = True");
    mr_interp_pref_set_fraction("music volume", 0.3);

    /* --- cancelling --- */
    click(reset_btn);
    draw();
    yes = find_action("yes_action");
    no  = find_action("no_action");
    check("the dialog is up", yes >= 0 && no >= 0);
    check("it is modal", mr_sl_modal_active());
    check("the screen behind is still drawn",
          find_action("Preference") >= 0 || find_action("Return") >= 0);

    /* The nine-slice background. `style confirm_frame` asks for
     * Frame(["gui/confirm_frame.png", "gui/frame.png"], Borders(40,40,40,40)),
     * and gui/frame.png is three rows of #0066CC over black -- so the top of
     * the dialog must be that rule and its middle must be black, at whatever
     * size the text made it. The frame is at 389,274 with the French text;
     * probe relative to nothing, just look for the rule above the vbox. */
    {
        int border = 0, centre = 0, yy;
        for (yy = 0; yy < MR_STAGE_H; yy++) {
            if ((g_st->px[yy * g_st->pitch + MR_STAGE_W / 2] & 0xFFFFFF)
                == 0x0066CCUL) {
                border = 1;
                break;
            }
        }
        centre = (g_st->px[(MR_STAGE_H / 2) * g_st->pitch + MR_STAGE_W / 2]
                  & 0xFFFFFF) == 0x000000UL;
        check("the frame border is drawn", border);
        check("and its centre is filled", centre);
    }

    /* Modality: every focusable thing must now belong to the dialog. A hit
     * anywhere either finds a dialog button or nothing. */
    {
        int leaked = 0;
        int x, y;
        for (y = 20; y < MR_STAGE_H; y += 40) {
            for (x = 20; x < MR_STAGE_W; x += 40) {
                id = mr_sl_hit(x, y);
                if (id >= 0 && id != yes && id != no)
                    leaked++;
            }
        }
        check("no click reaches the screen behind", leaked == 0);
    }

    click(no);
    draw();
    check("Non closed it", find_action("yes_action") < 0);
    check("not modal any more", !mr_sl_modal_active());
    check("the preferences screen is still there", find_action("Confirm") >= 0);
    check("nothing was reset", mr_var_get_int("persistent.watashi", 0) == 1);

    /* --- right-click is the same as Non --- */
    click(find_action("Confirm"));
    draw();
    check("dialog up again", find_action("yes_action") >= 0);
    mr_interp_game_menu_toggle();
    draw();
    check("right-click closed it", find_action("yes_action") < 0);
    check("and did not reset", mr_var_get_int("persistent.watashi", 0) == 1);

    /* --- confirming --- */
    click(find_action("Confirm"));
    draw();
    yes = find_action("yes_action");
    check("dialog up a third time", yes >= 0);
    click(yes);
    draw();
    check("Oui closed it", find_action("yes_action") < 0);
    check("the unlock is gone", mr_var_get_int("persistent.watashi", 0) == 0);
    check("we are back at the main menu",
          mr_var_get_int("main_menu", 0) == 1);
    {
        mr_value v;
        check("the volume went back to its default",
              mr_var_get("_preferences.volume.music", &v) &&
              (v.type == MR_V_FLOAT ? v.f : (double)v.i) > 0.99);
    }

    /* --- overwriting a save asks first ---
     *
     * Ren'Py's FileSave puts up layout.OVERWRITE_SAVE when the slot is
     * already taken and only writes once the player says yes
     * (00action_file.rpy:406). We used to overwrite silently.
     *
     * Driven through the same confirm screen as everything above, so this
     * also covers that the dialog's styles, hovering and modality are the
     * ones the reset prompt gets -- it is literally the same screen.
     */
    printf("\n-- overwriting a save asks first --\n");
    {
        int slot_btn, yes, no;

        mr_save_delete("1-1");
        mr_interp_main_menu();
        mr_interp_show_screen("save");
        draw();

        slot_btn = find_action("FileAction");
        check("the save screen has slot buttons", slot_btn >= 0);

        /* Each save is told apart by its thumbnail, which is taken from the
         * stage at the moment of writing -- the trick uitest already uses.
         * The slot *label* cannot serve here: it comes from `save_name` as
         * captured on entry to the game menu (mr_save_screenshot), and this
         * harness is at the main menu, where that capture does not happen.
         * An earlier version compared labels and they were all empty, so the
         * comparisons held for any behaviour at all. */
        mr_surface_fill(g_st, MR_RGB(10, 200, 30));          /* green */
        click(slot_btn);
        draw();
        check("an empty slot saves with no prompt",
              find_action("yes_action") < 0);
        check("  and the slot is written", mr_save_exists("1-1"));
        check("  with the stage it was saved from", thumb_is(0x0AC81EUL));

        /* The same slot again now asks. */
        mr_surface_fill(g_st, MR_RGB(200, 0, 0));            /* red */
        slot_btn = find_action("FileAction");
        click(slot_btn);
        draw();
        yes = find_action("yes_action");
        check("an occupied slot raises the prompt", yes >= 0);
        check("  which is modal", mr_sl_modal_active());

        /* Non leaves the old save alone. */
        no = find_action("no_action");
        check("  and offers Non", no >= 0);
        click(no);
        draw();
        check("Non closed the prompt", find_action("yes_action") < 0);
        check("  and the old save survived", thumb_is(0x0AC81EUL));

        /* Oui overwrites. */
        click(find_action("FileAction"));
        draw();
        yes = find_action("yes_action");
        check("the prompt comes back", yes >= 0);

        /* Painted here and not earlier: draw() has repainted the stage
         * several times since, and the save takes whatever is on it at the
         * moment of the write. */
        mr_surface_fill(g_st, MR_RGB(200, 0, 0));            /* red */
        click(yes);
        draw();
        check("Oui closed it", find_action("yes_action") < 0);
        check("  and the slot was overwritten", thumb_is(0xC80000UL));

        /* That the text is *translated* is not checked here: this harness is
         * given the game directory with no pak, and mr_vfs_enum does not walk
         * a loose directory on a host build, so no catalogue can load and
         * every lookup would return its input. langtest, which is handed the
         * pak, owns that check. */

        mr_save_delete("1-1");
    }

    /* --- loading asks first, but only in-game ---
     *
     * FileLoad prompts with layout.LOADING when `confirm` and not at the main
     * menu (00action_file.rpy:496); at the main menu there is nothing to
     * lose, so it loads straight away. Both halves are checked, because the
     * gate is the whole feature -- a version that always prompts would pass
     * any test that only ever looks in-game.
     */
    printf("\n-- loading asks first, in-game only --\n");
    {
        int slot_btn, yes;

        /* A variable carried in the save, so "did it load" has an answer
         * that a no-op cannot fake. main_menu will not serve: mr_save_load
         * clears it whatever the save held, so in-game it reads 0 before and
         * after and the comparison would hold for any behaviour. */
        mr_surface_fill(g_st, MR_RGB(10, 200, 30));
        mr_expr_exec("_loadmark = 7");
        mr_save_write("1-1", g_st);
        check("a save to load exists", mr_save_exists("1-1"));
        mr_expr_exec("_loadmark = 3");
        check("  and the marker in it differs from the one live",
              mr_var_get_int("_loadmark", 0) == 3);

        /* At the main menu: straight in. */
        mr_interp_main_menu();
        check("  we are at the main menu", mr_var_get_int("main_menu", 0) == 1);
        mr_interp_show_screen("load");
        draw();
        slot_btn = find_action("FileAction");
        check("the load screen has slot buttons", slot_btn >= 0);
        click(slot_btn);
        draw();
        check("no prompt at the main menu", find_action("yes_action") < 0);
        check("  and the save was loaded",
              mr_var_get_int("_loadmark", 0) == 7);

        /* In-game: asks. Start() is what clears main_menu, and the load
         * screen has to be re-shown because loading left it. */
        mr_interp_action("Start()");
        mr_expr_exec("_loadmark = 3");
        draw();
        check("we are in the story now",
              mr_var_get_int("main_menu", 0) == 0);

        mr_interp_show_screen("load");
        draw();
        slot_btn = find_action("FileAction");
        check("the load screen is up again", slot_btn >= 0);
        click(slot_btn);
        draw();
        yes = find_action("yes_action");
        check("an in-game load raises the prompt", yes >= 0);
        check("  which is modal", mr_sl_modal_active());

        /* The message is the translated one, with its embedded newline
         * surviving the catalogue. Checked as a *lookup*, not by reading the
         * screen: this harness has no pak, so mr_tl_string returns its input
         * and the assertion would be about nothing. What matters here is
         * that the string we hand mr_tl_string is spelled exactly as the
         * catalogue's `old`, so compare against that literal. */
        /* The frame hugs its longest line rather than spanning the stage.
         *
         * This message is the only two-line one in the game, and it was the
         * case that went wrong: mr_font_text_width measured across the
         * embedded newline, so the natural width came back as both sentences
         * end to end, overflowed the stage, and measure_inner fell back to
         * wrapping at the full width it had been offered. Ren'Py's frame is
         * 1044px of 1280 (comfirm-load.png); ours was the whole 1280.
         *
         * Bounded on both sides: too narrow would mean the text no longer
         * fits, and a frame merely "not the full width" would pass with any
         * arbitrary shrink. */
        {
            int fx0 = 0, fx1 = 0, fy = 0, fw = frame_extent(&fx0, &fx1, &fy);
            printf("   frame %d..%d (%d wide)\n", fx0, fx1, fw);
            check("  the frame is drawn", fw > 0);
            check("  it does not span the stage", fw < MR_STAGE_W - 40);
            check("  and is centred", abs((fx0 + fx1) / 2 - MR_STAGE_W / 2)
                                      <= 2);
            /* The real case, in the language the player sees: the French
             * message is two lines, so three bands of ink. Ren'Py's frame is
             * 1044 of 1280 (comfirm-load.png); ours drew the full 1280 when
             * the width was measured across the newline, and then drew a
             * single clipped line when the width was fixed but the draw was
             * not. Both of those pass a bare "is it narrower" check. */
            check("  the French message is on two lines", ink_bands() == 3);
            check("  and the frame is about Ren'Py's width",
                  fw > 950 && fw < 1150);
        }

        /* The catalogue really is loaded -- otherwise everything below is
         * about the English original and the long French string that caused
         * the bug never gets drawn at all. */
        {
            const char *m = mr_tl_string(
                "Loading will lose unsaved progress.\n"
                "Are you sure you want to do this?");
            printf("   message: %s\n", m);
            mr_value v;

            check("  the message is translated",
                  strstr(m, "chargement") != NULL);
            check("  and is longer than the English",
                  (int)strlen(m) > 70);

            /* It has to survive being *carried* as a value, which is how it
             * reaches the screen. At MR_VSTR_MAX 128 this exact string lost
             * its final "?" -- it is 128 bytes, so 127 were kept. The
             * catalogue is read into a plain buffer, so comparing
             * mr_tl_string's return against itself proves nothing; the value
             * round trip is the step that used to lose a byte. */
            check("  and it is still the length that overflowed a value",
                  strlen(m) >= MR_VSTR_V5);
            mr_value_set_str(&v, m);
            check("  a value carries the whole message", !strcmp(v.s, m));
            check("  including its final '?'",
                  v.s[strlen(v.s) - 1] == '?');
        }

        check("  the message carries its newline",
              strchr(mr_tl_string("Loading will lose unsaved progress.\n"
                                  "Are you sure you want to do this?"),
                     '\n') != NULL);

        /* Non leaves the story alone. */
        click(find_action("no_action"));
        draw();
        check("Non closed the prompt", find_action("yes_action") < 0);
        check("  and did not load", mr_var_get_int("_loadmark", 0) == 3);

        /* Oui loads, which brings the saved marker back. */
        click(find_action("FileAction"));
        draw();
        yes = find_action("yes_action");
        check("the prompt comes back", yes >= 0);
        click(yes);
        draw();
        check("Oui closed it", find_action("yes_action") < 0);
        check("  and the save was loaded",
              mr_var_get_int("_loadmark", 0) == 7);

        mr_save_delete("1-1");
    }

    /* --- a second line does not widen the frame ---
     *
     * The confirm frame sizes to its message. mr_font_text_width measured
     * across an embedded newline, so a two-line message came back as wide as
     * both lines laid end to end; that overflowed the stage, and measure_inner
     * fell back to wrapping at the full width it was offered. The load prompt
     * -- the only two-line message in the game, layout.LOADING at
     * 00gui.rpy:451 -- drew a frame spanning the whole 1280 where Ren'Py's is
     * 1044 (comfirm-load.png).
     *
     * Compared as widths rather than read off the stage: the screen behind
     * the dialog is still drawn and still bright, so a scan for the text
     * picks up the slot labels either side of the frame. Two frames measured
     * the same way cancel all of that out.
     */
    printf("\n-- a second line does not widen the frame --\n");
    {
        const char *l1 = "Loading will lose unsaved progress.";
        int w_one, w_two, w_long, fx0 = 0, fx1 = 0, fy = 0;
        char act[512];

        mr_interp_action("cancel");

        /* One line. */
        sprintf(act, "Confirm(\"%s\", \"cancel\", \"cancel\")", l1);
        mr_interp_action(act);
        draw();
        w_one = frame_extent(&fx0, &fx1, &fy);
        check("the one-line frame is drawn", w_one > 0);
        check("  with one line of text above the buttons", ink_bands() == 2);
        mr_interp_action("cancel");
        draw();

        /* The same line plus a shorter second one. The frame must not grow:
         * the first line is still the widest. */
        sprintf(act, "Confirm(\"%s\\nAre you sure?\", \"cancel\", \"cancel\")",
                l1);
        mr_interp_action(act);
        draw();
        w_two = frame_extent(&fx0, &fx1, &fy);
        printf("   one line %d wide, two lines %d wide\n", w_one, w_two);
        check("the two-line frame is drawn", w_two > 0);
        check("  and is no wider than the one-line frame", w_two == w_one);
        check("  and still fits on the stage", w_two < MR_STAGE_W);
        /* The newline has to survive all the way to the glyphs, not just to
         * the measurement -- the frame was sized from the longest line while
         * the draw ran both lines together and clipped the overflow. */
        check("  and the message really is on two lines", ink_bands() == 3);
        mr_interp_action("cancel");
        draw();

        /* ...but the frame does track its message, so the equality above is
         * not just a constant width. A longer single line must be wider. */
        sprintf(act, "Confirm(\"%s %s\", \"cancel\", \"cancel\")", l1, l1);
        mr_interp_action(act);
        draw();
        w_long = frame_extent(&fx0, &fx1, &fy);
        printf("   a doubled line is %d wide\n", w_long);
        check("a longer message gets a wider frame", w_long > w_one);
        mr_interp_action("cancel");
        draw();
    }

    /* --- quitting asks first, but only in-game ---
     *
     * Quit() with no argument confirms when `(not main_menu) and
     * _confirm_quit` (00action_menu.rpy:265). This game's navigation screen
     * supplies the gate itself as `Quit(confirm=not main_menu)`
     * (screens.rpy:337), so clicking the real button exercises the explicit
     * argument; the bare Quit() below exercises the default.
     */
    printf("\n-- quitting asks first, in-game only --\n");
    {
        int quit_btn, yes;

        mr_interp_main_menu();
        draw();
        quit_btn = find_action("Quit");
        check("the navigation has a Quit button", quit_btn >= 0);

        g_quit = 0;
        click(quit_btn);
        draw();
        check("no prompt at the main menu", find_action("yes_action") < 0);
        check("  and it quit", g_quit == 1);

        /* In-game. */
        mr_interp_action("Start()");
        mr_interp_game_menu_toggle();
        draw();
        quit_btn = find_action("Quit");
        check("the game menu has one too", quit_btn >= 0);

        g_quit = 0;
        click(quit_btn);
        draw();
        yes = find_action("yes_action");
        check("an in-game quit raises the prompt", yes >= 0);
        check("  and has not quit yet", g_quit == 0);
        check("  the prompt is modal", mr_sl_modal_active());

        click(find_action("no_action"));
        draw();
        check("Non closed the prompt", find_action("yes_action") < 0);
        check("  and still has not quit", g_quit == 0);

        click(find_action("Quit"));
        draw();
        click(find_action("yes_action"));
        draw();
        check("Oui quit", g_quit == 1);
        check("  and closed the prompt", find_action("yes_action") < 0);

        /* The default when no argument is given: Ren'Py's own rule, which
         * the screens here never exercise. In-game it must still ask. */
        g_quit = 0;
        mr_interp_action("Quit()");
        draw();
        check("bare Quit() in-game asks", find_action("yes_action") >= 0);
        check("  without quitting", g_quit == 0);
        click(find_action("no_action"));
        draw();

        /* ...and _confirm_quit=False turns it off, as it does during the
         * splashscreen (00start.rpy:196). */
        mr_expr_exec("_confirm_quit = False");
        g_quit = 0;
        mr_interp_action("Quit()");
        draw();
        check("_confirm_quit=False skips the prompt",
              find_action("yes_action") < 0);
        check("  and quits", g_quit == 1);
        mr_expr_exec("_confirm_quit = True");
    }

    remove("PERSIST.SAV");
    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}

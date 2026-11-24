/* input.c - turning keys and clicks into what the game does about them.
 *
 * Lifted out of main.c's wnd_proc unchanged, comments and ordering included.
 * The ordering in particular is not arbitrary: nearly every comment in this
 * file records a bug that came from doing two of these checks the other way
 * round, and the ones about where mr_interp_skip_stop goes are the worst of
 * them.
 *
 * Nothing here knows what a message is. See input.h.
 */

#include <string.h>

#include "mr.h"
#include "input.h"
#include "keys.h"
#include "plat.h"
#include "scale.h"     /* mr_fit_map_point: output pixel -> stage pixel */
#include "interp.h"
#include "expr.h"      /* _preferences.display, for the fullscreen toggle */
#include "slrender.h"
#include "ui.h"

/* The focus id currently being dragged (a bar or a scrollbar), or -1. */
static int g_dragging = -1;

void mr_input_mouse_move(int ox, int oy)
{
    int sx, sy;

    if (mr_fit_map_point(ox, oy, &sx, &sy)) {
        /* A bar or scrollbar being dragged keeps the pointer even when it
         * wanders off the control, which is what makes a slider usable. */
        if (g_dragging >= 0) {
            mr_sl_focus_drag(g_dragging, sx, sy);
            mr_invalidate();
        }
        /* The rhythm game's give-up dialog is drawn natively and has no
         * focus entry, so it tracks the pointer itself; when it is up it
         * owns the motion, exactly as it owns the click below. */
        else if (mr_interp_rhythm_hover(sx, sy)) {
            mr_ui_set_hover(-1);
            mr_ui_set_menu_hover(-1);
        }
        else {
            mr_ui_set_hover(mr_sl_hit(sx, sy));
            mr_ui_set_menu_hover(mr_ui_menu_hit(sx, sy));
        }
    } else if (g_dragging < 0) {
        mr_ui_set_hover(-1);
        mr_ui_set_menu_hover(-1);
    }
}

void mr_input_mouse_up(int button)
{
    if (button != MR_MB_LEFT)
        return;

    if (g_dragging >= 0) {
        g_dragging = -1;
        mr_plat_capture_mouse(0);
    }
}

void mr_input_wheel(int notches, int ox, int oy)
{
    int sx, sy;

    if (!mr_fit_map_point(ox, oy, &sx, &sy))
        return;

    if (mr_sl_scroll_at(sx, sy, notches))
        mr_invalidate();
    else if (notches > 0) {
        /* Wheel up over the story rolls back, as in Ren'Py. There is no
         * wheel on Windows 3.1, so Backspace and Page Up below are what the
         * target actually uses. */
        mr_interp_rollback();
        mr_invalidate();
    }
    else if (notches < 0) {
        /* And wheel down rolls *forward*: `rollforward` includes
         * 'mousedown_5' (00keymap.rpy:66). Not "advance" -- with no rollback
         * outstanding this does nothing, which is what Ren'Py does. */
        mr_interp_rollforward();
        mr_invalidate();
    }
}

void mr_input_mouse_down(int button, int ox, int oy)
{
    int sx, sy;

    if (button == MR_MB_RIGHT) {
        /* Ren'Py's right-click opens the game menu, and closes it again. */
        mr_ui_set_hover(-1);
        mr_interp_game_menu_toggle();
        mr_invalidate();
        return;
    }

    if (button != MR_MB_LEFT)
        return;

    if (mr_fit_map_point(ox, oy, &sx, &sy)) {
        int id;
        const char *act;

        /* A click on a lane is a tap on that lane's note, and the whole
         * column is active. Checked before the focus list, which the
         * minigame does not populate. */
        if (mr_interp_rhythm_click(sx, sy)) {
            mr_invalidate();
            return;
        }

        /* A choice menu is drawn over the scene rather than as a screen, so
         * it is hit-tested here; without this the click fell through to
         * mr_interp_advance and the menu could only be answered with the
         * number keys. */
        {
            int choice = mr_ui_menu_hit(sx, sy);
            if (choice >= 0) {
                mr_ui_set_menu_hover(-1);
                mr_interp_choose(choice);
                mr_invalidate();
                return;
            }
        }

        id = mr_sl_hit(sx, sy);

        if (mr_sl_focus_draggable(id)) {
            /* Press sets the value straight away, then the pointer is
             * captured so a drag keeps tracking. */
            mr_sl_focus_drag(id, sx, sy);
            mr_invalidate();
            g_dragging = id;
            mr_plat_capture_mouse(1);
            return;
        }

        act = mr_sl_focus_action(id);
        if (act) {
            /* A click on a button runs its action instead of advancing the
             * story. */
            mr_ui_set_hover(-1);
            mr_sl_focus_scope(id);
            mr_interp_action(act);
            mr_sl_focus_scope_end();
            mr_invalidate();
            return;
        }
    }

    /* A click on the scene ends a toggled skip: the player reaching for the
     * mouse is the player wanting to read again.
     *
     * This belongs *here*, on the path that advances the story, and not at
     * the top of the handler where it used to be. Ren'Py puts it in
     * SayBehavior.event (behavior.py:773), the invisible widget that advances
     * dialogue, and that only acts when it is the focused thing -- so a click
     * consumed by a button never reaches it. Doing it before the dispatch
     * instead made the quick menu's "Avance rapide" impossible to switch off:
     * the click cancelled the skip, then Skip() found no skip in progress and
     * started another one, so the button toggled on for ever. */
    mr_interp_skip_stop();

    mr_interp_advance();
    mr_invalidate();
}

void mr_input_focus_lost(void)
{
    mr_interp_skip_hold(0);
}

void mr_input_key_up(int key)
{
    if (key == MR_KEY_CTRL) {
        mr_interp_skip_hold(0);
        mr_invalidate();
    }
}

void mr_input_key_down(int key)
{
    /* 'r' restores the window to its configured size. First, ahead of
     * everything including the minigame, because it is a window hotkey and
     * not a game input: it changes nothing the player is reading, so nothing
     * should be able to claim it.
     *
     * It used to sit just below the minigame check, which was wrong in a way
     * that only showed up in one place. mr_rhythm_key returns 1 for *every*
     * key while its give-up dialog is up, and again on the score summary --
     * deliberately, so a stray Space cannot advance the story behind the
     * dialog -- so 'r' was swallowed there and nowhere else. Letting those
     * states fall through instead would have fixed the symptom and reopened
     * that hole.
     *
     * Not running mr_interp_skip_stop below is also deliberate: ending a
     * toggled skip because someone resized a window would be surprising.
     *
     * Safe as a bare letter only because no letter is otherwise bound outside
     * the minigame's lanes (arrows, A/Q/S, numpad -- not 'r') and there is no
     * text entry anywhere in the engine; no character input is handled at
     * all. A save-name field would have to take this back. */
    if (key == 'R') {
        mr_plat_restore_size();
        return;
    }

    /* Fullscreen, here for the same reason: a display hotkey, not a game
     * input, so nothing downstream may claim it. Ren'Py binds bare 'f' as
     * well as F11, which is only safe here because the engine has no text
     * entry -- see the note on 'r' above.
     *
     * Alt+Enter is the other half. It arrives by whatever route the platform
     * uses for a key pressed with Alt held -- on Win32 that is a *system*
     * key and a different message -- and the platform turns it into a call to
     * mr_input_fullscreen_toggle below rather than into a key down, because
     * Alt+Enter is not "the Enter key" as far as the rest of this file is
     * concerned. */
    if (key == MR_KEY_F11 || key == 'F') {
        mr_input_fullscreen_toggle();
        return;
    }

    /* The minigame takes the keyboard while it is running: the arrows are its
     * lanes, and Escape asks whether to give up rather than closing the
     * game. */
    if (mr_interp_rhythm_key(key)) {
        mr_invalidate();
        return;
    }

    /* A screen's own `key "K_SPACE" action ...` comes first. Ren'Py resolves
     * a screen's bindings before the default keyboard behaviour, and only a
     * keysym some visible screen actually bound can match, so this is inert
     * for screens that bind nothing.
     *
     * This is the table that turns a key code into the Ren'Py keysym name the
     * screen wrote; the lookup and the running of the action are in interp.c.
     */
    {
        static const struct { int key; const char *sym; } keysyms[] = {
            { MR_KEY_SPACE,    "K_SPACE"     },
            { MR_KEY_RETURN,   "K_RETURN"    },
            { MR_KEY_ESCAPE,   "K_ESCAPE"    },
            { MR_KEY_BACKSPACE,"K_BACKSPACE" },
            { MR_KEY_TAB,      "K_TAB"       },
            { MR_KEY_LEFT,     "K_LEFT"      },
            { MR_KEY_RIGHT,    "K_RIGHT"     },
            { MR_KEY_UP,       "K_UP"        },
            { MR_KEY_DOWN,     "K_DOWN"      },
            { MR_KEY_PAGEUP,   "K_PAGEUP"    },
            { MR_KEY_PAGEDOWN, "K_PAGEDOWN"  },
            { MR_KEY_HOME,     "K_HOME"      },
            { MR_KEY_END,      "K_END"       },
            { MR_KEY_DELETE,   "K_DELETE"    }
        };
        int i;

        for (i = 0; i < (int)(sizeof(keysyms) / sizeof(keysyms[0])); i++) {
            if (key != keysyms[i].key)
                continue;
            if (mr_interp_screen_key(keysyms[i].sym)) {
                mr_ui_set_hover(-1);
                mr_invalidate();
                return;
            }
            break;
        }
    }

    /* Ctrl skips while held, Tab toggles -- Ren'Py's bindings. Both come
     * before the rest: Ctrl is not otherwise used, and Tab would otherwise
     * fall through to nothing. */
    if (key == MR_KEY_CTRL) {
        mr_interp_skip_hold(1);
        mr_invalidate();
        return;
    }
    if (key == MR_KEY_TAB) {
        mr_interp_skip_toggle();
        mr_invalidate();
        return;
    }

    /* Any other key is the player taking the wheel back. Ren'Py ends a
     * toggled skip on the next click or keypress too; without this, Tab would
     * be the only way out of one. */
    mr_interp_skip_stop();

    if (key == MR_KEY_ESCAPE) {
        /* Ren'Py binds Escape and the right-click to the same "game_menu"
         * keysym, and the help screen says so: "Echap. -- Ouvre le menu du
         * jeu." It used to close the window instead. */
        mr_ui_set_hover(-1);
        mr_interp_game_menu_toggle();
    }
    else if (key == MR_KEY_SPACE || key == MR_KEY_RETURN)
        mr_interp_advance();
    else if (key == MR_KEY_BACKSPACE || key == MR_KEY_PAGEUP)
        mr_interp_rollback();
    /* Page Down is `rollforward`, the mirror of Page Up (00keymap.rpy:66).
     * It does nothing unless a rollback is outstanding -- it is not a second
     * way to advance. */
    else if (key == MR_KEY_PAGEDOWN)
        mr_interp_rollforward();
    else if (key >= '1' && key <= '9')
        mr_interp_choose(key - '1');
    mr_invalidate();
}

/* F11, Alt+Enter and bare 'f', which is Ren'Py's `toggle_fullscreen`:
 *
 *     toggle_fullscreen = [ 'alt_K_RETURN', 'alt_K_KP_ENTER', 'K_F11',
 *                           'noshift_K_f' ]          00keymap.rpy:42
 *
 * This writes the *preference* rather than calling the platform, which is the
 * whole trick: the frame loop already polls `_preferences.display` once a
 * frame and turns it into a call to mr_plat_set_fullscreen, so writing the
 * variable is enough to change the display, light the right radio button on
 * the preferences screen and have the choice persisted -- all three for free,
 * and with no second path that could disagree with the first.
 */
void mr_input_fullscreen_toggle(void)
{
    mr_value v;
    int full = mr_var_get("_preferences.display", &v) &&
               v.type == MR_V_STR && !strcmp(v.s, "fullscreen");

    mr_value_set_str(&v, full ? "window" : "fullscreen");
    mr_var_set("_preferences.display", &v);
    mr_invalidate();
}

/* platw32.c - the Win32s platform: a window, a message pump, and a clock.
 *
 * The implementation of plat.h for Windows 3.1x + Win32s, and the only file
 * in the engine that creates a window or looks at a message. What a keypress
 * *means* is not here -- that is input.c, which this file feeds. The split
 * is the whole point: this is translation, and it should stay thin enough to
 * read in one sitting.
 *
 * Windows 3.1 multitasks cooperatively, so mr_plat_yield gives the rest of
 * the slice back rather than spinning. A game that never yields makes the
 * whole system unusable, which on Win32s includes our own sound driver.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <mmsystem.h>  /* timeGetTime */

#include "mr.h"
#include "plat.h"
#include "input.h"
#include "keys.h"
#include "present.h"
#include "presw32.h"
#include "movie.h"     /* a cutscene repaints itself; see WM_PAINT */
#include "audio.h"
#include "config.h"

/* The window class name. Private to this process, so its only visible effect
 * is on a tool that enumerates windows -- which is why it is named for the
 * engine rather than for whichever game is being shipped. */
#define MR_CLASS "mrpyWnd"

/* Only alive while the window is being dragged or resized; see WM_SYSCOMMAND. */
#define MR_DRAG_TIMER 1

/* keys.h promises its codes are the Win32 virtual keys. If that ever stops
 * being true the translation below becomes a silent mis-mapping -- Page Up
 * doing what Home does -- so it is checked here instead, where both headers
 * are in scope. A failing line declares an array of negative size and the
 * build stops on it.
 *
 * C89, so no static_assert. The typedef name says which key broke. */
typedef char mr_keychk_backspace[MR_KEY_BACKSPACE == VK_BACK   ? 1 : -1];
typedef char mr_keychk_tab      [MR_KEY_TAB       == VK_TAB    ? 1 : -1];
typedef char mr_keychk_return   [MR_KEY_RETURN    == VK_RETURN ? 1 : -1];
typedef char mr_keychk_shift    [MR_KEY_SHIFT     == VK_SHIFT  ? 1 : -1];
typedef char mr_keychk_ctrl     [MR_KEY_CTRL      == VK_CONTROL? 1 : -1];
typedef char mr_keychk_alt      [MR_KEY_ALT       == VK_MENU   ? 1 : -1];
typedef char mr_keychk_escape   [MR_KEY_ESCAPE    == VK_ESCAPE ? 1 : -1];
typedef char mr_keychk_space    [MR_KEY_SPACE     == VK_SPACE  ? 1 : -1];
typedef char mr_keychk_pageup   [MR_KEY_PAGEUP    == VK_PRIOR  ? 1 : -1];
typedef char mr_keychk_pagedown [MR_KEY_PAGEDOWN  == VK_NEXT   ? 1 : -1];
typedef char mr_keychk_end      [MR_KEY_END       == VK_END    ? 1 : -1];
typedef char mr_keychk_home     [MR_KEY_HOME      == VK_HOME   ? 1 : -1];
typedef char mr_keychk_left     [MR_KEY_LEFT      == VK_LEFT   ? 1 : -1];
typedef char mr_keychk_up       [MR_KEY_UP        == VK_UP     ? 1 : -1];
typedef char mr_keychk_right    [MR_KEY_RIGHT     == VK_RIGHT  ? 1 : -1];
typedef char mr_keychk_down     [MR_KEY_DOWN      == VK_DOWN   ? 1 : -1];
typedef char mr_keychk_delete   [MR_KEY_DELETE    == VK_DELETE ? 1 : -1];
typedef char mr_keychk_numpad1  [MR_KEY_NUMPAD1   == VK_NUMPAD1? 1 : -1];
typedef char mr_keychk_numpad2  [MR_KEY_NUMPAD2   == VK_NUMPAD2? 1 : -1];
typedef char mr_keychk_f11      [MR_KEY_F11       == VK_F11    ? 1 : -1];

static HWND      g_hwnd;
static HINSTANCE g_inst;
static int       g_closed;

/* --- the clock ----------------------------------------------------------- */

unsigned int mr_plat_ticks(void)
{
    return (unsigned int)timeGetTime();
}

/* Win32s maps Sleep onto the Win16 scheduler's yield, which is what keeps the
 * rest of the system -- including the sound driver -- responsive. */
void mr_plat_yield(void)
{
    Sleep(1);
}

/* Nothing to do here. Windows draws the mouse pointer itself, so a long
 * image decode does not freeze it, and the audio runway is a second deep --
 * which is what covers the one thing a load could otherwise starve.
 *
 * Deliberately *not* a message pump. It is called from inside a PNG decode,
 * and dispatching a message there would run the window procedure -- and so
 * input.c, and so the screen system -- in the middle of loading the image
 * that screen is waiting for. The DOS side has the same rule for the same
 * reason; see plat.h. */
void mr_plat_service(void)
{
}

/* --- the display preference ---
 *
 * `Preference("display", "fullscreen")`. Win32s has no display-mode switching
 * to speak of -- ChangeDisplaySettings is a Win95/NT call, and on Windows 3.1
 * the resolution is whatever the driver was configured with at setup. So
 * "fullscreen" here is a borderless window covering the screen, which is what
 * the player actually wants from it anyway.
 *
 * Nothing else has to change: the stage stays 1280x720 and present.c already
 * fits the largest 16:9 rectangle into the client area and letterboxes the
 * rest, so a 640x480 or 800x600 desktop is handled by the code that was
 * already there. SetWindowPos raises WM_SIZE, which is what re-sizes the
 * presentation buffers.
 *
 * The windowed rectangle is saved rather than recomputed from mrpy.ini, so
 * coming back out of fullscreen returns the window to where the player had
 * put it rather than to where it started.
 */
static int  g_fullscreen;
static RECT g_windowed_rect;
static int  g_have_windowed_rect;

/* The client size mrpy.ini asked for, kept so the 'r' hotkey can put the
 * window back to it. Set once in WinMain, next to the CreateWindow that first
 * uses it; 0 before that, which mr_plat_restore_size treats as "no idea" and
 * declines rather than guessing. */
static int g_cfg_w, g_cfg_h;

void mr_plat_set_fullscreen(int on)
{
    if (!g_hwnd || on == g_fullscreen)
        return;

    if (on) {
        GetWindowRect(g_hwnd, &g_windowed_rect);
        g_have_windowed_rect = 1;

        /* WS_VISIBLE has to be kept: the window is already on screen, and
         * dropping it here leaves a window that is shown but not visible. */
        SetWindowLong(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_hwnd, NULL, 0, 0,
                     GetSystemMetrics(SM_CXSCREEN),
                     GetSystemMetrics(SM_CYSCREEN),
                     SWP_NOZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLong(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        if (g_have_windowed_rect)
            SetWindowPos(g_hwnd, NULL,
                         g_windowed_rect.left, g_windowed_rect.top,
                         g_windowed_rect.right - g_windowed_rect.left,
                         g_windowed_rect.bottom - g_windowed_rect.top,
                         SWP_NOZORDER | SWP_FRAMECHANGED);
        else
            SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0,
                         SWP_NOZORDER | SWP_NOMOVE | SWP_NOSIZE |
                         SWP_FRAMECHANGED);
    }

    g_fullscreen = on;
    mr_log("display: %s", on ? "borderless fullscreen" : "windowed");
    mr_invalidate();
}

/* 'r' puts the window back to the client size mrpy.ini asked for, which is
 * the only way to undo a drag-resize exactly -- dragging back by hand lands a
 * pixel or two out, and present.c then letterboxes to keep 16:9, so the
 * picture is very slightly smaller than it was. The position is left alone:
 * the player put the window there on purpose, and only the size was asked
 * about.
 *
 * Windowed only, as asked. In fullscreen the size is the screen and there is
 * nothing to restore to, so this declines silently rather than dropping out
 * of fullscreen as a side effect of a key that does not say it does that.
 *
 * Not a Ren'Py binding -- Ren'Py has no 'r' -- so it is listed under
 * "Deliberate departures" in NOTES.
 */
void mr_plat_restore_size(void)
{
    RECT r;

    if (!g_hwnd || g_fullscreen || g_cfg_w <= 0 || g_cfg_h <= 0)
        return;

    /* A maximized window keeps its maximized size whatever SetWindowPos is
     * told, so it has to come out of that first. Without this, 'r' on a
     * maximized window logs success and changes nothing. */
    if (IsZoomed(g_hwnd))
        ShowWindow(g_hwnd, SW_RESTORE);

    /* AdjustWindowRect with the creation style, matching WinMain: the ini
     * figure is the *client* area and the frame goes on top of it. Passing
     * the live GWL_STYLE would differ only by WS_VISIBLE, which it ignores,
     * but this way the two places that size this window agree by
     * construction. */
    r.left = 0;
    r.top = 0;
    r.right = g_cfg_w;
    r.bottom = g_cfg_h;
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);

    /* SWP_NOMOVE keeps the position. SetWindowPos raises WM_SIZE, which is
     * what resizes the presentation buffers -- same path as leaving
     * fullscreen. */
    SetWindowPos(g_hwnd, NULL, 0, 0,
                 r.right - r.left, r.bottom - r.top,
                 SWP_NOZORDER | SWP_NOMOVE);

    mr_log("display: window client area restored to %dx%d", g_cfg_w, g_cfg_h);
    mr_invalidate();
}

void mr_plat_capture_mouse(int on)
{
    if (!g_hwnd)
        return;
    if (on)
        SetCapture(g_hwnd);
    else
        ReleaseCapture();
}

void mr_plat_set_title(const char *text)
{
    if (g_hwnd && text && *text)
        SetWindowText(g_hwnd, text);
}

/* No watchpoint here. See plat.h: the bug it exists for has appeared on
 * one DOS machine only, and a second implementation would be guesswork
 * with nothing to test it against. */
void mr_plat_watch_write(const void *addr)
{
    (void)addr;
}

void mr_plat_fatal(const char *msg)
{
    MessageBox(g_hwnd, msg, MR_TITLE " - error", MB_OK | MB_ICONSTOP);

    /* exit() runs no window-destruction path, and Windows 3.1 is not NT: a
     * handle the task still owns at termination is not reliably reclaimed.
     * The caller has already given back the wave device and the palette --
     * the wave device being the worse of the two to leak, as it locks the
     * sound driver against every other application until reboot -- so what
     * is left is the window and its class.
     *
     * After this the window still exists but has nothing left to paint from;
     * mr_present returns early once the presentation buffer is gone. */
    if (g_hwnd) {
        DestroyWindow(g_hwnd);
        g_hwnd = NULL;
    }
    /* The class has to go after the window and not before: a class with a
     * live window of its own still on screen cannot be unregistered. USER
     * does reclaim classes at task exit, unlike GDI objects, so this is
     * insurance rather than a known leak -- but a class lives in USER's 64K
     * local heap, which is the scarcest thing on this machine, and it costs
     * one call to not have to trust the Win32s shim about it. */
    UnregisterClass(MR_CLASS, g_inst);
}

/* --- the window procedure ---
 *
 * Translation only. Every case either maintains the window itself or hands
 * an input to input.c; none of them decides what the game does about it.
 */
static LONG CALLBACK wnd_proc(HWND hwnd, UINT msg, UINT wp, LONG lp)
{
    PAINTSTRUCT ps;
    HDC hdc;

    switch (msg) {
    case WM_CREATE:
        return 0;

    case WM_SIZE:
        mr_present_resize(LOWORD(lp), HIWORD(lp));
        mr_invalidate();
        return 0;

    case WM_ERASEBKGND:
        /* We repaint the whole client area ourselves, including letterbox
         * bars, so letting GDI erase first would only cause flicker. */
        return 1;

    /* BeginPaint/EndPaint still bracket this, to validate the update region;
     * mr_present acquires its own DC inside. Without the bracketing the
     * region stays dirty and Windows sends WM_PAINT again for ever. */
    case WM_PAINT:
        BeginPaint(hwnd, &ps);
        /* ShowWindow raises this before mr_main_startup has allocated the
         * stage, so there is a genuine window of a second or more -- the pak
         * is being mounted and the script parsed -- with nothing to paint
         * from.
         *
         * Black it in by hand. The class background brush is BLACK_BRUSH and
         * would do it, but WM_ERASEBKGND below claims the message and so the
         * brush is never used; that is right once there is a stage, because
         * mr_present covers every pixel itself, and wrong before there is
         * one. Skipping the paint entirely leaves whatever was on screen
         * under the window -- on Windows 3.1, the desktop pattern.
         *
         * A cutscene is asked first, and must be. main.c stops compositing
         * the stage for the duration -- the video is written straight into
         * the presenter's picture buffer -- so the stage here still holds
         * the scene from before the cutscene began. Painting it puts the old
         * background and the quick menu over the video.
         *
         * That showed up two ways on the real machine. As a flicker on an
         * activate or an uncover, where one WM_PAINT paints the scene and
         * the next decoded frame paints over it again; and much worse during
         * a drag-resize, where the whole drag showed the scene. The second
         * is the instructive one: a resize runs a modal loop inside
         * DefWindowProc, so the application's message loop never gets a
         * turn, no frame is decoded, and WM_PAINT is the only thing drawing
         * at all.
         *
         * mr_movie_repaint decodes nothing -- it re-converts the frame that
         * is already on screen -- so it is safe at any rate Windows asks for
         * and cannot disturb playback. */
        if (mr_movie_repaint())
            ;                           /* the cutscene owns the screen */
        else if (mr_stage())
            mr_present(mr_stage());
        else
            FillRect(ps.hdc, &ps.rcPaint,
                     (HBRUSH)GetStockObject(BLACK_BRUSH));
        EndPaint(hwnd, &ps);
        return 0;

    /* On a 256-colour display the game owns a 216-entry colour cube plus a
     * grey ramp. Windows hands the physical palette to the foreground window,
     * so we re-realize whenever it changes hands. */
    case WM_QUERYNEWPALETTE:
        hdc = GetDC(hwnd);
        mr_present_realize(hdc, 0);
        ReleaseDC(hwnd, hdc);
        InvalidateRect(hwnd, NULL, FALSE);
        return 1;

    case WM_PALETTECHANGED:
        if ((HWND)wp == hwnd)
            return 0;           /* our own realization caused this */
        hdc = GetDC(hwnd);
        mr_present_realize(hdc, 1);
        ReleaseDC(hwnd, hdc);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_CLOSE:
        g_closed = 1;
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_MOUSEMOVE:
        mr_input_mouse_move(LOWORD(lp), HIWORD(lp));
        return 0;

    case WM_RBUTTONDOWN:
        /* Win32s does not reliably give an unfocused window keyboard input
         * after a click, so take focus explicitly. */
        SetFocus(hwnd);
        mr_input_mouse_down(MR_MB_RIGHT, LOWORD(lp), HIWORD(lp));
        return 0;

    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        mr_input_mouse_down(MR_MB_LEFT, LOWORD(lp), HIWORD(lp));
        return 0;

    case WM_LBUTTONUP:
        mr_input_mouse_up(MR_MB_LEFT);
        return 0;

    /* Windows 3.1 has no wheel, so this only ever arrives on a modern host.
     * The scrollbar is the control that has to work on the real target.
     *
     * The coordinates are screen-relative here, unlike every other mouse
     * message, which is why this one converts. */
    case WM_MOUSEWHEEL: {
        POINT pt;
        pt.x = LOWORD(lp);
        pt.y = HIWORD(lp);
        ScreenToClient(hwnd, &pt);
        mr_input_wheel((short)HIWORD(wp) / 120, pt.x, pt.y);
        return 0;
    }

    case WM_KEYDOWN:
        mr_input_key_down((int)wp);
        return 0;

    case WM_KEYUP:
        mr_input_key_up((int)wp);
        return 0;

    /* Losing the keyboard with Ctrl down would otherwise leave the skip held
     * forever: no KEYUP is coming, because the key goes up in another
     * window. */
    case WM_KILLFOCUS:
        mr_input_focus_lost();
        return 0;

    /* Alt+Enter. A keypress with Alt held is a *system* key and arrives here,
     * not in WM_KEYDOWN, so binding it there would have looked right and done
     * nothing. Returning 0 also stops DefWindowProc treating it as a menu
     * gesture and beeping.
     *
     * The numeric-keypad Enter is the same virtual key as the main one under
     * Windows -- they are told apart only by the extended-key bit in lParam,
     * which there is no reason to look at here -- so Ren'Py's separate
     * 'alt_K_KP_ENTER' binding comes along for free. */
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN) {
            mr_input_fullscreen_toggle();
            return 0;
        }
        break;

    /* Dragging or resizing the window hands control to a modal loop inside
     * DefWindowProc, which does not return until the mouse is released. Our
     * frame loop is the only thing that feeds the mixer, so the queued audio
     * -- a runway's worth, about a second -- ran out shortly into the drag
     * and the music stopped until the drag finished.
     *
     * A timer is enough to fix it: that modal loop dispatches messages, so
     * WM_TIMER still arrives and can top the mixer up. The timer only exists
     * for the duration of the drag, which is why it is started and stopped
     * around the DefWindowProc call rather than for the life of the window.
     *
     * SC_MOVE/SC_SIZE rather than WM_ENTERSIZEMOVE deliberately: the latter
     * is a Win32 message that Windows 3.1's USER never sends, so it would do
     * nothing on the actual target. Dragging the title bar arrives here too,
     * as DefWindowProc turns WM_NCLBUTTONDOWN into SC_MOVE.
     *
     * 50ms against a queue of over a second is a wide margin, and just as
     * well: the timer resolution here is about 55ms and nothing guarantees
     * the drag loop is prompt about dispatching. */
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MOVE || (wp & 0xFFF0) == SC_SIZE) {
            LONG r;
            SetTimer(hwnd, MR_DRAG_TIMER, 50, NULL);
            r = DefWindowProc(hwnd, msg, wp, lp);
            KillTimer(hwnd, MR_DRAG_TIMER);
            return r;
        }
        break;

    case WM_TIMER:
        if (wp == MR_DRAG_TIMER) {
            mr_audio_poll();
            return 0;
        }
        break;

    default:
        break;
    }

    return DefWindowProc(hwnd, msg, wp, lp);
}

int mr_plat_pump(void)
{
    MSG msg;

    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT)
            return 0;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return !g_closed;
}

int PASCAL WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    WNDCLASS wc;
    RECT r;

    (void)prev;

    g_inst = inst;

    /* Before the window, because the window's size comes out of it. */
    mr_config_load(MR_CONFIG_FILE);

    memset(&wc, 0, sizeof(wc));
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = (WNDPROC)wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIcon(inst, "APPICON");
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = MR_CLASS;

    if (!RegisterClass(&wc)) {
        MessageBox(NULL, "RegisterClass failed", MR_TITLE, MB_OK);
        return 1;
    }

    /* Size the window so the *client* area is what the ini asked for, the
     * border and caption being added on top of it by AdjustWindowRect. The
     * stage stays 1280x720 whatever this is: the window is a scaled view of
     * it, so an odd size letterboxes rather than changing the layout. */
    r.left = 0;
    r.top = 0;
    r.right = (int)mr_config_int("width", MR_WIN_W, 160, 4096);
    r.bottom = (int)mr_config_int("height", MR_WIN_H, 90, 4096);

    /* Kept for the 'r' hotkey. Recorded here rather than re-read from the ini
     * later so that the two can never disagree -- the clamping and the
     * fallback have already been applied to these. */
    g_cfg_w = r.right;
    g_cfg_h = r.bottom;

    mr_log("main: window client area %dx%d", r.right, r.bottom);
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);

    g_hwnd = CreateWindow(MR_CLASS, MR_TITLE,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top,
        NULL, NULL, inst, NULL);

    if (!g_hwnd) {
        MessageBox(NULL, "CreateWindow failed", MR_TITLE, MB_OK);
        return 1;
    }

    if (!mr_present_init(g_hwnd))
        mr_fatal("presentation init failed");

    GetClientRect(g_hwnd, &r);
    mr_present_resize(r.right, r.bottom);

    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);

    if (!mr_main_startup(cmdline))
        return 1;

    mr_main_loop();
    mr_main_shutdown();

    DestroyWindow(g_hwnd);
    g_hwnd = NULL;
    UnregisterClass(MR_CLASS, inst);
    return 0;
}

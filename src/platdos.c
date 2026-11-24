/* platdos.c - the DOS platform: a keyboard, a mouse, a clock and main().
 *
 * The implementation of plat.h for 32-bit protected-mode DOS under DOS/32A,
 * and the counterpart of platw32.c. Like that file it is translation only:
 * what a keypress *means* is input.c's, which knows nothing about scancodes.
 *
 * Everything here talks to hardware the BIOS would rather talk to for us, and
 * in each case for a reason:
 *
 *   keyboard   int 16h cannot report a key going *up*, and the engine needs
 *              that -- Ctrl held is skip, and a Ctrl that never comes up is a
 *              game that skips for ever. So int 9, read port 0x60, and
 *              translate at pump time.
 *
 *   timer      the BIOS tick is 18.2Hz, or 55ms. Transitions are timed in
 *              milliseconds and the profiler divides by this clock, so the
 *              PIT is reprogrammed to 1kHz and the BIOS handler is chained at
 *              its original rate so the time of day stays right.
 *
 *   mouse      int 33h, but by motion counters rather than by position. The
 *              driver has no idea what a VESA mode is and reports absolute
 *              coordinates in the system of whatever mode it last understood,
 *              which on a 1280x720 screen is wrong in a way that looks like a
 *              broken pointer. Mickeys are mickeys whatever the mode.
 *
 *              Polled, from mr_plat_pump and from mr_plat_service. The
 *              driver's own event handler (function 0Ch, through a DPMI
 *              real-mode callback) was written, tried on both targets and
 *              removed: it works until it is delivered while the engine is
 *              inside a DOS call, and then the extender deadlocks. NOTES
 *              has the full account under "Drawing a pointer nobody gave
 *              us" -- read it before writing that code a second time.
 *
 * Every interrupt function in this file and in wavedos.c is declared
 * `__interrupt __loadds`, and the second keyword is not decoration. A
 * protected-mode interrupt handler is entered with CS and SS:ESP set by the
 * DPMI host and **DS undefined** -- the spec says so in as many words, and a
 * DPMI real-mode callback is worse than undefined: it is entered with DS:ESI
 * deliberately pointing at the real-mode stack. Without `__loadds` the
 * handler reads and writes its own globals through whatever selector
 * happened to be there.
 *
 * It cost a crash to find, in the mouse callback that has since been
 * removed, and the crash did not look like this at all: DOSBox-X died with
 * "JMP Illegal descriptor type" from inside the mouse driver, because the
 * handler had scribbled through the wrong selector. The reason it is worth
 * stating here is that a handler without `__loadds` very often *appears* to
 * work -- under a flat model DS is usually the same selector as everything
 * else -- so the two that remain could run for a long time before meeting a
 * host that enters them differently.
 *
 * The interrupt handlers are *not* locked in memory with DPMI 0600h. Under
 * DOS/32A and DOS/4GW nothing pages, so there is nothing to lock against;
 * under a paging host (CWSDPMI, a DOS box inside Windows) a handler that
 * touches a swapped-out page faults inside an interrupt, which is a hang with
 * no message. That is a known gap rather than an oversight -- getting the
 * extent of a function portably enough to lock it is its own small mess --
 * and audiodos.c will have to solve it properly, since an audio IRQ runs far
 * more often than these two and carries a buffer with it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <conio.h>
#include <dos.h>
#include <i86.h>

#include "mr.h"
#include "plat.h"
#include "input.h"
#include "keys.h"
#include "present.h"
#include "presdos.h"    /* the pointer: this platform has to draw its own */
#include "scale.h"      /* mr_fit_get: the picture, for clamping the mouse */
#include "audio.h"      /* mr_audio_poll: keeping the card fed during a load */
#include "dosmem.h"     /* mr_dpmi_lock_image, before any vector is hooked */
#include "config.h"

/* --- the clock -------------------------------------------------------------
 *
 * Channel 0 of the 8254 at 1193182Hz / 1193 = 1000.15Hz. One count per
 * millisecond, near enough that the error is a second and a half a day and
 * nothing here measures anything for a day.
 *
 * The BIOS wants its 18.2Hz. Rather than call it every tick -- which would
 * run the time of day 55 times fast -- each of our ticks adds the divisor to
 * an accumulator and the BIOS handler is chained whenever that passes 65536,
 * which is exactly the number of PIT counts the original divisor stood for.
 */
#define PIT_DIV 1193

static volatile unsigned long g_ms;
static unsigned long          g_bios_accum;
static void (__interrupt __far *g_old_tick)(void);
static int                    g_tick_hooked;

static void __interrupt __loadds tick_isr(void)
{
    g_ms++;
    g_bios_accum += PIT_DIV;
    if (g_bios_accum >= 0x10000UL) {
        g_bios_accum -= 0x10000UL;
        /* Chaining does not come back, and the BIOS handler signals the end
         * of interrupt for us. */
        _chain_intr(g_old_tick);
    }
    outp(0x20, 0x20);
}

static void pit_divisor(unsigned div)
{
    outp(0x43, 0x36);                       /* channel 0, lo/hi, mode 3 */
    outp(0x40, (int)(div & 0xFF));
    outp(0x40, (int)((div >> 8) & 0xFF));
}

unsigned int mr_plat_ticks(void)
{
    return (unsigned int)g_ms;
}

/* DPMI 2F/1680h: give the rest of the slice back. On bare DOS nothing is
 * listening and it returns immediately, which is what plat.h asks for; inside
 * a Windows DOS box or DOSBox it is the difference between the host being
 * usable and not. */
void mr_plat_yield(void)
{
    union REGS r;

    memset(&r, 0, sizeof(r));
    r.w.ax = 0x1680;
    int386(0x2F, &r, &r);
}

/* --- the keyboard ----------------------------------------------------------
 *
 * The handler does as little as a handler can: one port read, one ring push,
 * one end-of-interrupt. Everything that could be slow or could call back into
 * the engine happens in mr_plat_pump, on the frame loop's own stack.
 *
 * It does *not* chain to the BIOS handler. Chaining would mean the BIOS
 * reading port 0x60 again -- it has already been emptied -- and filling a
 * keystroke buffer nothing reads. The cost is that Ctrl+Alt+Del and
 * Ctrl+Break do nothing while the game is up, and that the BIOS shift and
 * lock flags at 0040:0017 stand still, which is why NumLock is tracked here
 * rather than read from them each time.
 */
#define KQ_SIZE 64                          /* a power of two */

static volatile unsigned char g_kq[KQ_SIZE];
static volatile int           g_kq_head, g_kq_tail;
static void (__interrupt __far *g_old_kbd)(void);
static int                    g_kbd_hooked;

static void __interrupt __loadds kbd_isr(void)
{
    int next;
    unsigned char sc = (unsigned char)inp(0x60);

    next = (g_kq_head + 1) & (KQ_SIZE - 1);
    if (next != g_kq_tail) {                /* a full queue drops, silently */
        g_kq[g_kq_head] = sc;
        g_kq_head = next;
    }
    outp(0x20, 0x20);
}

/* Set 1 make codes to MR_KEY_*, for the keys the engine binds. 0 means "not
 * a key we have a name for", which is most of them.
 *
 * The numeric keypad is the entry with a decision in it. A bare 0x4B is the
 * keypad 4, and whether that means "4" or "left arrow" depends on NumLock;
 * the grey cursor keys send the same code behind an E0 prefix. Both cases are
 * handled, the first through this table and the second through the E0 one
 * below, which is exactly how Windows tells them apart and so keeps
 * rhythm.c's numpad bindings and the arrow keys both working.
 */
static const short g_sc[128] = {
    /* 00 */ 0, MR_KEY_ESCAPE, '1', '2', '3', '4', '5', '6',
    /* 08 */ '7', '8', '9', '0', 0, 0, MR_KEY_BACKSPACE, MR_KEY_TAB,
    /* 10 */ 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',
    /* 18 */ 'O', 'P', 0, 0, MR_KEY_RETURN, MR_KEY_CTRL, 'A', 'S',
    /* 20 */ 'D', 'F', 'G', 'H', 'J', 'K', 'L', 0,
    /* 28 */ 0, 0, MR_KEY_SHIFT, 0, 'Z', 'X', 'C', 'V',
    /* 30 */ 'B', 'N', 'M', 0, 0, 0, MR_KEY_SHIFT, 0,
    /* 38 */ MR_KEY_ALT, MR_KEY_SPACE, 0, MR_KEY_F1, 0, 0, 0, 0,
    /* 40 */ 0, 0, 0, 0, 0, 0, 0, MR_KEY_NUMPAD7,
    /* 48 */ MR_KEY_NUMPAD8, MR_KEY_NUMPAD9, 0, MR_KEY_NUMPAD4,
    /* 4C */ MR_KEY_NUMPAD5, MR_KEY_NUMPAD6, 0, MR_KEY_NUMPAD1,
    /* 50 */ MR_KEY_NUMPAD2, MR_KEY_NUMPAD3, MR_KEY_NUMPAD0, MR_KEY_DELETE,
    /* 54 */ 0, 0, 0, MR_KEY_F11,
    /* 58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 78 */ 0, 0, 0, 0, 0, 0, 0, 0
};

/* The same codes behind an E0 prefix: the grey block, and the keypad's own
 * Enter and the right-hand Ctrl and Alt. Anything not listed is ignored. */
static int extended_key(unsigned char sc)
{
    switch (sc) {
    case 0x1C: return MR_KEY_RETURN;        /* keypad Enter */
    case 0x1D: return MR_KEY_CTRL;          /* right Ctrl */
    case 0x38: return MR_KEY_ALT;           /* right Alt */
    case 0x47: return MR_KEY_HOME;
    case 0x48: return MR_KEY_UP;
    case 0x49: return MR_KEY_PAGEUP;
    case 0x4B: return MR_KEY_LEFT;
    case 0x4D: return MR_KEY_RIGHT;
    case 0x4F: return MR_KEY_END;
    case 0x50: return MR_KEY_DOWN;
    case 0x51: return MR_KEY_PAGEDOWN;
    case 0x52: return 0;                    /* Insert: nothing binds it */
    case 0x53: return MR_KEY_DELETE;
    default:   return 0;
    }
}

/* NumLock off turns the keypad into cursor keys, as it does everywhere else.
 * Tracked here because the handler does not chain and so the BIOS flag at
 * 0040:0017 is frozen for the life of the game; it is read once at startup
 * for the initial state, which the low megabyte being identity-mapped under
 * DOS/32A makes a plain dereference. */
static int g_numlock;

static int keypad_alias(int key)
{
    switch (key) {
    case MR_KEY_NUMPAD7: return MR_KEY_HOME;
    case MR_KEY_NUMPAD8: return MR_KEY_UP;
    case MR_KEY_NUMPAD9: return MR_KEY_PAGEUP;
    case MR_KEY_NUMPAD4: return MR_KEY_LEFT;
    case MR_KEY_NUMPAD6: return MR_KEY_RIGHT;
    case MR_KEY_NUMPAD1: return MR_KEY_END;
    case MR_KEY_NUMPAD2: return MR_KEY_DOWN;
    case MR_KEY_NUMPAD3: return MR_KEY_PAGEDOWN;
    case MR_KEY_NUMPAD5: return 0;
    case MR_KEY_NUMPAD0: return 0;          /* Insert */
    default:             return key;
    }
}

/* Drain the ring and dispatch. The E0 prefix is a state here rather than in
 * the handler, so the handler stays three statements long and the queue
 * carries raw bytes exactly as the controller produced them. */
static void pump_keyboard(void)
{
    static int e0;
    unsigned char sc;
    int key, up;

    while (g_kq_tail != g_kq_head) {
        sc = g_kq[g_kq_tail];
        g_kq_tail = (g_kq_tail + 1) & (KQ_SIZE - 1);

        if (sc == 0xE0) {
            e0 = 1;
            continue;
        }
        /* 0xE1 introduces the Pause sequence, which is six bytes long and
         * means nothing to us. Swallowing just the prefix leaves the rest to
         * be read as ordinary keys, so the whole thing is skipped by
         * remembering nothing and letting the five stragglers map to keys the
         * table has no name for. */
        if (sc == 0xE1) {
            e0 = 0;
            continue;
        }

        up = (sc & 0x80) != 0;
        sc &= 0x7F;

        if (e0) {
            key = extended_key(sc);
            e0 = 0;
        } else {
            key = g_sc[sc];
            if (sc == 0x45 && !up) {        /* NumLock itself */
                g_numlock = !g_numlock;
                continue;
            }
            if (!g_numlock)
                key = keypad_alias(key);
        }

        if (!key)
            continue;

        if (up)
            mr_input_key_up(key);
        else
            mr_input_key_down(key);
    }
}

/* --- the mouse -------------------------------------------------------------
 *
 * int 33h, which is a driver and may not be there. Functions 0 (reset) and
 * 0Bh (read the motion counters) and 3 (buttons) take and return registers
 * only, so they can be called straight through int386 without a real-mode
 * buffer -- which is worth insisting on, given that both of this port's
 * run-time faults so far came from handing the BIOS a pointer.
 *
 * Position is integrated from mickeys rather than read with function 3,
 * because function 3 answers in the coordinate system of a mode the driver
 * understands and it does not understand a 1280x720 VESA mode. A mickey is
 * about 1/200 inch whatever is on screen. `mousespeed` in mrpy.ini scales
 * them, as a percentage, for someone whose pointer crawls or flies.
 */
static int g_mouse;                         /* a driver answered */
static int g_mspeed = 100;

/* The picture rectangle, refreshed once a frame. The event handler needs it
 * to clamp and cannot call mr_fit_get from interrupt time -- not because
 * that function is unsafe, but because reading four statics the frame loop
 * may be writing is exactly the kind of thing that works until it does
 * not. A frame stale is harmless: the fit only changes at startup. */
static int g_px, g_py, g_pw, g_ph;

/* Written by the event handler, read by the frame loop. The handler does two
 * things and no more: it moves the *picture* of the pointer, and it records
 * what happened for the frame loop to deliver. Nothing in input.c is called
 * from interrupt time -- hover and click run the whole screen system, and
 * that has no business happening in the middle of a PNG decode. */
static volatile int g_mx, g_my;
static volatile int g_mbuttons;             /* the current button mask */
static volatile int g_press, g_release;     /* edges since the last pump */
static volatile int g_press_x, g_press_y;   /* where the press happened */

static int mouse_reset(void)
{
    union REGS r;

    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0000;
    int386(0x33, &r, &r);
    return r.w.ax == 0xFFFF;
}

/* Poll, for when there is no callback. Identical arithmetic, once a frame. */
static void poll_mouse(void)
{
    union REGS r;
    int dx, dy, x, y, buttons;

    memset(&r, 0, sizeof(r));
    r.w.ax = 0x000B;                /* read and clear the motion counters */
    int386(0x33, &r, &r);
    dx = (int)(short)r.w.cx;
    dy = (int)(short)r.w.dx;

    x = g_mx + dx * g_mspeed / 100;
    y = g_my + dy * g_mspeed / 100;
    if (x < g_px)            x = g_px;
    else if (x >= g_px + g_pw) x = g_px + g_pw - 1;
    if (y < g_py)            y = g_py;
    else if (y >= g_py + g_ph) y = g_py + g_ph - 1;
    g_mx = x;
    g_my = y;

    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0003;
    int386(0x33, &r, &r);
    buttons = r.w.bx & 3;

    g_press   |= buttons & ~g_mbuttons;
    g_release |= ~buttons & g_mbuttons;
    if (buttons & ~g_mbuttons) {
        g_press_x = x;
        g_press_y = y;
    }
    g_mbuttons = buttons;
}

/* Deliver whatever the handler recorded. This is where input.c is called
 * from, and the only place: everything above only moves numbers and pixels.
 *
 * The latches are read and cleared with interrupts off, because the handler
 * writes them and a press that arrived between the read and the clear would
 * otherwise be dropped -- rare, and so the sort of thing that would show up
 * as "sometimes a click does nothing". */
static void pump_mouse(void)
{
    static int last_x = -1, last_y = -1;
    int press, release, x, y, pressx, pressy;

    if (!g_mouse)
        return;

    /* Clamped to the *picture*, not to the screen. Two reasons, and the
     * second is the one that decided it: every control the game has is
     * inside the 16:9 rectangle, so the bars are dead space; and the
     * presenter composites the pointer out of its scaled buffer, which only
     * covers the picture, so a pointer in the bars would be an invisible
     * one. Confining it is better than letting it disappear. */
    mr_fit_get(&g_px, &g_py, &g_pw, &g_ph, NULL, NULL);

    poll_mouse();

    _disable();
    press     = g_press;
    release   = g_release;
    pressx    = g_press_x;
    pressy    = g_press_y;
    x         = g_mx;
    y         = g_my;
    g_press   = 0;
    g_release = 0;
    _enable();

    if (x != last_x || y != last_y) {
        mr_input_mouse_move(x, y);
        last_x = x;
        last_y = y;
    }

    /* Down before up, so a click and release inside one frame still arrives
     * as a click rather than as a stray release. */
    if (press & 1)
        mr_input_mouse_down(MR_MB_LEFT, pressx, pressy);
    if (press & 2)
        mr_input_mouse_down(MR_MB_RIGHT, pressx, pressy);
    if (release & 1)
        mr_input_mouse_up(MR_MB_LEFT);
    if (release & 2)
        mr_input_mouse_up(MR_MB_RIGHT);

    /* And catch the pointer up if the handler declined to draw it because a
     * present was running. */
    mr_presdos_cursor_move(x, y);
}

/* Called from inside a long operation -- in practice from png.c, every few
 * scanlines of a decode. See plat.h.
 *
 * This is what actually fixes the pointer freezing during a load, and it is
 * worth saying why it beats the interrupt-driven answer that was tried
 * first: it runs on the frame loop's own stack, between two rows of a PNG,
 * where nothing is half-written and no DOS call is in progress. There is no
 * reentrancy to reason about at all.
 *
 * It polls and moves and does nothing else. Buttons are latched by
 * poll_mouse and delivered by the next pump, so a click during a load is
 * not lost and is not acted on at a moment when the screen it would hit is
 * still being built. */
void mr_plat_service(void)
{
    static unsigned int last;
    unsigned int now;

    /* Rate-gated, at 100Hz, and that is what makes it safe to sprinkle.
     * Reading int 33h is two calls through the extender's real-mode switch
     * -- cheap next to a decode, not cheap next to a scanline -- so the
     * callers do not have to judge how often to call this. They call it
     * whenever they are about to be slow and this decides.
     *
     * mr_plat_ticks is a variable read, so the common case costs a compare.
     * The wrap at 2^32 takes care of itself: the subtraction is done in
     * unsigned arithmetic and only the difference is looked at. */
    now = mr_plat_ticks();
    if (now - last < 10)
        return;
    last = now;

    if (g_mouse) {
        poll_mouse();
        mr_presdos_cursor_move(g_mx, g_my);
    }

    /* And refill the sound card's queue.
     *
     * The queue is a second of audio deep precisely so that a slow frame
     * does not reach the speaker, but a single PNG decode on this target
     * can be longer than the whole runway -- a 7-second run on a Sound
     * Blaster Pro logged 2043ms of inserted silence, most of it while the
     * frame loop was somewhere inside startup. No runway can be made deep
     * enough for a loop that stops; the fix is to not stop.
     *
     * This is a different claim from the mouse one above and worth stating
     * separately. The mixer is not touched by the interrupt handler -- the
     * handler only takes buffers off a queue -- so calling it here races
     * with nothing, and the callers of mr_plat_service are all between
     * operations rather than inside one. mr_audio_poll's own guard covers
     * the case this file cannot see: a caller that is itself downstream of
     * the mixer. */
    mr_audio_poll();
}

int mr_plat_pump(void)
{
    pump_keyboard();
    pump_mouse();

    /* There is no close box and no window manager: the only way out is the
     * script's Quit(), which does not come through here. */
    return 1;
}

/* --- the rest of plat.h ---------------------------------------------------
 *
 * Four of these have nothing to do on a machine the game owns outright. They
 * are not stubs that should be filled in later; there is genuinely nothing
 * for them to do, and saying so here is what keeps the core from having to
 * ask which platform it is on.
 */

void mr_plat_set_fullscreen(int on)
{
    /* Always fullscreen: the output is a video mode. The core calls this
     * every frame with the current preference and expects the platform to
     * ignore the ones that change nothing, which here is all of them. */
    (void)on;
}

void mr_plat_capture_mouse(int on)
{
    /* Nothing can take the pointer away from us, so a drag already keeps
     * receiving motion. */
    (void)on;
}

void mr_plat_restore_size(void)
{
    /* The 'r' hotkey. There is no window to resize, so it is inert. */
}

void mr_plat_set_title(const char *text)
{
    /* Nowhere to put it. */
    (void)text;
}

/* --- coming up and going down --------------------------------------------- */

static void hook_handlers(void)
{
    g_old_kbd = _dos_getvect(0x09);
    _dos_setvect(0x09, kbd_isr);
    g_kbd_hooked = 1;

    g_old_tick = _dos_getvect(0x08);
    _dos_setvect(0x08, tick_isr);
    g_tick_hooked = 1;
    pit_divisor(PIT_DIV);
}

/* Put the machine back. Safe to call twice and safe to call when nothing was
 * hooked, because it is registered with atexit as well as being called on the
 * way out: a vector left pointing into a program that is no longer there is a
 * locked-up machine, and that must not depend on reaching the end of main.
 *
 * The timer divisor goes back before the vector, so there is no window in
 * which the BIOS handler is running at 1kHz. */
static void unhook_handlers(void)
{
    if (g_tick_hooked) {
        pit_divisor(0);                     /* 0 means 65536: 18.2Hz */
        _dos_setvect(0x08, g_old_tick);
        g_tick_hooked = 0;
    }
    if (g_kbd_hooked) {
        _dos_setvect(0x09, g_old_kbd);
        g_kbd_hooked = 0;
    }
}

void mr_plat_watch_write(const void *addr)
{
    mr_dpmi_watch_write(addr);
}

void mr_plat_fatal(const char *msg)
{
    /* The caller has already given the audio device and the presenter back,
     * and giving the presenter back is what returned the screen to text mode
     * -- so there is somewhere to print this. The handlers have not been
     * given back yet, and must be before anything is read or printed. */
    unhook_handlers();

    fprintf(stderr, "\n%s: %s\n", MR_TITLE, msg);
    fflush(stderr);
}

int main(int argc, char **argv)
{
    int px, py, w, h;

    /* Before anything is hooked: if the machine is left with our vectors in
     * it, nothing else matters. */
    atexit(unhook_handlers);

    /* The ini is read first only so that `dpmilock` below can be read from
     * it. Nothing else here depends on the order. */
    mr_config_load(MR_CONFIG_FILE);

    /* Nothing a handler touches may be pageable -- a page fault taken
     * inside an interrupt is a hang with no log. See dosmem.h. Nothing
     * pages under DOS/32A, so this is insurance against being run
     * somewhere that does.
     *
     * `dpmilock=0` turns it off, and the reason is a real machine: on
     * FreeDOS with Jemm the whole address space is 53MB, and this asks the
     * host to pin 9MB of it before anything else has run. A host that
     * mishandles that is a host where the engine reads back memory it
     * never wrote. Insurance is not worth a machine that cannot start, so
     * it can be declined. */
    if (mr_config_int("dpmilock", 1, 0, 1))
        mr_dpmi_lock_image();
    else
        mr_log("dpmi: locking declined by mrpy.ini");

    mr_dpmi_report_memory();

    g_mspeed = (int)mr_config_int("mousespeed", 100, 10, 1000);

    if (!mr_present_init(NULL)) {
        fprintf(stderr, "%s: no usable video mode. Run MODES.EXE to see "
                        "what this card offers.\n", MR_TITLE);
        return 1;
    }

    /* The presenter sized itself to the mode it chose; ask the fit where the
     * picture ended up, so the pointer starts in the middle of it. */
    mr_fit_get(&px, &py, &w, &h, NULL, NULL);

    /* Start the pointer in the middle, as a driver would, and tell the engine
     * where it is so the first frame hovers correctly rather than waiting for
     * the player to jog the mouse. Nothing is on screen yet, so the pointer
     * itself is not drawn until the first frame has been. */
    g_mouse = mouse_reset();
    if (g_mouse) {
        g_px = px; g_py = py; g_pw = w; g_ph = h;
        g_mx = px + w / 2;
        g_my = py + h / 2;
        mr_input_mouse_move(g_mx, g_my);
    } else {
        mr_log("mouse: no int 33h driver; keyboard only");
    }

    /* 0040:0017 bit 5 is NumLock. The low megabyte is identity-mapped under
     * DOS/32A, which is the same assumption vbedos.c makes of the real-mode
     * scratch buffer. */
    g_numlock = (*(volatile unsigned char *)0x417 & 0x20) != 0;

    hook_handlers();

    if (!mr_main_startup(argc > 1 ? argv[1] : "")) {
        unhook_handlers();
        mr_present_shutdown();
        return 1;
    }

    mr_main_loop();
    mr_main_shutdown();

    unhook_handlers();
    return 0;
}

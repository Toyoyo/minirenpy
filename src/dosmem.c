/* dosmem.c - see dosmem.h. DOS only; a do-nothing build elsewhere.
 *
 * DPMI 0100h/0101h and 0600h/0601h by hand rather than through Watcom's
 * _dos_allocmem/_dos_freemem. The library pair is where the segment and the
 * selector got interchanged: 0100h hands back both -- AX a real-mode
 * segment, DX a protected-mode selector -- and 0101h frees the selector.
 * Freeing a segment as though it were a selector releases a descriptor
 * belonging to something else, and the symptom is a program that produces
 * its entire output and then hangs on exit, because the arena is only walked
 * when the process terminates.
 *
 * Note throughout: Watcom's int386 does **not** return the carry flag, so
 * `if (r.w.cflag)` after one of these is reading an uninitialised field.
 * Every call below judges its result by value instead.
 */

#include <stdlib.h>
#include <string.h>

#include "mr.h"
#include "dosmem.h"

#if defined(__DOS__) || defined(MR_TARGET_DOS)

#include <i86.h>
#include <dos.h>

int mr_dos_alloc(mr_dos_block *b, unsigned paras)
{
    union REGS r;

    b->seg = 0;
    b->sel = 0;

    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0100;
    r.w.bx = (unsigned short)paras;
    int386(0x31, &r, &r);

    /* No carry to read, so the result is judged by value. A real-mode
     * segment is below the 640K line and above the interrupt table; a DPMI
     * error code (0x8011 and friends) is neither, and nor is zero. */
    if (r.w.ax < 0x0040 || r.w.ax >= 0xA000)
        return 0;

    b->seg = r.w.ax;
    b->sel = r.w.dx;
    return 1;
}

void mr_dos_free(mr_dos_block *b)
{
    union REGS r;

    if (!b->sel)
        return;

    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0101;
    r.w.dx = (unsigned short)b->sel;    /* the selector, not the segment */
    int386(0x31, &r, &r);

    b->seg = 0;
    b->sel = 0;
}

unsigned long mr_dos_linear(const mr_dos_block *b)
{
    return (unsigned long)b->seg << 4;
}

long mr_dos_conventional_free(void)
{
    union REGS r;

    /* FFFFh paragraphs is just under a megabyte, so this cannot succeed;
     * DPMI 0100h then returns the largest block it *could* have given, in
     * paragraphs, in BX. Carry is the reliable signal here -- unlike in
     * mr_dos_alloc, where the value is judged instead because a success
     * has to be told from an error code, there is no success to tell
     * apart. */
    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0100;
    r.w.bx = 0xFFFF;
    int386(0x31, &r, &r);

    if (!r.w.cflag) {
        /* It cannot have worked. If a host claims it did, give the block
         * back rather than leak most of a megabyte to produce a log
         * line. */
        mr_dos_block b;

        b.seg = r.w.ax;
        b.sel = r.w.dx;
        mr_dos_free(&b);
        return -1;
    }

    return (long)r.w.bx * 16L;
}

int mr_dpmi_lock(void *addr, unsigned long len)
{
    union REGS r;
    unsigned long lin = (unsigned long)addr;

    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0600;
    r.w.bx = (unsigned short)(lin >> 16);
    r.w.cx = (unsigned short)(lin & 0xFFFF);
    r.w.si = (unsigned short)(len >> 16);
    r.w.di = (unsigned short)(len & 0xFFFF);
    int386(0x31, &r, &r);

    /* 0600h returns nothing but a flag, which cannot be read here. AX is
     * left holding the function number on success and a DPMI error code on
     * failure, and the error codes are all 0x80xx, so that is the test. It
     * is a weak one; a host that failed the call another way would look like
     * a success. The consequence of believing a lie here is a page fault in
     * an interrupt handler under a paging host, which is why the caller logs
     * the answer rather than only acting on it. */
    return (r.w.ax & 0xFF00) != 0x8000;
}

void mr_dpmi_unlock(void *addr, unsigned long len)
{
    union REGS r;
    unsigned long lin = (unsigned long)addr;

    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0601;
    r.w.bx = (unsigned short)(lin >> 16);
    r.w.cx = (unsigned short)(lin & 0xFFFF);
    r.w.si = (unsigned short)(len >> 16);
    r.w.di = (unsigned short)(len & 0xFFFF);
    int386(0x31, &r, &r);
}

/* --- locking the program ------------------------------------------------
 *
 * The extents come from the linker, which is the only thing that knows
 * them. OpenWatcom defines `__begtext` at the front of the text segment,
 * `_end` past the end of BSS, and `_STACKLOW`/`_STACKTOP` around the
 * stack; all four were checked to resolve under wlink for the dos32a
 * target before this was written.
 *
 * Taking the extent from the linker rather than from markers around the
 * handlers is the whole reason this is worth having. The usual trick --
 * a dummy function after each handler, locking from one to the other --
 * needs the linker to keep them adjacent and breaks silently the day
 * somebody adds a handler outside the brackets. A hang on a machine with
 * no debugger is the wrong failure mode to design in. Locking the lot
 * costs a few hundred kilobytes of pageability on a host that pages, and
 * nothing at all on one that does not.
 *
 * Addresses are used as linear addresses, which is true in the zero-based
 * flat model DOS/32A sets up and is already assumed by every other lock
 * in this file.
 */
extern char __begtext[];
extern char _end[];

/* These two are *variables holding* the stack bounds, not the bounds
 * themselves -- the map puts them four bytes apart in _DATA. Declared as
 * arrays, as everything else here is, they lock four bytes of nothing and
 * report a stack of 0K, which is exactly what the first version of this
 * did on the machine. Read their values. */
extern unsigned long _STACKLOW;
extern unsigned long _STACKTOP;

int mr_dpmi_lock_image(void)
{
    /* Code, then DGROUP, laid out contiguously: `_end` is the end of BSS
     * and the stack lives past it, so this is everything except the stack
     * and the heap. On this build it is about 8MB, nearly all of it BSS. */
    unsigned long image = (unsigned long)(_end - __begtext);
    unsigned long stack = _STACKTOP - _STACKLOW;
    int ok = 1;

    if (!mr_dpmi_lock(__begtext, image)) {
        mr_log("dpmi: could not lock the %luK image at %p",
               image / 1024UL, (void *)__begtext);
        ok = 0;
    }

    /* The stack matters as much as the code: a hardware interrupt may be
     * taken on whatever stack was current, however deep into the engine
     * that is. A DPMI host is supposed to switch to a locked stack of its
     * own for hardware interrupts and the ones here do, but that is the
     * host's promise rather than something observable from in here, and
     * the cost of not relying on it is one more call. */
    /* A sanity check on the two values, because they are read out of the
     * runtime rather than computed here, and a wrong pair would either
     * lock nothing (the bug this replaces) or hand the host an enormous
     * bogus range. The stack is a megabyte by `option stack` in
     * Makefile.dos; anything far from that is not the stack. */
    if (stack < 4096UL || stack > 16UL * 1024 * 1024) {
        mr_log("dpmi: stack bounds look wrong (%08lx..%08lx); not locking "
               "them", _STACKLOW, _STACKTOP);
        ok = 0;
    } else if (!mr_dpmi_lock((void *)_STACKLOW, stack)) {
        mr_log("dpmi: could not lock the %luK stack at %08lx",
               stack / 1024UL, _STACKLOW);
        ok = 0;
    }

    if (ok)
        mr_log("dpmi: locked %luK of image at %08lx and %luK of stack at "
               "%08lx against a paging host",
               image / 1024UL, (unsigned long)__begtext,
               stack / 1024UL, _STACKLOW);

    return ok;
}

/* --- the debug registers, through DPMI ----------------------------------- */

static int  g_watch;        /* handle from 0B00h, plus one */

int mr_dpmi_watch_write(const void *addr)
{
    union REGS r;
    unsigned long lin = (unsigned long)addr;

    mr_dpmi_unwatch();

    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0B00;
    r.w.bx = (unsigned short)(lin >> 16);
    r.w.cx = (unsigned short)(lin & 0xFFFF);
    r.h.dl = 4;                 /* four bytes */
    r.h.dh = 1;                 /* break on write */
    int386(0x31, &r, &r);

    if (r.w.cflag) {
        mr_log("dpmi: no watchpoint available for %08lx", lin);
        return 0;
    }

    g_watch = (int)r.w.bx + 1;
    mr_log("dpmi: watching writes to %08lx (handle %d)", lin, g_watch - 1);
    return 1;
}

void mr_dpmi_unwatch(void)
{
    union REGS r;

    if (!g_watch)
        return;

    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0B01;
    r.w.bx = (unsigned short)(g_watch - 1);
    int386(0x31, &r, &r);
    g_watch = 0;
}

/* What the host has, and where it put us. Separate from the locking above
 * and called whether or not that ran, because the first thing wanted when
 * a DOS build misbehaves is the memory picture, and `dpmilock=0` is one of
 * the things one would be testing at the time.
 *
 * Two malloc probes rather than one: the question when a pointer looks
 * wrong is not only "is that the heap" but "where does the heap start and
 * which way does it grow", and a small and a large request need not come
 * from the same place. Both are freed again.
 *
 * It exists because of the FreeDOS 1.4 fault, where a script text pointer
 * read back as an address inside the image, below anywhere malloc returns
 * on the machines where the game runs. */
void mr_dpmi_report_memory(void)
{
    void *small = malloc(32);
    void *large = malloc(256UL * 1024);
    unsigned long freemem[12];
    union REGS  r;
    struct SREGS s;
    long conv;

    mr_log("mem: image %08lx..%08lx, malloc 32 at %p, 256K at %p",
           (unsigned long)__begtext, (unsigned long)_end, small, large);
    free(large);
    free(small);

    /* DPMI 0500h: how much the host will actually part with. Worth a line
     * because DOS has no other way to ask, and because an engine that
     * peaks in the tens of megabytes cares about the answer -- but read it
     * together with how much RAM the machine has. During the FreeDOS hunt
     * this number went from 53MB to 245MB between two runs and the
     * difference was the VM being resized, not anything the engine or the
     * host did. A figure with no baseline invites exactly that mistake.
     *
     * The first field of the 48-byte block is the largest available free
     * block in bytes. The rest are page counts that hosts fill in
     * inconsistently and are not worth printing. */
    memset(&r, 0, sizeof(r));
    memset(&freemem, 0, sizeof(freemem));
    segread(&s);
    r.w.ax = 0x0500;
    s.es = FP_SEG(&freemem);
    r.x.edi = (unsigned long)FP_OFF(&freemem);
    int386x(0x31, &r, &r, &s);
    if (r.w.cflag)
        mr_log("mem: the host will not say how much memory is free");
    else
        mr_log("mem: largest free block %luK", freemem[0] / 1024UL);

    /* Below the 640K line, which is a different and much scarcer thing:
     * all the engine wants of it is the DMA ring, but DOS/32A and the stub
     * have already taken theirs by the time this runs. Logged again after
     * the ring is allocated, in wavedos.c. */
    conv = mr_dos_conventional_free();
    if (conv < 0)
        mr_log("mem: the host will not say how much conventional memory "
               "is free");
    else
        mr_log("mem: conventional %ldK free before the DMA ring",
               conv / 1024L);
}

#else  /* not DOS */

int mr_dos_alloc(mr_dos_block *b, unsigned paras)
{
    (void)paras;
    b->seg = 0;
    b->sel = 0;
    return 0;
}

void mr_dos_free(mr_dos_block *b)
{
    b->seg = 0;
    b->sel = 0;
}

unsigned long mr_dos_linear(const mr_dos_block *b)
{
    return (unsigned long)b->seg << 4;
}

long mr_dos_conventional_free(void)
{
    return -1;
}

int mr_dpmi_lock(void *addr, unsigned long len)
{
    (void)addr; (void)len;
    return 0;
}

void mr_dpmi_unlock(void *addr, unsigned long len)
{
    (void)addr; (void)len;
}

int mr_dpmi_lock_image(void)
{
    return 0;
}

void mr_dpmi_report_memory(void)
{
}

int mr_dpmi_watch_write(const void *addr)
{
    (void)addr;
    return 0;
}

void mr_dpmi_unwatch(void)
{
}

#endif

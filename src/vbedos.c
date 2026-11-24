/* vbedos.c - the int 10h half of vbe.h: enumerate, set, and map.
 *
 * DOS only. Nothing here is reachable from a harness: there is no VESA BIOS
 * on the build host, and a VBE call cannot be faked at this level in any
 * way that would prove anything. That is why mr_vbe_decode and mr_vbe_pick
 * are in the other file -- the judgement is over there, where it can be
 * argued with, and what is left here is plumbing that either works on the
 * machine or does not.
 *
 * ---------------------------------------------------------------------
 * How the BIOS is called, and the mistake that was made first
 *
 * The first version of this file called int 10h with Watcom's int386x and
 * put the real-mode segment of the scratch buffer into sregs.es:
 *
 *     sr.es = (unsigned short)seg;        // WRONG
 *     int386x(0x10, &r, &r, &sr);
 *
 * int386x loads the segment registers with *selectors*. A real-mode
 * segment -- 0x1234, say -- is not one; loading it indexes past the
 * descriptor table and the processor raises a general protection fault on
 * the spot. Under DOSBox-X with an S3 card that is:
 *
 *     DOS/4GW error (2001): exception 0Dh at 160:0019E8ED
 *
 * The right way, and the only way that is correct on every DPMI host, is
 * function 0300h: simulate a real-mode interrupt. The real-mode register
 * values -- including ES -- live in a 50-byte structure, where ES is a
 * plain 16-bit field and so cannot be confused with a selector; the pointer
 * to *that* structure is what goes in a segment register, and it is one of
 * ours, which is a genuine selector. Every int 10h call below goes through
 * it, including the ones with no buffer, because having one path is worth
 * more than saving a few instructions on a call made twice per run.
 *
 * Two other things this file does not do any more:
 *
 *   - It does not read the carry flag. Watcom's int386 and int386x do not
 *     return it, so every `if (r.w.cflag)` in the first version was reading
 *     an uninitialised field and believing it. Results are checked by value
 *     instead, which is weaker but honest.
 *
 *   - It does not allocate DOS memory with _dos_allocmem/_dos_freemem.
 *     dosmem.h calls DPMI 0100h directly and keeps both numbers it
 *     returns; the comment there says why the library pair was tried and
 *     then dropped.
 *
 * Still unverified: everything below. It has been reasoned about, not run.
 */

#include <string.h>

#include "mr.h"
#include "vbe.h"
#include "dosmem.h"

#if defined(__DOS__) || defined(MR_TARGET_DOS)

#include <dos.h>
#include <i86.h>

/* --- the DPMI real-mode call structure ----------------------------------
 *
 * DPMI 0.9 specification, function 0300h. Fifty bytes, and like the
 * ModeInfoBlock it is handled as bytes at named offsets rather than as a
 * struct: the layout is fixed by a document, not by this compiler, and a
 * struct is one padding decision away from being wrong in a way that would
 * show up as the BIOS being passed nonsense.
 */
#define RM_EDI   0x00
#define RM_ESI   0x04
#define RM_EBP   0x08
#define RM_EBX   0x10
#define RM_EDX   0x14
#define RM_ECX   0x18
#define RM_EAX   0x1C
#define RM_FLAGS 0x20
#define RM_ES    0x22
#define RM_DS    0x24
#define RM_FS    0x26
#define RM_GS    0x28
#define RM_IP    0x2A
#define RM_CS    0x2C
#define RM_SP    0x2E
#define RM_SS    0x30
#define RM_SIZE  0x32

static void rm_put16(unsigned char *p, int off, unsigned v)
{
    p[off]     = (unsigned char)(v & 0xFF);
    p[off + 1] = (unsigned char)((v >> 8) & 0xFF);
}

static void rm_put32(unsigned char *p, int off, unsigned long v)
{
    p[off]     = (unsigned char)(v & 0xFF);
    p[off + 1] = (unsigned char)((v >> 8) & 0xFF);
    p[off + 2] = (unsigned char)((v >> 16) & 0xFF);
    p[off + 3] = (unsigned char)((v >> 24) & 0xFF);
}

static unsigned rm_get16(const unsigned char *p, int off)
{
    return (unsigned)p[off] | ((unsigned)p[off + 1] << 8);
}

/* Run a real-mode interrupt with the registers in `rc`, and read them back
 * into it. Returns the real-mode AX.
 *
 * CS:IP and SS:SP are left zero, which asks DPMI to supply its own
 * real-mode stack -- the alternative is finding one below 1MB ourselves,
 * and the host is better placed to do it. */
static unsigned rm_int(int intno, unsigned char *rc)
{
    union REGS   r;
    struct SREGS sr;

    segread(&sr);
    memset(&r, 0, sizeof(r));

    r.w.ax = 0x0300;
    r.w.bx = (unsigned short)(intno & 0xFF);  /* BL = vector, BH = flags 0 */
    r.w.cx = 0;                               /* no words off our stack */
    r.x.edi = (unsigned long)rc;

    /* ES:EDI points at the structure. ES must be a selector that addresses
     * it -- ours does, which is the whole difference from the version that
     * faulted. Under the zero-based flat model DS has base 0, so the flat
     * address of rc is also its offset within the selector. */
    sr.es = sr.ds;

    int386x(0x31, &r, &r, &sr);

    return rm_get16(rc, RM_EAX);
}

/* --- DOS memory ---------------------------------------------------------
 *
 * dosmem.h now, shared with the Sound Blaster sink, which needs the same
 * thing for its DMA buffer. It was here first: DPMI 0100h and 0101h by hand
 * rather than _dos_allocmem/_dos_freemem, because the allocation hands back
 * *two* numbers -- AX a real-mode segment, DX a protected-mode selector --
 * and only one of them can be freed. That distinction, and what it cost to
 * learn, is written up where the code is.
 */

/* --- mapping the framebuffer -------------------------------------------- */

/* DPMI 0800h, physical address mapping. Returns a linear address, or 0.
 *
 * Carry cannot be read, so success is judged by the result being non-zero
 * and the caller falls back to the physical address when it is not. Under
 * DOS/4GW and DOS/32A that fallback is correct anyway: both use zero-based
 * flat selectors covering the whole address space with paging off, so
 * linear equals physical. A host that pages -- CWSDPMI, HDPMI32 -- needs
 * the mapping, which is why it is attempted first rather than skipped.
 *
 * An address below 1MB is already identity-mapped and must not be passed
 * here; the spec says so, and some hosts fail the call rather than
 * returning the identity. */
static unsigned long dpmi_map_physical(unsigned long phys, unsigned long len)
{
    union REGS r;
    unsigned long lin;

    if (phys < 0x100000UL)
        return phys;

    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0800;
    r.w.bx = (unsigned short)(phys >> 16);
    r.w.cx = (unsigned short)(phys & 0xFFFF);
    r.w.si = (unsigned short)(len >> 16);
    r.w.di = (unsigned short)(len & 0xFFFF);
    int386(0x31, &r, &r);

    lin = ((unsigned long)r.w.bx << 16) | r.w.cx;
    return lin;
}

/* --- enumeration --------------------------------------------------------
 *
 * One DOS allocation serves both blocks: 512 bytes for the VbeInfoBlock
 * (256 for VBE 1.2, 512 once 'VBE2' has asked for the extended one) and 256
 * for a ModeInfoBlock.
 */
#define SCRATCH_INFO  0
#define SCRATCH_MODE  512
#define SCRATCH_BYTES 768

int mr_vbe_enumerate(mr_vbe_mode *list, int max, int *version)
{
    unsigned char rc[RM_SIZE];
    unsigned char *info;
    unsigned char *mode;
    unsigned long modelist;
    mr_dos_block blk;
    unsigned seg;
    int vbever, count = 0, seen = 0, ax;

    if (!list || max <= 0)
        return -1;

    /* Conventional memory, which is the only kind the BIOS can reach. */
    if (!mr_dos_alloc(&blk, (SCRATCH_BYTES + 15) / 16)) {
        mr_log("vbe: could not allocate real-mode memory for the BIOS");
        return -1;
    }
    seg = blk.seg;

    /* The low megabyte is mapped 1:1 under DOS/4GW and DOS/32A, so the same
     * bytes the BIOS wrote through a real-mode segment are readable here
     * through a flat pointer. */
    info = (unsigned char *)((unsigned long)seg << 4);
    mode = info + SCRATCH_MODE;

    /* 'VBE2' in the signature asks a 2.0 BIOS for the extended block. A 1.2
     * BIOS ignores it and fills the first 256 bytes only, which is why
     * nothing past the version is read before the version is checked. */
    memset(info, 0, 512);
    memcpy(info, "VBE2", 4);

    memset(rc, 0, sizeof(rc));
    rm_put32(rc, RM_EAX, 0x4F00);
    rm_put16(rc, RM_ES, seg);
    rm_put32(rc, RM_EDI, SCRATCH_INFO);
    ax = (int)rm_int(0x10, rc);

    if (ax != 0x004F || memcmp(info, "VESA", 4) != 0) {
        mr_log("vbe: no VESA BIOS extensions (ax=%04x, sig=%02x%02x%02x%02x)",
               (unsigned)ax, info[0], info[1], info[2], info[3]);
        mr_dos_free(&blk);
        return -1;
    }

    vbever = (int)info[4] | ((int)info[5] << 8);
    if (version)
        *version = vbever;
    mr_log("vbe: VESA BIOS %d.%d", (vbever >> 8) & 0xFF, vbever & 0xFF);

    /* VideoModePtr, offset 0x0E, is a real-mode far pointer: offset first,
     * then segment. It points into the BIOS data area or into ROM, both
     * below 1MB, so the same 1:1 reasoning applies -- but it is checked,
     * because a far pointer read out of firmware is exactly the sort of
     * thing to walk off into nothing if the block was not what we think. */
    modelist = ((unsigned long)rm_get16(info, 0x10) << 4)
             + (unsigned long)rm_get16(info, 0x0E);

    if (modelist < 0x400UL || modelist >= 0x100000UL) {
        mr_log("vbe: mode list pointer %08lx is not in real-mode memory",
               modelist);
        mr_dos_free(&blk);
        return -1;
    }

    for (;;) {
        const unsigned char *mp = (const unsigned char *)modelist;
        unsigned number = rm_get16(mp, 0);
        const char *why = NULL;

        if (number == 0xFFFF)
            break;
        modelist += 2;

        /* A list that is not terminated would otherwise be followed until
         * something faults. Real BIOSes report a few dozen modes. */
        if (++seen > 512) {
            mr_log("vbe: mode list is not terminated, stopping at %d", seen);
            break;
        }
        if (count >= max)
            break;
        if (modelist >= 0x100000UL)
            break;

        memset(mode, 0, MR_VBE_BLOCK);

        memset(rc, 0, sizeof(rc));
        rm_put32(rc, RM_EAX, 0x4F01);
        rm_put32(rc, RM_ECX, number);
        rm_put16(rc, RM_ES, seg);
        rm_put32(rc, RM_EDI, SCRATCH_MODE);
        ax = (int)rm_int(0x10, rc);

        /* A BIOS may refuse any individual mode in its own list. Skip it
         * rather than stopping: the list is not sorted and the useful modes
         * may be past it. */
        if (ax != 0x004F) {
            mr_log("vbe:   0x%03x  refused by the BIOS (ax=%04x)", number,
                   (unsigned)ax);
            continue;
        }

        if (mr_vbe_decode(mode, (int)number, vbever, &list[count], &why)) {
            mr_log("vbe:   0x%03x  %dx%d  %dbpp (%d significant)  pitch %d",
                   list[count].number, list[count].w, list[count].h,
                   list[count].bpp, list[count].sigbpp, list[count].pitch);
            count++;
        } else {
            /* Logged, not silent. A mode count of zero with no reasons is
             * indistinguishable from reading the block wrong, and on the
             * target there is no second chance to ask. */
            mr_log("vbe:   0x%03x  %dx%d  skipped: %s", number,
                   (int)rm_get16(mode, 0x12), (int)rm_get16(mode, 0x14),
                   why ? why : "unknown");
        }
    }

    mr_dos_free(&blk);

    mr_log("vbe: %d of %d mode%s usable", count, seen, seen == 1 ? "" : "s");
    return count;
}

void *mr_vbe_set_mode(const mr_vbe_mode *mode)
{
    unsigned char rc[RM_SIZE];
    unsigned long lin, len;
    int ax;

    if (!mode)
        return NULL;

    /* Map before switching. If the mapping fails there is nothing to draw
     * into, and finding that out while still in text mode means the error
     * is readable. */
    len = (unsigned long)mode->pitch * mode->h;
    lin = dpmi_map_physical(mode->phys, len);
    if (!lin) {
        mr_log("vbe: DPMI would not map %lu bytes at %08lx; using the "
               "physical address directly, which is right under DOS/4GW "
               "and DOS/32A and wrong under a paging host", len, mode->phys);
        lin = mode->phys;
    }

    memset(rc, 0, sizeof(rc));
    rm_put32(rc, RM_EAX, 0x4F02);
    /* 0x4000 asks for the linear framebuffer. Carried here rather than in
     * the mode number so that a number in the log or in mrpy.ini is the one
     * the VBE documentation and every mode-list utility would show. */
    rm_put32(rc, RM_EBX, (unsigned long)mode->number | 0x4000UL);
    ax = (int)rm_int(0x10, rc);

    if (ax != 0x004F) {
        mr_log("vbe: mode 0x%03x refused (ax=%04x)", mode->number,
               (unsigned)ax);
        return NULL;
    }

    mr_log("vbe: mode 0x%03x set, %dx%d %dbpp, framebuffer %08lx -> %08lx",
           mode->number, mode->w, mode->h, mode->bpp, mode->phys, lin);

    return (void *)lin;
}

void mr_vbe_text_mode(void)
{
    unsigned char rc[RM_SIZE];

    memset(rc, 0, sizeof(rc));
    rm_put32(rc, RM_EAX, 0x0003);
    rm_int(0x10, rc);
}

#else  /* not DOS */

/* The host build compiles this file to three do-nothing functions.
 *
 * vbetest links it even though it drives none of it, which is the point:
 * the DOS half is built by no makefile on this machine, so without that
 * the first thing to notice a prototype drifting out of step with vbe.h
 * would be `wmake -f Makefile.dos` on a machine that may not get run for
 * weeks. The shipped Win32 binary does not link this file and carries none
 * of it. */

int mr_vbe_enumerate(mr_vbe_mode *list, int max, int *version)
{
    (void)list; (void)max;
    if (version)
        *version = 0;
    return -1;
}

void *mr_vbe_set_mode(const mr_vbe_mode *mode)
{
    (void)mode;
    return NULL;
}

void mr_vbe_text_mode(void)
{
}

#endif

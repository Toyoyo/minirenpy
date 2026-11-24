/* memwalk.c - does this DPMI host give back the memory it took, and does
 * mapping the framebuffer change its mind?
 *
 * Built for the FreeDOS 1.4 fault, where the engine's statement array took
 * 6466 writes during parsing and read back as noise a moment later. That is
 * not something a C program can do to itself reproducibly, and the machine
 * it happens on differs from two working ones in one way: the memory
 * manager is from the Jemm family (JEMMEX or HIMEMX+JEMM386) rather than
 * MS-DOS EMM386, and Jemm386 reports a VCPI pool capped at 16200 pages --
 * 63.3MB -- whatever the machine's RAM.
 *
 * So: take memory in 1MB chunks, stamp every page with its own linear
 * address, then read all of it back and report the first page that
 * disagrees. The stamp is the address itself, which is what makes this
 * worth running rather than a plain memory test: if a page comes back
 * holding *another page's* address, the mapping aliases, and that is a
 * different fault from a page that is merely corrupt or absent.
 *
 * Three outcomes, and they mean different things:
 *
 *   - all clean                          the host is honest; look elsewhere
 *   - bad at a round boundary            the pool or the page tables ran out
 *   - bad page holds another's stamp     the mapping aliases: two linear
 *                                        addresses, one physical page
 *
 * The first pass came back clean, so there is a second one. The engine does
 * something this never did: it asks DPMI 0800h to map the card's linear
 * framebuffer, and on the failing machine that mapping is *relocated* --
 * physical fd000000 appears at linear 80000000 -- where the machines that
 * work map it identity. Building page tables at 2GB while a heap is live
 * below is the one host operation left that could plausibly damage it. So
 * the memory is stamped, verified, the framebuffer is mapped through the
 * engine's own vbe.c/vbedos.c, and then the same memory is verified again.
 * Anything that changes across that is the host doing it, with no engine
 * in the picture.
 *
 * Build it beside modes.exe -- it links vbe.c, vbedos.c and dosmem.c:
 *
 *   wmake -f Makefile.dos memwalk     memwalk.exe,   DOS/32A
 *   wmake -f Makefile.cw  memwalk     memwalkcw.exe, CauseWay
 *
 * This used to be a wcc386 and a wlink line written out here, and they named
 * `system dos32a` and a literal stub path. That is a trap for this probe in
 * particular: the question it answers is whose memory manager is misbehaving,
 * so building it against one extender while believing it was the other
 * answers the wrong question and looks like a result. The makefile takes the
 * extender from whichever file you invoke.
 *
 * Note it is compiled unoptimised, on purpose, and the makefile says why --
 * nothing here is declared volatile, and a probe you have to reason about
 * before trusting is worth less than one built the dull way. The hand-written
 * line had no -otexan either, so the numbers stay comparable.
 *
 * Usage: MEMWALK [megabytes] [file-to-read]   (default 96, the pak)
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "mr.h"
#include "vbe.h"

#define PAGE   4096
#define CHUNK  (1024L * 1024L)      /* one megabyte per allocation */
#define MAXCH  512

/* The value a page at address `a` should hold. Not `a` itself: a stamp that
 * differs from the address in every bit position makes a half-written or
 * shifted value as visible as a wholly wrong one, and it still names its
 * page uniquely, which is what the aliasing check needs. */
static unsigned long stamp_for(unsigned long a)
{
    return a ^ 0xA5A5A5A5UL;
}

/* Reverse it, so a wrong value can be reported as "this is page X's". */
static unsigned long page_of(unsigned long v)
{
    return v ^ 0xA5A5A5A5UL;
}

/* vbedos.c logs every mode it looks at, which is 90 lines here and none of
 * them the point. Swallowed. */
void mr_log(const char *fmt, ...)
{
    (void)fmt;
}

void mr_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("FATAL: ");
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    exit(1);
}

/* Check every page of every chunk against its stamp. Returns the number of
 * bad pages and prints the first few, naming the owner of a wrong stamp
 * when the value turns out to be some other page's. */
static int verify(char **chunk, int n, const char *when)
{
    int i, bad = 0;
    long off;

    for (i = 0; i < n; i++) {
        for (off = 0; off < CHUNK; off += PAGE) {
            unsigned long a = (unsigned long)(chunk[i] + off);
            unsigned long got = *(unsigned long *)(chunk[i] + off);

            if (got == stamp_for(a))
                continue;

            bad++;
            if (bad <= 8) {
                unsigned long who = page_of(got);
                printf("  %s %08lx: holds %08lx", when, a, got);
                if ((who & (PAGE - 1)) == 0 && who != a)
                    printf("  <- page %08lx's stamp, %ldKB away (ALIASED)",
                           who, ((long)who - (long)a) / 1024);
                else
                    printf("  <- no page's stamp (CORRUPT)");
                printf("\n");
            }
        }
    }
    return bad;
}

/* Grow a block the way the parser grows its statement array, and check
 * that realloc brings the contents with it.
 *
 * This is the one the evidence points at. The engine's array is realloc'd
 * 1024 -> 2048 -> 4096 -> 8192 entries of 272 bytes; on the failing
 * machine the contents after one of those moves are not the old contents
 * but a copy of the program's own code, and every byte of the old data is
 * gone. Nothing in the engine copies code, and the address embedded in
 * those bytes is the *relocated* one, so it came from live memory rather
 * than from the exe on disk.
 *
 * Same sizes, same doubling, filled with a pattern derived from the index
 * so a wrong byte names its own position. Returns the number of bad
 * bytes found.
 */
static int realloc_ladder(void)
{
    unsigned char *p = NULL;
    long cap = 1024, used, i;
    int round, bad = 0;

    for (round = 0; round < 6; round++) {
        long bytes = cap * 272;
        unsigned char *np = (unsigned char *)realloc(p, (size_t)bytes);

        if (!np) {
            printf("memwalk: realloc to %ldKB refused; stopping the "
                   "ladder\n", bytes / 1024);
            break;
        }

        /* Check what came across before writing anything new. */
        used = round ? (cap / 2) * 272 : 0;
        for (i = 0; i < used; i++) {
            if (np[i] == (unsigned char)(i * 7 + 13))
                continue;
            if (bad < 4)
                printf("  realloc to %ldKB: byte %ld is %02x, should be "
                       "%02x%s\n", bytes / 1024, i, np[i],
                       (unsigned char)(i * 7 + 13),
                       i == 0 ? "  <- the whole block, from byte zero" : "");
            bad++;
        }
        if (bad)
            printf("memwalk: realloc to %ldKB lost %d of %ld bytes\n",
                   bytes / 1024, bad, used);

        /* Fill the whole thing and double. */
        for (i = 0; i < bytes; i++)
            np[i] = (unsigned char)(i * 7 + 13);
        p = np;
        cap *= 2;
    }

    free(p);
    if (!bad)
        printf("memwalk: realloc kept every byte through %ld doublings\n",
               (long)round);
    return bad;
}

/* Read a file, hard, into heap buffers of awkward sizes.
 *
 * The third thing the engine does that this did not. Under DOS/32A every
 * DOS read lands first in a transfer buffer in conventional memory and is
 * copied up into the protected-mode buffer by the extender; a host that
 * gets that wrong writes somewhere other than where it was asked to. The
 * engine's parse is the first phase to read megabytes, and the evidence
 * says the statement array was already damaged when parsing finished.
 *
 * Buffer sizes are deliberately not round: a bug that only shows when the
 * copy crosses a page or a 64KB boundary needs reads that straddle them.
 */
static void hammer_file(const char *path, int n)
{
    static const int sizes[6] = { 4095, 4096, 4097, 17000, 65535, 65537 };
    FILE *f;
    char *buf;
    long total = 0, total_pass = 0;
    int pass;

    if (!n)
        return;

    for (pass = 0; pass < 6; pass++) {
        f = fopen(path, "rb");
        if (!f) {
            printf("memwalk: cannot open %s; skipping the read test\n", path);
            return;
        }
        buf = (char *)malloc((size_t)sizes[pass]);
        if (!buf) {
            fclose(f);
            continue;
        }
        /* 8MB a pass, not the whole file. The point is to make the
         * extender copy through its transfer buffer a few thousand times
         * at each awkward size, and the pak is 93MB -- reading all of it
         * six times is half a gigabyte of VM disk I/O to learn the same
         * thing. */
        while (total_pass < 8L * 1024 * 1024 &&
               fread(buf, 1, (size_t)sizes[pass], f) > 0) {
            total += sizes[pass];
            total_pass += sizes[pass];
        }
        total_pass = 0;
        free(buf);
        fclose(f);
    }

    printf("memwalk: read %ldKB from %s in six buffer sizes\n",
           total / 1024, path);
}

/* Map the card's framebuffer exactly as the engine does: enumerate with
 * mr_vbe_enumerate, choose with mr_vbe_pick, map and set with
 * mr_vbe_set_mode, then straight back to text. The mode is set for as
 * little time as possible -- the point is the mapping, not the picture --
 * and the screen flickering once is the whole visible effect. */
static int map_framebuffer(void)
{
    static mr_vbe_mode list[128];
    mr_vbe_want want;
    int n, k, version = 0;
    void *fb;

    n = mr_vbe_enumerate(list, 128, &version);
    if (n <= 0) {
        printf("memwalk: no usable VBE mode; cannot test the mapping\n");
        return 0;
    }

    want.want_bpp = 0;
    want.stage_w = 1280;
    want.stage_h = 720;
    want.allow_8bpp = 1;

    k = mr_vbe_pick(list, n, &want);
    if (k < 0) {
        printf("memwalk: the picker refused all %d modes\n", n);
        return 0;
    }

    fb = mr_vbe_set_mode(&list[k]);

    /* Write through it, which the first version of this did not and the
     * engine does within milliseconds of setting the mode -- clear_screen
     * puts a whole frame of black through the mapping. A mapping that is
     * accepted and then lands somewhere other than the card is a thing
     * only a write can discover. A frame of 0xDB is used rather than
     * black so that the screen says plainly that this happened. */
    if (fb) {
        long y;
        for (y = 0; y < list[k].h; y++)
            memset((char *)fb + y * (long)list[k].pitch, 0xDB,
                   (size_t)list[k].pitch);
    }

    mr_vbe_text_mode();

    if (!fb) {
        printf("memwalk: mode %03x would not set\n", list[k].number);
        return 0;
    }

    printf("memwalk: mapped mode %03x, %dx%d %dbpp: physical %08lx -> "
           "linear %08lx%s\n",
           list[k].number, list[k].w, list[k].h, list[k].bpp,
           list[k].phys, (unsigned long)fb,
           (unsigned long)fb == list[k].phys ? " (identity)" : " (RELOCATED)");
    return 1;
}

int main(int argc, char **argv)
{
    static char *chunk[MAXCH];
    int want = argc > 1 ? atoi(argv[1]) : 96;
    int n = 0, i, before, after;
    long off;

    if (want < 1)     want = 1;
    if (want > MAXCH) want = MAXCH;

    printf("memwalk: taking %dMB in 1MB chunks, stamping every page\n", want);

    /* Take it all first, then verify. Writing and reading each chunk in turn
     * would let a host hand back the same physical page over and over and
     * still pass. */
    for (i = 0; i < want; i++) {
        chunk[i] = (char *)malloc(CHUNK);
        if (!chunk[i])
            break;
        for (off = 0; off < CHUNK; off += PAGE) {
            unsigned long a = (unsigned long)(chunk[i] + off);
            *(unsigned long *)(chunk[i] + off) = stamp_for(a);
        }
        n++;
    }

    printf("memwalk: got %dMB, %08lx to %08lx\n", n,
           n ? (unsigned long)chunk[0] : 0UL,
           n ? (unsigned long)(chunk[n - 1] + CHUNK) : 0UL);
    if (n < want)
        printf("memwalk: malloc refused chunk %d -- that part is honest, a "
               "host saying no is not the bug being looked for\n", n);

    before = verify(chunk, n, "before:");
    printf("memwalk: before mapping, %d bad page%s\n",
           before, before == 1 ? "" : "s");

    /* The realloc ladder, which is where the engine's array was lost. */
    printf("memwalk: growing a block 1024->32768 x 272 bytes by doubling\n");
    realloc_ladder();

    /* Read a lot of file through the extender, then look again. */
    hammer_file(argc > 2 ? argv[2] : "MIWA.PAK", n);
    {
        int afterio = verify(chunk, n, "after I/O:");
        printf("memwalk: after file reads, %d bad page%s\n",
               afterio, afterio == 1 ? "" : "s");
        if (!before && afterio)
            printf("memwalk: the file reads did this. The extender or the "
                   "host wrote outside the buffer it was given.\n");
        before = afterio;
    }

    /* The second pass is the point of this version. */
    if (map_framebuffer()) {
        after = verify(chunk, n, "after: ");
        printf("memwalk: after mapping, %d bad page%s\n",
               after, after == 1 ? "" : "s");

        if (!before && after)
            printf("memwalk: the framebuffer mapping did this. The host "
                   "damaged memory it had already given away.\n");
        else if (!before && !after)
            printf("memwalk: %dMB survived the mapping unchanged. This host "
                   "is not the problem.\n", n);
    } else {
        after = before;
    }

    for (i = 0; i < n; i++)
        free(chunk[i]);

    return after ? 1 : 0;
}

/* The CPUID feature decode, and what the host can honestly say about it.
 *
 * src/cpu.c gates both MMX kernels. Before it existed they were enabled by a
 * compile-time #define, so a Watcom build ran them on whatever it was started
 * on and a 486 faulted at the first `punpcklbw` with nothing in the log. The
 * cost of fixing that is a new way to be wrong: if the detector says "no MMX"
 * on a machine that has it, the engine silently runs 7ms slower a frame.
 *
 * So the detector is split the way yuvmmx.h and blendmmx.h are, and this is
 * the half that can be checked here:
 *
 *   checked here   MR_CPU_DECODE -- which bit, and in which order relative to
 *                  the leaf-count guard. Driven with synthesised CPUID
 *                  results, including ones no real processor returns, because
 *                  the interesting cases (leaf 1 unsupported; every feature
 *                  bit but MMX) are exactly the ones no machine in this
 *                  tree's reach would hand us.
 *
 *   not checked    the assembly in src/cpu.c's Watcom path: the EFLAGS ID
 *                  toggle, and the two CPUID executions that produce the
 *                  values decoded above. Nothing on this host can run it.
 *                  probe/cpuchk.c and the engine's own `cpu:` log line are
 *                  that half.
 *
 * **And one thing this harness deliberately does not assert**: that
 * mr_cpu_has_mmx() returns 1. It does, on every machine that can build this,
 * and it would go on returning 1 if the decode were replaced by `return
 * MR_CPU_MMX;`. An assertion that cannot fail is worse than none, so what is
 * asserted about the live detector is only the invariants that *could* break
 * -- agreement between the two public queries, idempotence, and a description
 * that is actually a description. The value itself is printed as information.
 */

#include <stdio.h>
#include <string.h>

#include "../../src/cpu.h"

static int g_fail;

static void check(const char *what, int ok)
{
    printf("  %s %s\n", ok ? "pass" : "FAIL", what);
    if (!ok)
        g_fail = 1;
}

int main(void)
{
    unsigned b;
    unsigned wrong_bit = 0;
    unsigned leaked = 0;
    const char *d1;
    const char *d2;
    unsigned f1, f2;

    printf("cputest: CPUID feature decode\n");

    /* --- the bit ---------------------------------------------------------
     *
     * Bit 23 of leaf 1's EDX and no other. Every single-bit EDX is tried, so
     * an off-by-one -- bit 22 is "ACPI", bit 24 is "FXSR" -- fails here
     * rather than on a machine that has one and not the other. There are
     * real processors with FXSR, and the Pentium III is one of them. */
    for (b = 0; b < 32; b++) {
        unsigned edx = 1u << b;
        unsigned got = MR_CPU_DECODE(1u, edx);

        if (b == 23) {
            if (got != MR_CPU_MMX)
                wrong_bit = 1;
        } else if (got != 0u) {
            wrong_bit = 1;
        }
    }
    check("leaf 1 EDX bit 23 means MMX, and no other single bit does",
          !wrong_bit);

    check("MMX is seen in a full EDX", MR_CPU_DECODE(1u, 0xFFFFFFFFu)
                                      == MR_CPU_MMX);
    check("and not in an EDX with only bit 23 clear",
          MR_CPU_DECODE(1u, ~MR_CPUID_EDX_MMX) == 0u);

    /* --- the order -------------------------------------------------------
     *
     * This is the claim worth having. src/cpu.c does not execute leaf 1 when
     * leaf 0 reports a maximum of 0, so the EDX it decodes in that case is
     * whatever was left in the register -- the initialiser, as it happens,
     * but the decode must not depend on knowing that. A decoder that read the
     * bit before checking the leaf count would report MMX from uninitialised
     * data, which is the kind of answer that is right on every machine anyone
     * tests and wrong on the one that matters.
     *
     * xvidcore checks the same thing at cpuid.asm:122. */
    for (b = 0; b < 32; b++) {
        if (MR_CPU_DECODE(0u, 1u << b) != 0u)
            leaked = 1;
    }
    if (MR_CPU_DECODE(0u, 0xFFFFFFFFu) != 0u)
        leaked = 1;
    check("no maxleaf means no features, whatever EDX says", !leaked);

    check("maxleaf 1 is enough", MR_CPU_DECODE(1u, MR_CPUID_EDX_MMX)
                                 == MR_CPU_MMX);
    check("and so is a higher one", MR_CPU_DECODE(11u, MR_CPUID_EDX_MMX)
                                    == MR_CPU_MMX);

    /* The two constants, pinned against xvid's, since they were copied from
     * it and a transcription error in either is invisible everywhere else. */
    check("CPUID_MMX is 0x00800000 as in xvid cpuid.asm:34",
          MR_CPUID_EDX_MMX == 0x00800000u);
    check("the EFLAGS ID flag is 0x00200000 as in cpuid.asm:107",
          MR_EFLAGS_ID == 0x00200000u);

    /* --- the live detector, for the little that can be said --------------*/

    f1 = mr_cpu_flags();
    f2 = mr_cpu_flags();
    check("mr_cpu_flags() is stable across calls", f1 == f2);
    check("mr_cpu_has_mmx() agrees with the flag word",
          mr_cpu_has_mmx() == ((f1 & MR_CPU_MMX) != 0));

    d1 = mr_cpu_desc();
    d2 = mr_cpu_desc();
    check("mr_cpu_desc() returns something", d1 != NULL && d1[0] != '\0');
    check("and the same thing twice", d1 != NULL && d2 != NULL &&
                                      strcmp(d1, d2) == 0);
    /* It goes through mr_log as one line. A newline in it would split the log
     * record in two, which is the sort of thing noticed only while reading a
     * log from a machine one cannot reach. */
    check("with no embedded newline",
          d1 != NULL && strchr(d1, '\n') == NULL);

    printf("  (this host: %s)\n", d1 ? d1 : "?");

    /* --- force, last, because it cannot be undone ------------------------
     *
     * mrpy.ini's `simd=2`. The point is that it holds even when detection
     * said no, so what is asserted is the flag afterwards, not the flag
     * before -- and that the description admits it, because a forced run that
     * logged itself as a detected one would make the next log unreadable. */
    mr_cpu_force_mmx();
    check("mr_cpu_force_mmx() makes MMX claimed", mr_cpu_has_mmx());
    check("and the description says so",
          strstr(mr_cpu_desc(), "forced") != NULL);

    printf("%s\n", g_fail ? "** FAIL **" : "ok");

    printf("note: src/cpu.c's Watcom path -- the EFLAGS ID toggle and the two\n"
           "      CPUID executions -- is NOT built or run here. Only the\n"
           "      decode above is. probe/cpuchk.c is the target half, and the\n"
           "      engine's `cpu:` log line reports it from every real run.\n");

    return g_fail ? 1 : 0;
}

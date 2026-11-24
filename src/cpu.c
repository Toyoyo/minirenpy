/* cpu.c - CPUID feature detection, following xvidcore's cpuid.asm.
 *
 * See cpu.h for why this exists and for the three conditions that have to be
 * checked in order. This file is only the three implementations of "ask the
 * processor", plus the caching and the log line around them.
 *
 * Unlike yuvmmx.c and blendmmx.c, **this file does compile on the build
 * host**, because the question it answers is not Watcom-specific: gcc has
 * <cpuid.h> and the harness build uses it. That is deliberate. A detector
 * that could only be reached from the target would mean the thing now
 * standing between the engine and its two fast paths was the one module no
 * test could link, and the failure mode of a wrong answer here is not a wrong
 * pixel but a silently slow machine -- or, in the other direction, exactly
 * the invalid-opcode fault the detection was added to prevent.
 *
 * What the host can and cannot say about it is worth being exact about:
 *
 *   it can    MR_CPU_DECODE, over synthesised leaf values, including the
 *             ones no processor this engine runs on would return. That is
 *             cputest, and it is where a mistake in the bit or in the
 *             maxleaf ordering would show.
 *
 *   it cannot say anything about the Watcom path below, which is the one
 *             that ships, and nothing useful about its own: every x86 the
 *             harness could possibly run on has had MMX for thirty years,
 *             so `mr_cpu_has_mmx() == 1` on the host is not a passing test,
 *             it is an unavoidable constant. cputest says so rather than
 *             asserting it.
 *
 * The target half is probe/cpuchk.c, and the engine's own startup line --
 * which is the better of the two, because it comes back from every run on
 * every machine without anyone being asked to run a probe.
 */

#include <string.h>
#include <stdio.h>

#include "cpu.h"

/* Detection runs once. CPUID is a serialising instruction -- it drains the
 * pipeline, which on a P6 is the whole point of it and costs a few hundred
 * cycles -- and mr_blend_have_simd() is asked once per blit row. */
static int      g_done;
static int      g_forced;
static unsigned g_flags;

/* Kept raw rather than only formatted, so that mr_cpu_desc() can be built
 * after a later mr_cpu_force_mmx() and still describe both what the processor
 * said and what we overrode it with. */
static int      g_have_cpuid;
static unsigned g_maxleaf;
static unsigned g_sig;
static unsigned g_edx;
static char     g_vendor[13];

/* ------------------------------------------------------------------------- */
/* The one platform-dependent step: execute CPUID, or report that it cannot   */
/* be executed. Returns 1 if the values were read.                            */
/* ------------------------------------------------------------------------- */

#if defined(__WATCOMC__) && defined(__386__)

/* The shipped path, and the only one that runs on the target.
 *
 * Three things about the assembly are not style:
 *
 * `cpuid` clobbers EAX, EBX, ECX and EDX. Watcom's inline assembler preserves
 * registers it can see being written, and it sees the `mov`s -- but EBX is
 * written by the instruction and not by a mov, so it is pushed and popped by
 * hand. It is popped **before any C variable is referenced**, because Watcom
 * may address a local off ESP rather than EBP and a live push would shift it.
 * (It emits EBP-relative offsets here, so an unbalanced push would happen to
 * work -- which is why the rule is followed rather than checked. The first
 * version of this also pushed and popped ESI around the stores, and was
 * correct only by that accident.) ESI carries EBX's value across the pop and
 * needs no saving of its own: naming it is enough for Watcom to push it in
 * the prologue, which the disassembly confirms it does.
 *
 * The EFLAGS dance is balanced at every point where a local is touched, for
 * the same reason.
 *
 * And it is written as three separate blocks rather than one, so that the C
 * between them can decide whether the next CPUID is legal to execute at all.
 * That ordering is the entire safety property; see cpu.h. */
static int cpu_ask(void)
{
    unsigned id = 0;
    /* Not one of these is named `ax`, `dx` or anything else that is also a
     * register: Watcom resolves a name in an _asm block as a register first,
     * so a local called `dx` silently assembles into DX. */
    unsigned ml = 0, v0 = 0, v1 = 0, v2 = 0, sg = 0, feat = 0;

    /* Is CPUID there? EFLAGS bit 21 is writable only where it is.
     * xvidcore cpuid.asm:103-111. */
    _asm {
        pushfd
        pop  eax
        mov  ecx, eax
        xor  eax, 0200000H
        push eax
        popfd
        pushfd
        pop  eax
        push ecx                /* the caller's EFLAGS back, either way */
        popfd
        xor  eax, ecx
        and  eax, 0200000H
        mov  id, eax
    }

    if (!id)
        return 0;

    /* Leaf 0: the highest leaf, and the vendor string in EBX:EDX:ECX. */
    _asm {
        push ebx
        xor  eax, eax
        cpuid
        mov  esi, ebx
        pop  ebx
        mov  ml, eax
        mov  v0, esi
        mov  v1, edx
        mov  v2, ecx
    }

    /* xvidcore cpuid.asm:122 does this too: leaf 1 is not guaranteed to
     * exist merely because leaf 0 does. */
    if (ml >= 1) {
        _asm {
            push ebx
            mov  eax, 1
            cpuid
            mov  esi, edx
            pop  ebx
            mov  sg, eax
            mov  feat, esi
        }
    }

    g_maxleaf = ml;
    g_sig = sg;
    g_edx = feat;
    memcpy(g_vendor + 0, &v0, 4);
    memcpy(g_vendor + 4, &v1, 4);
    memcpy(g_vendor + 8, &v2, 4);
    g_vendor[12] = '\0';
    return 1;
}

#elif defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))

#include <cpuid.h>

/* The host path. <cpuid.h> rather than hand-written assembly on purpose: it
 * gets the i386 PIC EBX juggling right, and the point of this path is to make
 * the module linkable and the decode testable, not to run on a 386. There is
 * no EFLAGS check here because __get_cpuid_max already returns 0 when CPUID
 * is absent, and because no machine that can build this engine lacks it. */
static int cpu_ask(void)
{
    unsigned a = 0, b = 0, c = 0, d = 0;

    g_maxleaf = __get_cpuid_max(0, 0);
    if (!g_maxleaf)
        return 0;

    if (__get_cpuid(0, &a, &b, &c, &d)) {
        memcpy(g_vendor + 0, &b, 4);
        memcpy(g_vendor + 4, &d, 4);
        memcpy(g_vendor + 8, &c, 4);
    }
    g_vendor[12] = '\0';

    if (g_maxleaf >= 1 && __get_cpuid(1, &a, &b, &c, &d)) {
        g_sig = a;
        g_edx = d;
    }
    return 1;
}

#else

/* Neither compiler, or not x86 at all. Reporting "no features" is the
 * fail-safe answer and costs nothing real: the only builds with an MMX kernel
 * compiled in are the two Watcom ones, which take the first path. If this one
 * is ever reached by a build that *does* have a kernel, it turns it off --
 * which is the right way round. */
static int cpu_ask(void)
{
    return 0;
}

#endif

/* ------------------------------------------------------------------------- */

static void detect(void)
{
    if (g_done)
        return;

    g_done = 1;
    g_have_cpuid = cpu_ask();
    if (g_have_cpuid)
        g_flags = MR_CPU_DECODE(g_maxleaf, g_edx);
    if (g_forced)
        g_flags |= MR_CPU_MMX;
}

unsigned mr_cpu_flags(void)
{
    detect();
    return g_flags;
}

int mr_cpu_has_mmx(void)
{
    return (mr_cpu_flags() & MR_CPU_MMX) != 0;
}

void mr_cpu_force_mmx(void)
{
    g_forced = 1;
    /* If something already asked, the cached answer has to change with it --
     * otherwise `simd=2` would depend on whether the first blit happened
     * before or after the ini was read, which is not a thing anyone should
     * have to know. */
    if (g_done)
        g_flags |= MR_CPU_MMX;
}

const char *mr_cpu_desc(void)
{
    static char desc[96];

    detect();

    if (!g_have_cpuid) {
        /* A 386, an early 486, or something pretending. Either way both
         * kernels are off unless forced, and this is the line that explains a
         * machine running at half speed with nothing else wrong. */
        sprintf(desc, "no CPUID%s",
                (g_flags & MR_CPU_MMX) ? ", mmx forced on anyway" : "");
        return desc;
    }

    /* Base family/model/stepping only. The extended fields matter from family
     * 15 upward and nothing here branches on either, so adding them would be
     * two more shifts in service of a log line. Family 6 is the target. */
    sprintf(desc, "%s family %u model %u stepping %u, %s",
            g_vendor[0] ? g_vendor : "?",
            (g_sig >> 8) & 0xFu,
            (g_sig >> 4) & 0xFu,
            g_sig & 0xFu,
            (g_flags & MR_CPU_MMX)
                ? (g_forced ? "mmx (forced)" : "mmx")
                : "no mmx");
    return desc;
}

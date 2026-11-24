/* cpuchk.c - does CPUID detection agree with what this machine can do?
 *
 *     wmake -f Makefile.dos cpucheck   -> cpuchk.exe
 *     wmake -f Makefile.cw  cpucheck   -> cpuchkcw.exe
 *
 * Run it on the target. The exit code is the test: 0 if MMX was detected and
 * then demonstrably works, 1 if it was detected and does not, 77 if it was
 * not detected at all (a skip, with the reason printed).
 *
 * --- what this is for -----------------------------------------------------
 *
 * src/cpu.c now stands between the engine and both of its MMX kernels. Its
 * Watcom path is inline assembly -- an EFLAGS ID toggle and two CPUID
 * executions -- and nothing on the build host can run a line of it.
 * tools/harness/cputest.c proves the *decode*, over synthesised leaf values,
 * and says at the end of its run that it proves nothing else.
 *
 * This is the other half, and it asks the one question the host cannot: do
 * the values that reach the decode come from the processor correctly?
 *
 * --- how the answer is checked, and the one case it cannot ----------------
 *
 * A detector has two ways to be wrong and they are not symmetrical.
 *
 * **Claiming MMX that is not there** is the fault the whole file exists to
 * prevent, and it is checkable: execute MMX and look at the result. Not
 * merely "it did not fault" -- a sequence whose answer is known, so that a
 * processor which executes the opcodes and gets them wrong (an emulator, a
 * V86 monitor, a Cyrix with MMX half-implemented) is caught too. `paddusb`
 * with a saturating lane is used because the saturation is the part an
 * incomplete implementation gets wrong.
 *
 * **Denying MMX that is there** cannot be checked here, and that is not an
 * oversight: the only way to find out would be to execute the instruction the
 * detector just said would fault. So the probe exits 77 and prints what to do
 * instead -- `simd=2` in mrpy.ini, which is mrpy's XVID_CPU_FORCE and exists
 * for exactly this. On the Pentium III this engine targets, a 77 from this
 * probe is a bug in src/cpu.c and not a feature of the machine.
 *
 * That asymmetry is why the escape hatch is in the ini and not only here.
 */

#include <stdio.h>

#include "../src/cpu.h"

/* No mr_fatal / mr_log stubs here, unlike the other probes: src/cpu.c calls
 * neither, and this links against that file alone. The whole point of the
 * detector being its own module with no engine dependencies is that this
 * probe can be two object files.
 */

/* Does MMX actually work, and give the right answer?
 *
 * Returns 1 if the sequence produced what it must. Only called when detection
 * has already said MMX is present -- calling it otherwise is the invalid
 * opcode this is all about.
 *
 * `emms` at the end because MMX registers alias the x87 stack and this binary
 * is built -fp5 -fpi87 like the engine. Leaving the tag word as MMX would
 * break the next float, which here is only the printf but in the engine is
 * everything.
 */
static int mmx_works(void)
{
#if defined(__WATCOMC__) && defined(__386__)
    /* Two chosen so that one lane saturates and one does not:
     *
     *   0x10 + 0x20 = 0x30   plain
     *   0xF0 + 0x20 = 0x110  saturates to 0xFF, not wraps to 0x10
     *
     * A wrap instead of a saturation is the specific thing a partial MMX
     * implementation gets wrong, and it is also what a C-level "surely it
     * works" assumption would never notice. */
    static const unsigned char a[8] =
        { 0x10, 0xF0, 0x01, 0x80, 0x00, 0xFF, 0x7F, 0x40 };
    static const unsigned char b[8] =
        { 0x20, 0x20, 0x02, 0x80, 0x00, 0x01, 0x01, 0x40 };
    static const unsigned char want[8] =
        { 0x30, 0xFF, 0x03, 0xFF, 0x00, 0xFF, 0x80, 0x80 };
    unsigned char got[8];
    int i;

    {
        const unsigned char *pa = a;
        const unsigned char *pb = b;
        unsigned char *pg = got;

        _asm {
            mov    esi, pa
            mov    edi, pb
            mov    eax, pg
            movq   mm0, [esi]
            movq   mm1, [edi]
            paddusb mm0, mm1
            movq   [eax], mm0
            emms
        }
    }

    for (i = 0; i < 8; i++) {
        if (got[i] != want[i]) {
            printf("FAIL lane %d: %02x + %02x = %02x, want %02x\n",
                   i, a[i], b[i], got[i], want[i]);
            return 0;
        }
    }
    printf("ok   paddusb over eight lanes, two of them saturating\n");
    return 1;
#else
    /* No MMX can be executed from this compiler, so there is nothing to
     * compare and saying "pass" would be a lie. The probe is built by the two
     * Watcom makefiles and by nothing else; this branch exists so that a
     * stray host build is loud rather than green. */
    printf("FAIL this build cannot execute MMX, so it cannot check it\n");
    return 0;
#endif
}

int main(void)
{
    printf("cpuchk: CPUID detection against this machine\n");
    printf("cpu: %s\n", mr_cpu_desc());

    if (!mr_cpu_has_mmx()) {
        /* A skip and not a pass: "could not reach the thing I test". On the
         * target this is itself the finding. */
        printf("SKIP no MMX detected, so there is nothing to compare it\n"
               "     against -- the one error this probe cannot catch.\n"
               "     If this machine does have MMX, src/cpu.c is wrong here\n"
               "     and `simd=2` in mrpy.ini is the way back in the\n"
               "     meantime. See cpu.h.\n");
        return 77;
    }

    if (!mmx_works()) {
        printf("** FAIL ** MMX was detected and does not work correctly.\n"
               "           Set simd=0 in mrpy.ini on this machine.\n");
        return 1;
    }

    printf("ok\n");
    return 0;
}

/* i86.h - enough of Open Watcom's to syntax-check the DOS branch.
 *
 * READ THIS BEFORE TRUSTING IT.
 *
 * This shim proves one thing and one thing only: that the DOS half of
 * vbedos.c is syntactically valid C89 with no typos, no undeclared
 * variables and no declarations after statements. It proves *nothing*
 * about whether the calls are used correctly, because the declarations
 * below were written from the same understanding of Watcom's API that the
 * code was -- if that understanding is wrong, the shim is wrong in the same
 * direction and agrees with the bug.
 *
 * It would not have caught the fault that made it necessary. The first
 * version of vbedos.c put a real-mode segment into sregs.es, where a
 * selector belongs; both are `unsigned short` and a compiler has nothing to
 * say about it. That was found by running the thing on DOSBox-X and reading
 * a general protection fault.
 *
 * So: a cheap guard against the class of mistake that costs a round trip to
 * the DOS machine for no reason, and not a substitute for the trip.
 *
 * Not on the include path of any harness. run.sh points one syntax-only
 * compile at it.
 */

#ifndef MR_HOSTSHIM_I86_H
#define MR_HOSTSHIM_I86_H

struct DWORDREGS {
    unsigned long eax, ebx, ecx, edx, esi, edi, cflag;
};

struct WORDREGS {
    unsigned short ax, ax_hi, bx, bx_hi, cx, cx_hi, dx, dx_hi;
    unsigned short si, si_hi, di, di_hi, cflag, cflag_hi;
};

struct BYTEREGS {
    unsigned char al, ah; unsigned short _1;
    unsigned char bl, bh; unsigned short _2;
    unsigned char cl, ch; unsigned short _3;
    unsigned char dl, dh; unsigned short _4;
};

union REGS {
    struct DWORDREGS x;
    struct WORDREGS  w;
    struct BYTEREGS  h;
};

struct SREGS {
    unsigned short cs, ss, ds, es, fs, gs;
};

/* What an __interrupt function receives: the registers its prologue pushed.
 * Watcom's real one is three overlapping views of the same frame; only the
 * 32-bit one is spelled out here, because only it is used.
 *
 * The usual warning applies with particular force to this one. A DPMI
 * real-mode callback is entered with ES:EDI pointing at the register
 * structure, which is a fact about the DPMI host and not about this struct;
 * nothing here can check that the handler reads the right field, or that it
 * is a valid interrupt procedure at all -- __interrupt is defined away in
 * dos.h beside this file. */
struct INTPACKX {
    unsigned gs, fs, es, ds, edi, esi, ebp, esp;
    unsigned ebx, edx, ecx, eax, eip, cs, flags;
};

union INTPACK {
    struct INTPACKX x;
};

void segread(struct SREGS *sregs);
int  int386(int inter_no, const union REGS *in, union REGS *out);
int  int386x(int inter_no, const union REGS *in, union REGS *out,
             struct SREGS *sregs);

#endif

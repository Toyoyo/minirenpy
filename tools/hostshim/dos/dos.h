/* dos.h - enough of Open Watcom's for the syntax-only DOS pass. See i86.h
 * beside it for what this shim does and does not prove: in short, it catches
 * typos and would not have caught either of the faults that made it
 * necessary.
 *
 * The interrupt-vector calls are the part to be most careful with. Watcom's
 * take and return `void (__interrupt __far *)()`, a type gcc has no idea
 * about; both keywords are defined away below, which makes the declarations
 * parse and makes the shim completely blind to the thing that actually
 * matters about an interrupt handler -- that the compiler must save every
 * register and return with iret. Get that wrong on the real compiler and the
 * machine locks up; get it wrong here and nothing happens, because here
 * there is no interrupt.
 */

#ifndef MR_HOSTSHIM_DOS_H
#define MR_HOSTSHIM_DOS_H

/* Both return 0 on success. _dos_allocmem takes paragraphs and writes the
 * real-mode segment -- under a DOS extender that is conventional memory,
 * which is the only kind the BIOS can reach. */
unsigned _dos_allocmem(unsigned size, unsigned *seg);
unsigned _dos_freemem(unsigned seg);

/* Defined away: see the warning above. __loadds in particular is load-
 * bearing on the target and invisible here -- it is what makes an interrupt
 * handler load its own DS instead of running with whatever was current. */
#define __interrupt
#define __loadds
#define __far

void (*_dos_getvect(unsigned vector))(void);
void _dos_setvect(unsigned vector, void (*isr)(void));
void _chain_intr(void (*isr)(void));

void _disable(void);
void _enable(void);

#endif

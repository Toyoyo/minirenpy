/* conio.h - enough of Open Watcom's for the syntax-only DOS pass.
 *
 * The same warning as i86.h beside it: this proves that presdos.c's DOS
 * branch compiles, and nothing whatever about whether the port writes in it
 * are right. A wrong port number or a value written in the wrong order is
 * exactly as acceptable to this header as a correct one.
 *
 * Watcom's outp/outpw take and return int, and are intrinsics under -O; the
 * declarations here are only so the call expressions type-check.
 */

#ifndef MR_HOSTSHIM_CONIO_H
#define MR_HOSTSHIM_CONIO_H

int inp(int port);
int outp(int port, int value);
unsigned inpw(int port);
unsigned outpw(int port, unsigned value);

#endif

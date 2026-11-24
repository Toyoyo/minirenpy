/* Minimal windows.h shim so the platform-independent modules (lex.c, parse.c,
 * vfs.c, audio.c) can be compiled and tested with gcc on the build host. Only
 * the types those files mention are needed. */
#ifndef HOSTSHIM_WINDOWS_H
#define HOSTSHIM_WINDOWS_H

typedef unsigned long  DWORD;
typedef unsigned short WORD;
typedef unsigned int   UINT;
typedef int            BOOL;
typedef unsigned char  BYTE;
typedef char          *LPSTR;
typedef void          *HWND;
typedef void          *HDC;
typedef void          *HANDLE;

/* Virtual key codes, with the real values, so a harness can press the keys
 * the rhythm game reads without pulling in the platform header. */
#define VK_RETURN   0x0D
#define VK_ESCAPE   0x1B
#define VK_SPACE    0x20
#define VK_PRIOR    0x21
#define VK_LEFT     0x25
#define VK_UP       0x26
#define VK_RIGHT    0x27
#define VK_DOWN     0x28
#define VK_NUMPAD1  0x61
#define VK_NUMPAD2  0x62

#endif

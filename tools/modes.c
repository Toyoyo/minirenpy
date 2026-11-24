/* modes.c - print the VBE mode list, and say which one the engine would
 * choose. Built by Makefile.dos as MODES.EXE.
 *
 * This exists before the DOS port does, on purpose. The mode picker is the
 * one part of the port that can be got wrong in a way that leaves nothing
 * on screen and nothing in a log, and the only way to know what a given
 * card and BIOS actually report is to ask them. Run this on the target
 * first; its output is what the picker will be working from.
 *
 * It is also the smallest possible test that the DOS toolchain, the
 * extender, the DPMI calls and the real-mode scratch buffer all work --
 * none of which has been exercised yet. If this prints a sensible mode
 * list, the hard half of mr_vbe_enumerate is right.
 *
 * Links vbe.c + vbedos.c and nothing else.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "mr.h"
#include "vbe.h"

/* The engine's two, which vbedos.c calls. Printed rather than logged: this
 * runs at a DOS prompt and there is no reason to make the user find a file.
 */
void mr_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
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

#define MAX_MODES 128

int main(int argc, char **argv)
{
    mr_vbe_mode list[MAX_MODES];
    mr_vbe_want want;
    int n, i, version = 0;

    printf("minirenpy VBE mode list\n\n");

    n = mr_vbe_enumerate(list, MAX_MODES, &version);
    if (n < 0) {
        printf("No VESA BIOS extensions, or no usable mode.\n"
               "The engine would fall back to plain VGA.\n");
        return 1;
    }
    if (n == 0) {
        printf("\nThe BIOS reports no linear-framebuffer mode we can use.\n"
               "A VBE 1.2 BIOS will always say this: PhysBasePtr is a 2.0\n"
               "addition and there is nothing to map without it.\n");
        return 1;
    }

    /* Mode numbers are printed as 0x112 rather than 112, because that is
     * what mrpy.ini's `vbemode` takes and this list is where the number
     * gets copied from. Hex is the VBE standard's own spelling and every
     * other mode utility uses it; printing a bare 112 here while the ini
     * read it as decimal meant the number on the screen was not the number
     * to type, and nothing said so. */
    printf("\n  %-6s %-12s %-6s %-6s %-8s %s\n",
           "mode", "size", "bpp", "sig", "pitch", "picture");
    for (i = 0; i < n; i++) {
        int pw, ph;
        mr_vbe_fit(list[i].w, list[i].h, MR_STAGE_W, MR_STAGE_H, &pw, &ph);
        printf("  0x%03x  %4dx%-7d %-6d %-6d %-8d %dx%d\n",
               list[i].number, list[i].w, list[i].h,
               list[i].bpp, list[i].sigbpp, list[i].pitch, pw, ph);
    }

    printf("\n  mrpy.ini takes these as printed: vbemode=0x%03x\n",
           list[0].number);

    /* "picture" above is the part worth reading: the stage is 16:9 and a
     * 4:3 mode letterboxes, so 1024x768 shows the same amount as a
     * 1024x576 mode would and no more. That column, not the mode size, is
     * what the picker ranks on. */

    memset(&want, 0, sizeof(want));
    want.stage_w = MR_STAGE_W;
    want.stage_h = MR_STAGE_H;
    want.allow_8bpp = 1;

    printf("\nWhat the engine would choose, by requested depth:\n");
    for (i = 0; i < 4; i++) {
        static const int depths[4] = { 8, 16, 24, 32 };
        int k;

        want.want_bpp = depths[i];
        k = mr_vbe_pick(list, n, &want);
        if (k < 0)
            printf("  depth %-2d  nothing\n", depths[i]);
        else
            printf("  depth %-2d  mode 0x%03x  %dx%d  %dbpp\n",
                   depths[i], list[k].number, list[k].w, list[k].h,
                   list[k].bpp);
    }

    want.want_bpp = 16;
    want.allow_8bpp = 0;
    i = mr_vbe_pick(list, n, &want);
    printf("  truecolour only, depth 16: ");
    if (i < 0)
        printf("nothing\n");
    else
        printf("mode 0x%03x  %dx%d  %dbpp\n", list[i].number,
               list[i].w, list[i].h, list[i].bpp);

    (void)argc; (void)argv;
    return 0;
}

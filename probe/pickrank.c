/* probe: what the mode picker does with the Mystique's actual mode list.
 *
 * The list is transcribed from the machine's own LOG.TXT, so this is the
 * real question and not a made-up one: with no `depth` and no `vbemode`,
 * which mode gets chosen and why.
 */
#include <stdio.h>
#include <stdlib.h>

#include "mr.h"
#include "vbe.h"

void mr_fatal(const char *f, ...) { (void)f; exit(1); }
void mr_log(const char *f, ...) { (void)f; }

static mr_vbe_mode mk(int num, int w, int h, int bpp, int sig, int pitch)
{
    mr_vbe_mode m;

    m.number = num;
    m.w = w;
    m.h = h;
    m.pitch = pitch;
    m.bpp = bpp;
    m.sigbpp = sig;
    m.model = (sig < 15) ? MR_VBE_PACKED : MR_VBE_DIRECT;
    m.rpos = 0; m.rsize = 0;
    m.gpos = 0; m.gsize = 0;
    m.bpos = 0; m.bsize = 0;
    m.phys = 0xE6800000UL;
    return m;
}

int main(void)
{
    mr_vbe_mode list[32];
    mr_vbe_want want;
    int n = 0, i, pick;

    list[n++] = mk(0x100,  640,  400,  8,  8,  640);
    list[n++] = mk(0x101,  640,  480,  8,  8,  640);
    list[n++] = mk(0x103,  800,  600,  8,  8,  800);
    list[n++] = mk(0x105, 1024,  768,  8,  8, 1024);
    list[n++] = mk(0x107, 1280, 1024,  8,  8, 1280);
    list[n++] = mk(0x110,  640,  480, 16, 15, 1280);
    list[n++] = mk(0x111,  640,  480, 16, 16, 1280);
    list[n++] = mk(0x112,  640,  480, 32, 24, 2560);
    list[n++] = mk(0x113,  800,  600, 16, 15, 1600);
    list[n++] = mk(0x114,  800,  600, 16, 16, 1600);
    list[n++] = mk(0x115,  800,  600, 32, 24, 3200);
    list[n++] = mk(0x116, 1024,  768, 16, 15, 2048);
    list[n++] = mk(0x117, 1024,  768, 16, 16, 2048);
    list[n++] = mk(0x11c, 1600, 1200,  8,  8, 1600);

    want.stage_w = MR_STAGE_W;
    want.stage_h = MR_STAGE_H;
    want.allow_8bpp = 1;

    printf("stage %dx%d\n\n", MR_STAGE_W, MR_STAGE_H);
    printf("  %-6s %-11s %-6s %-12s %s\n",
           "mode", "mode size", "bpp", "picture", "bytes/frame");
    for (i = 0; i < n; i++) {
        int pw, ph;
        mr_vbe_fit(list[i].w, list[i].h, MR_STAGE_W, MR_STAGE_H, &pw, &ph);
        printf("  %-6x %4dx%-6d %2d(%2d) %5dx%-6d %ld\n",
               list[i].number, list[i].w, list[i].h,
               list[i].bpp, list[i].sigbpp, pw, ph,
               (long)pw * ph * (list[i].bpp / 8));
    }

    for (want.want_bpp = 0; want.want_bpp <= 32; want.want_bpp += 8) {
        pick = mr_vbe_pick(list, n, &want);
        printf("\ndepth=%-2d -> mode %x, %dx%d %dbpp",
               want.want_bpp, list[pick].number,
               list[pick].w, list[pick].h, list[pick].bpp);
    }
    printf("\n");

    want.want_bpp = 0;
    want.allow_8bpp = 0;
    pick = mr_vbe_pick(list, n, &want);
    printf("truecolour=1 -> mode %x, %dx%d %dbpp\n",
           list[pick].number, list[pick].w, list[pick].h, list[pick].bpp);
    return 0;
}

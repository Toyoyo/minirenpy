/* probe: run a raw 44100 stereo s16 file through sbconv, as the sink does.
 *
 *   sbfile in.raw out.u8 [rate [gain% [dither]]]
 *
 * Build with `make -C probe sbfile`.
 *
 * One mixer buffer at a time, through one converter, so the output is
 * exactly what the DMA ring would have held. Then the recording from the
 * machine can be compared against it: whatever they share is the
 * converter's doing, and whatever only the recording has is the card's.
 */
#include <stdio.h>
#include <stdlib.h>

#include "mr.h"
#include "wave.h"
#include "sbconv.h"

void mr_fatal(const char *f, ...) { (void)f; exit(1); }
void mr_log(const char *f, ...) { (void)f; }

static short in[MR_WAVE_FRAMES * 2];
static unsigned char out[MR_WAVE_BYTES];

int main(int argc, char **argv)
{
    mr_sbconv c;
    FILE *fi, *fo;
    int rate = (argc > 3) ? atoi(argv[3]) : MR_WAVE_RATE / 2;

    if (argc < 3)
        return fprintf(stderr, "usage: sbfile in.raw out.u8 "
                               "[rate [gain%% [dither]]]\n"), 1;
    if (!mr_sbconv_init(&c, rate, 1, 8))
        return fprintf(stderr, "unsupported rate %d\n", rate), 1;
    if (argc > 4)
        mr_sbconv_set_gain(&c, atoi(argv[4]));
    if (argc > 5)
        mr_sbconv_set_dither(&c, atoi(argv[5]));
    /* There was a third knob here, mr_sbconv_set_shaping. It is gone:
     * shaping is not optional any more -- err[] feeds the quantiser error
     * forward unconditionally -- and dithering is the switch, because
     * decorrelating the error is the part a listener has an opinion about.
     * See sbconv.h above mr_sbconv_set_dither. */

    fi = fopen(argv[1], "rb");
    fo = fopen(argv[2], "wb");
    if (!fi || !fo)
        return fprintf(stderr, "cannot open\n"), 1;

    while (fread(in, sizeof(short) * 2, MR_WAVE_FRAMES, fi) ==
           MR_WAVE_FRAMES) {
        mr_sbconv_run(&c, in, MR_WAVE_FRAMES, out);
        fwrite(out, 1, (size_t)mr_sbconv_out_bytes(&c, MR_WAVE_FRAMES), fo);
    }

    fclose(fi);
    fclose(fo);
    return 0;
}

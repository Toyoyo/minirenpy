/* probe: signal-to-noise of sbconv's 8-bit path against an ideal reference.
 *
 * Not a harness test -- a measurement. "It sounds like white noise" needs a
 * number before it needs a theory, and the number says whether the fault is
 * in the arithmetic or in the card plumbing around it.
 *
 * 8 bits is 6.02*8 + 1.76 = 49.9dB of ideal SNR for a full-scale sine, less
 * a little for headroom and for noise shaping that is not weighted. Anything
 * in the forties means the converter is doing its job and the noise is
 * somewhere else. Anything in the single digits is the bug.
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "mr.h"
#include "wave.h"
#include "sbconv.h"

void mr_fatal(const char *f, ...) { (void)f; exit(1); }
void mr_log(const char *f, ...) { (void)f; }

#define N 44100

static short in[N * 2];
static unsigned char out[N];

static void measure(const char *what, double freq, double amp, int rate)
{
    mr_sbconv c;
    int decim = MR_WAVE_RATE / rate;
    int i, n;
    double sig = 0.0, err = 0.0;

    for (i = 0; i < N; i++) {
        double v = amp * 32767.0 * sin(2.0 * 3.14159265358979 * freq * i /
                                       MR_WAVE_RATE);
        in[i * 2] = (short)v;
        in[i * 2 + 1] = (short)v;
    }

    mr_sbconv_init(&c, rate, 1, 8);

    /* In mixer-buffer sized pieces, as the card is fed. */
    n = 0;
    for (i = 0; i + MR_WAVE_FRAMES <= N; i += MR_WAVE_FRAMES) {
        mr_sbconv_run(&c, in + i * 2, MR_WAVE_FRAMES, out + n);
        n += MR_WAVE_FRAMES / decim;
    }

    /* The reference is the same sine sampled at the output rate: the
     * decimation itself is not an error. Compare after a few samples so the
     * error feedback has settled. */
    for (i = 8; i < n; i++) {
        double want = amp * 32767.0 *
                      sin(2.0 * 3.14159265358979 * freq *
                          ((double)i * decim + (decim - 1) / 2.0) /
                          MR_WAVE_RATE);
        double got = ((double)out[i] - 128.0) * 256.0;
        sig += want * want;
        err += (want - got) * (want - got);
    }

    printf("  %-34s %6.1f dB\n", what,
           (err > 0.0) ? 10.0 * log10(sig / err) : 99.9);
}

int main(void)
{
    printf("sbconv SNR, 8-bit mono output\n");
    measure("1kHz -3dBFS at 44100", 1000.0, 0.7, 44100);
    measure("1kHz -3dBFS at 22050", 1000.0, 0.7, 22050);
    measure("1kHz -20dBFS at 22050", 1000.0, 0.1, 22050);
    measure("100Hz -3dBFS at 22050", 100.0, 0.7, 22050);
    measure("4kHz -3dBFS at 22050", 4000.0, 0.7, 22050);
    return 0;
}

"""How much does a real decimation filter buy over averaging two samples?

Averaging two samples is a two-tap filter: its response is cos(pi f / fs),
which is -3dB at 11kHz and only -6dB at 15kHz. Everything above 11025 that
it fails to remove folds back into the audible band when the rate halves,
and that folded energy is the "noise" the downsampler is accused of.

Measured here against a brick-walled reference, for the real music, for
candidate FIR lengths -- because the filter has to run in an interrupt
handler on a 486 and the length is the whole cost.
"""
import numpy as np

RATE = 44100
src = np.fromfile('probe/src.raw', dtype='<i2').astype(np.float64)
src = src.reshape(-1, 2).mean(axis=1)

# The reference: brick wall just under the new Nyquist, then decimate.
n = len(src)
sp = np.fft.rfft(src)
f = np.fft.rfftfreq(n, 1.0 / RATE)
sp[f > 11025 * 0.95] = 0
ideal = np.fft.irfft(sp)[::2]


def dither8(x):
    """The same error-feedback quantiser sbconv uses."""
    out = np.empty(len(x))
    err = 0.0
    for i in range(len(x)):
        t = np.clip(x[i] + err, -32768, 32767)
        q = np.floor((t + 128) / 256.0)
        q = min(max(q, -128), 127)
        err = t - q * 256
        out[i] = q * 256
    return out


def score(y, label):
    k = min(len(y), len(ideal))
    a, b = y[:k], ideal[:k]
    d = a - b
    print("  %-28s %5.1f dB" % (label, 10 * np.log10((b ** 2).sum() /
                                                     max((d ** 2).sum(), 1e-9))))


def halfband(ntaps):
    """Windowed sinc at the half-band cutoff, odd length, symmetric."""
    m = (ntaps - 1) / 2.0
    i = np.arange(ntaps) - m
    h = np.sinc(i / 2.0) * np.hamming(ntaps)
    return h / h.sum()


print("against a brick-walled reference, music through the 8-bit path:")
box = (src[0::2] + src[1::2]) / 2.0
score(dither8(box), "average two (today)")

for ntaps in (7, 11, 15, 23, 31):
    h = halfband(ntaps)
    y = np.convolve(src, h, mode='same')[::2]
    score(dither8(y), "%d-tap windowed sinc" % ntaps)

# And without any quantisation, to separate the filter from the 8 bits.
print("\nthe filter alone, no 8-bit step:")
score(box, "average two")
for ntaps in (7, 11, 15, 23, 31):
    h = halfband(ntaps)
    score(np.convolve(src, h, mode='same')[::2], "%d-tap windowed sinc" % ntaps)

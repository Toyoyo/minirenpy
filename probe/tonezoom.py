"""Zoom in on the recorded test tone: where exactly is the non-tone energy?

The tone is generated as 20 cycles in a 2048-frame buffer, so it should be
one line. Anything else is the sink, and its *shape* names the fault:

  sidebands at the buffer rate (21.5Hz)   something wrong once per buffer
  sidebands at the half rate               ... once per DMA half
  harmonics at 2f, 3f                      distortion in the conversion
  a broad skirt                            jitter in the playback clock
  a flat floor                             quantisation, which is expected
"""
import sys
import wave

import numpy as np


def load(path):
    w = wave.open(path)
    a = np.frombuffer(w.readframes(w.getnframes()), dtype='<i2')
    a = a.astype(np.float64).reshape(-1, w.getnchannels()).mean(axis=1)
    return a, w.getframerate()


x, rate = load(sys.argv[1])

# A steady stretch from the middle.
n = 1 << 18
mid = len(x) // 2
seg = x[mid - n // 2: mid + n // 2]
sp = np.abs(np.fft.rfft(seg * np.hanning(n))) ** 2
f = np.fft.rfftfreq(n, 1.0 / rate)
peak = int(np.argmax(sp[1:])) + 1
f0 = f[peak]
print("carrier %.2f Hz (generated 430.66, expected 434.0 at the 22222Hz "
      "time constant)" % f0)

tot = sp[1:].sum()
print("\nthe twenty strongest components:")
idx = np.argsort(sp[1:])[::-1][:20] + 1
for i in sorted(idx, key=lambda j: f[j]):
    print("  %8.1f Hz  %6.1f dB   offset from carrier %+8.1f Hz"
          % (f[i], 10 * np.log10(sp[i] / sp[peak]), f[i] - f0))

print("\nenergy by distance from the carrier:")
d = np.abs(f - f0)
for lo, hi in [(0, 2), (2, 10), (10, 25), (25, 60), (60, 150), (150, 400)]:
    m = (d >= lo) & (d < hi)
    print("  %5.0f-%5.0f Hz away  %6.1f dB" % (lo, hi,
          10 * np.log10(max(sp[m].sum(), 1e-30) / tot)))

print("\nharmonics:")
for h in range(2, 7):
    m = np.abs(f - h * f0) < 6
    print("  %dx (%.0f Hz)  %6.1f dB rel carrier"
          % (h, h * f0, 10 * np.log10(max(sp[m].sum(), 1e-30) / sp[peak])))

# Amplitude envelope: is it modulated, and at what rate?
an = np.abs(np.fft.rfft(seg * np.hanning(n)))
env = np.abs(seg)
env = env - env.mean()
es = np.abs(np.fft.rfft(env * np.hanning(n))) ** 2
ef = np.fft.rfftfreq(n, 1.0 / rate)
m = (ef > 2) & (ef < 200)
k = int(np.argmax(es[m]))
print("\nstrongest amplitude-modulation rate below 200Hz: %.2f Hz"
      % ef[m][k])
print("  (one mixer buffer is 21.5 Hz, one DMA half the same, "
      "the PIT tick 100 Hz)")

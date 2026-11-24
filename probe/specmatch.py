"""Match the recording to the source by spectrogram, not by samples.

Sample alignment needs the two clocks to agree; spectrogram alignment does
not, which makes it the right tool both for "is this even the same music"
and for comparing noise floors. Band energies over time, cross-correlated.
"""
import sys
import wave

import numpy as np


def load_wav(path):
    w = wave.open(path)
    a = np.frombuffer(w.readframes(w.getnframes()), dtype='<i2')
    return a.astype(np.float64).reshape(-1, w.getnchannels()).mean(axis=1), \
        w.getframerate()


def spectrogram(x, rate, hop_ms=20.0, n=1024):
    hop = int(rate * hop_ms / 1000.0)
    k = (len(x) - n) // hop
    out = np.empty((k, n // 2 + 1))
    win = np.hanning(n)
    for i in range(k):
        out[i] = np.abs(np.fft.rfft(x[i * hop:i * hop + n] * win)) ** 2
    return out, np.fft.rfftfreq(n, 1.0 / rate)


def bandify(sp, f, edges):
    out = np.empty((sp.shape[0], len(edges) - 1))
    for j in range(len(edges) - 1):
        m = (f >= edges[j]) & (f < edges[j + 1])
        out[:, j] = sp[:, m].sum(axis=1)
    return np.log10(out + 1.0)


EDGES = [100, 200, 400, 700, 1100, 1700, 2500, 3600, 5200, 7500, 10000]

rec, rr = load_wav(sys.argv[1])
src, sr = load_wav(sys.argv[2]) if sys.argv[2].endswith('.wav') else (
    np.fromfile(sys.argv[2], dtype='<i2').astype(np.float64)
      .reshape(-1, 2).mean(axis=1), 44100)

a, fa = spectrogram(rec, rr)
b, fb = spectrogram(src, sr)
A = bandify(a, fa, EDGES)
B = bandify(b, fb, EDGES)
A = (A - A.mean(axis=0)) / (A.std(axis=0) + 1e-9)
B = (B - B.mean(axis=0)) / (B.std(axis=0) + 1e-9)

best, bestlag = -2.0, 0
for lag in range(0, len(B) - len(A)):
    seg = B[lag:lag + len(A)]
    c = float((A * seg).mean())
    if c > best:
        best, bestlag = c, lag
print("best spectrogram match: correlation %.3f at %.2f s into the source"
      % (best, bestlag * 0.020))

# What the recording has that the source does not, band by band, at that
# alignment: this is the added noise, and it does not need sample accuracy.
seg = b[bestlag:bestlag + len(a)]
ra = a[:len(seg)]
print("\nband energy, recording minus source (dB), after level matching:")
scale = np.sqrt((ra.sum(axis=1).mean()) / (seg.sum(axis=1).mean() + 1e-30))
for j in range(len(EDGES) - 1):
    m = (fa >= EDGES[j]) & (fa < EDGES[j + 1])
    m2 = (fb >= EDGES[j]) & (fb < EDGES[j + 1])
    d = 10 * np.log10((ra[:, m].sum() + 1e-30) /
                      (seg[:, m2].sum() * scale ** 2 + 1e-30))
    print("  %5d-%5d Hz  %+6.1f" % (EDGES[j], EDGES[j + 1], d))

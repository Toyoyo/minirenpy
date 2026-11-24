#!/usr/bin/env python3
"""Report the first-frame header of each MP3 given, plus whether every frame
in the file agrees with it.

The engine takes a track's length from its file size and the bitrate of its
first frame (src/audio.c:368), which is exact only for constant-bitrate files
and only when nothing but frames follows the tag. This says whether that holds
for the files the game actually ships, and reports the mode so a joint-stereo
file is visibly covered.
"""
import sys

BITRATES = {  # MPEG1 Layer III, kbps, by bitrate index
    1: 32, 2: 40, 3: 48, 4: 56, 5: 64, 6: 80, 7: 96,
    8: 112, 9: 128, 10: 160, 11: 192, 12: 224, 13: 256, 14: 320,
}
RATES = {0: 44100, 1: 48000, 2: 32000}
MODES = {0: "stereo", 1: "joint", 2: "dual", 3: "mono"}


def id3_len(d):
    if d[:3] == b"ID3":
        size = (d[6] << 21) | (d[7] << 14) | (d[8] << 7) | d[9]
        return 10 + size
    return 0


def frame_at(d, i):
    """(bitrate, rate, mode, frame_bytes) for a frame header at i, or None."""
    if i + 4 > len(d) or d[i] != 0xFF or (d[i + 1] & 0xE0) != 0xE0:
        return None
    ver, layer = (d[i + 1] >> 3) & 3, (d[i + 1] >> 1) & 3
    if ver != 3 or layer != 1:          # MPEG1 Layer III only
        return None
    br, sr = BITRATES.get(d[i + 2] >> 4), RATES.get((d[i + 2] >> 2) & 3)
    if not br or not sr:
        return None
    pad, mode = (d[i + 2] >> 1) & 1, (d[i + 3] >> 6) & 3
    return br, sr, MODES[mode], 144 * br * 1000 // sr + pad


for path in sys.argv[1:]:
    d = open(path, "rb").read()
    i = id3_len(d)
    while i < len(d) and not frame_at(d, i):
        i += 1
    first = frame_at(d, i)
    if not first:
        print("%-40s no MPEG1 Layer III frame found" % path)
        continue

    tag = i                              # bytes before the first frame
    br0, sr0, mode0, _ = first
    n, varies, trailing = 0, set(), 0
    while i < len(d):
        f = frame_at(d, i)
        if not f:
            trailing = len(d) - i
            break
        if f[0] != br0:
            varies.add(f[0])
        n += 1
        i += f[3]

    # Xing/Info: a tag inside the first frame whose first field is the frame
    # count. Present in every file this game ships, which is what makes a fix
    # possible without walking the whole file.
    xing = ""
    side = 17 if mode0 == "mono" else 32
    o = tag + 4 + side
    if d[o:o + 4] in (b"Xing", b"Info") and d[o + 7] & 1:
        nx = int.from_bytes(d[o + 8:o + 12], "big")
        xing = "  xing=%d(%.0fms)" % (nx, nx * 1152 * 1000.0 / sr0)

    # 1152 samples per MPEG1 Layer III frame.
    exact = n * 1152 * 1000.0 / sr0
    guess = len(d) * 8.0 / br0           # the engine's formula, in ms
    print("%-40s %d kbps %d Hz %-6s frames=%d tag=%d trail=%d  "
          "exact=%.0fms engine=%.0fms err=%+.0fms%s%s"
          % (path, br0, sr0, mode0, n, tag, trailing, exact, guess,
             guess - exact, "  VBR" if varies else "", xing))

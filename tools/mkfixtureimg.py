#!/usr/bin/env python3
"""Write a PNG for a harness fixture.

The four images in tools/harness/story/images are flat colours that were
committed without a generator, which was fine until a test needed a *taller
than the stage* background: a vertical pan is only observable on one, and a
flat one is not observable at all -- it covers the whole stage at both ends of
the pan, so every pixel check reads the same thing whatever the engine does.

So this writes bands too. `tall.png` is 1280x1460 with its upper half one
colour and its lower half another, which makes "which part of the backdrop is
on screen" a single pixel read:

    scene tall              -> y = 720-1460 = -740, the lower band showing
    show tall: yoffset 740  -> y = 0,               the upper band showing

And the same trap caught im.Blur: blurring a flat image gives the flat image
back, so a blur test on these fixtures passes whether or not the blur runs at
all. `-m quad` writes four quadrants, which has a hard edge on both axes and
so is sensitive to an x pass, a y pass, a radius and an axis swap -- any of
which would otherwise go unseen.

Modes:
    flat       one colour (the second is ignored)
    hband      upper half / lower half        (a y-axis edge)
    vband      left half / right half         (an x-axis edge)
    quad       both, the second colour on the off-diagonal
    room       a scene background: two-axis gradient, an 80px grid and
               five hard-edged blocks. Both colours are ignored.

`room` exists because the choice fixture's background used to be a 627KB
background from the game, symlinked in, and nothing in choicetest looks at
it: `choice/script.rpy` opens with `scene room` only because a menu has to
be drawn over something. What the fixture needs is a 1280x720 image, not
that one.

It is structured rather than flat for the reason the whole first half of
this docstring is about, and it contains **no blue** -- which is not a
style choice and is checked, not trusted. choicetest finds the menu's hover
highlight by looking for a markedly blue pixel:

    MR_B(c) > 150 && MR_B(c) > MR_R(c) + 80 && MR_B(c) > MR_G(c) + 40
                                                     (choicetest.c:175)

and it asserts not only that the bar lands on the hovered row but that it
appears *nowhere else*. The first version of this background had a
blue-violet gradient and cool blocks, so the background read as highlight
everywhere and three assertions went red. The game's background passed
those checks by happening not to be blue; a fixture should say so instead
of inheriting it. See no_blue below.

Usage:
    tools/mkfixtureimg.py [-m <mode>] <out.png> <w> <h> <rrggbb[aa]> [<rrggbb[aa]>]

With no -m the mode is hband when two colours are given and flat otherwise,
which is how the existing fixtures were made.

A colour may carry an eighth and ninth hex digit, which is alpha, and makes
the file RGBA instead of RGB. Six-digit colours still write RGB, so the
fixtures that predate this come out byte-identical. Alpha is here because a
cross-fade between two frames of an animation has to interpolate it, and
getting that wrong -- mixing straight RGB independently of straight alpha --
is only visible where the two frames' alphas *differ*. Two opaque frames blend
identically either way, so a fixture without alpha cannot fail that test.
"""

import struct
import sys
import zlib


# Warm blocks for `room`, hard-edged so a blur has something to soften.
# None of them is blue by no_blue's reckoning.
ROOM_BLOCKS = [
    ((240, 180), (214, 160, 90)),
    ((1040, 180), (96, 150, 110)),
    ((240, 540), (198, 118, 112)),
    ((1040, 540), (162, 170, 92)),
    ((640, 360), (124, 112, 96)),
]
ROOM_GRID = (236, 226, 206)
ROOM_EDGE = (248, 240, 224)


def no_blue(r, g, b):
    """choicetest.c:175's hover predicate. True means this pixel would be
    mistaken for the menu's hover highlight."""
    return b > 150 and b > r + 80 and b > g + 40


def room_rows(w, h, nchan):
    """Yield one `room` row at a time, as a list of per-pixel tuples."""
    for y in range(h):
        # Blue is the weakest channel everywhere and capped well under 150.
        row = [(60 + x * 150 // w,
                50 + y * 120 // h,
                40 + (x + y) * 60 // (w + h)) for x in range(w)]

        # A grid at 80px, a divisor of both stage axes, so a scale that is
        # off by a pixel shows as uneven spacing rather than as nothing.
        if y % 80 == 0 or y == h - 1:
            row = [ROOM_GRID] * w
        else:
            for x in range(0, w, 80):
                row[x] = ROOM_GRID

        for (cx, cy), colour in ROOM_BLOCKS:
            if cy - 60 <= y <= cy + 60:
                for x in range(max(0, cx - 90), min(w, cx + 91)):
                    row[x] = colour

        if y == 0 or y == h - 1:
            row = [ROOM_EDGE] * w
        else:
            row[0] = row[w - 1] = ROOM_EDGE

        for x, (r, g, b) in enumerate(row):
            if no_blue(r, g, b):
                raise SystemExit(
                    "room: pixel at %d,%d is (%d,%d,%d), which choicetest "
                    "would read as the menu hover highlight" % (x, y, r, g, b))

        yield [p + (255,) if nchan == 4 else p for p in row]


def chunk(tag, data):
    return (struct.pack(">I", len(data)) + tag + data +
            struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def write_png(path, w, h, a, b, mode):
    raw = bytearray()

    if mode == "room":
        # Filter 1 (Sub: each byte minus the one a pixel to its left), not 0
        # as the band modes use. A horizontal gradient is nearly constant
        # under that difference, so it deflates better: 120,267 bytes
        # unfiltered against 88,941 here. Not the order of magnitude a pure
        # gradient would give -- the grid lines and the blocks interrupt the
        # run every 80 pixels, and the vertical component changes the whole
        # row's baseline each line. The band modes keep filter 0 so their
        # committed files stay byte-identical.
        bpp = len(a)
        for row in room_rows(w, h, bpp):
            raw.append(1)
            flat = bytearray()
            for px in row:
                flat += bytes(px)
            for i in range(len(flat)):
                left = flat[i - bpp] if i >= bpp else 0
                raw.append((flat[i] - left) & 0xFF)
        ctype = 6 if len(a) == 4 else 2
        png = (b"\x89PNG\r\n\x1a\n" +
               chunk(b"IHDR",
                     struct.pack(">IIBBBBB", w, h, 8, ctype, 0, 0, 0)) +
               chunk(b"IDAT", zlib.compress(bytes(raw), 9)) +
               chunk(b"IEND", b""))
        with open(path, "wb") as f:
            f.write(png)
        return

    for y in range(h):
        raw.append(0)                       # filter: none
        if mode == "flat":
            raw += bytes(a) * w
        elif mode == "hband":
            raw += bytes(a if y < h // 2 else b) * w
        elif mode == "vband":
            raw += bytes(a) * (w // 2) + bytes(b) * (w - w // 2)
        elif mode == "quad":
            top = y < h // 2
            left, right = (a, b) if top else (b, a)
            raw += bytes(left) * (w // 2) + bytes(right) * (w - w // 2)
        else:
            raise SystemExit("unknown mode: %s" % mode)

    ctype = 6 if len(a) == 4 else 2
    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, ctype, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(bytes(raw), 9)) +
           chunk(b"IEND", b""))

    with open(path, "wb") as f:
        f.write(png)


def rgb(s):
    s = s.lstrip("#")
    if len(s) not in (6, 8):
        raise SystemExit("colour must be rrggbb or rrggbbaa: %s" % s)
    return tuple(int(s[i:i + 2], 16) for i in range(0, len(s), 2))


def main(argv):
    argv = argv[1:]
    mode = None
    if len(argv) > 1 and argv[0] == "-m":
        mode = argv[1]
        argv = argv[2:]

    if len(argv) < 4:
        sys.stderr.write(__doc__)
        return 2

    out, w, h = argv[0], int(argv[1]), int(argv[2])
    a = rgb(argv[3])
    b = rgb(argv[4]) if len(argv) > 4 else a
    if len(a) != len(b):
        raise SystemExit("both colours must be the same width (one file has "
                         "one pixel format)")
    if mode is None:
        mode = "hband" if len(argv) > 4 else "flat"

    write_png(out, w, h, a, b, mode)
    print("%s: %dx%d %s" % (out, w, h, mode))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

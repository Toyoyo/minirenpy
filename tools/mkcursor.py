#!/usr/bin/env python3
"""Bake a PNG into the mouse pointer the DOS build draws.

    python3 tools/mkcursor.py tools/cursor.png src/cursordat.c [hotx hoty]

Why a bake and not a load. The pointer has to be on screen from the first
frame and has to survive anything going wrong afterwards -- a pointer that
depends on the archive mounting is a pointer that is missing exactly when
the player needs to click the error away. It is also small: twelve by
nineteen pixels is 912 bytes, where the smallest thing in the pak is tens of
kilobytes.

Why a PNG and not pixels in a .c file. Because the art is meant to be
changed, and by somebody drawing rather than somebody typing hex. Point
CURSOR_PNG at your own file and rebuild; a game with a reason to have its
own pointer -- a little biwa, say -- changes one line of the makefile.

The output is one C file holding a straight-alpha 0xAARRGGBB array, which is
what mr_surface is, so the engine's own blitter composites it with no
conversion.

Accepts the PNG subset this repository actually produces and that an
exported icon is likely to be: 8 bits per channel, non-interlaced, colour
type 0 (grey), 2 (RGB), 3 (palette, with tRNS if present) or 6 (RGBA), with
any of the five scanline filters. Anything else is refused by name rather
than decoded wrongly -- a 16-bit PNG silently read as 8 would be a pointer
of noise, and the failure would show up on the DOS machine rather than
here.
"""

import struct
import sys
import zlib


def die(msg):
    sys.stderr.write("mkcursor: %s\n" % msg)
    sys.exit(1)


def read_chunks(data):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        die("not a PNG")
    pos = 8
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        tag = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        yield tag, body
        pos += 12 + length


def unfilter(raw, w, h, bpp):
    """Undo the per-scanline filters. stride is bytes per row, bpp the
    distance back to the pixel on the left."""
    stride = w * bpp
    out = bytearray(stride * h)
    pos = 0
    for y in range(h):
        ft = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        up = out[(y - 1) * stride:y * stride] if y else bytearray(stride)

        if ft == 0:
            pass
        elif ft == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + up[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + up[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = up[i]
                c = up[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        else:
            die("scanline filter %d is not a PNG filter" % ft)

        out[y * stride:(y + 1) * stride] = line
    return out


def decode(path):
    with open(path, "rb") as f:
        data = f.read()

    idat = b""
    pal = None
    trns = None
    w = h = depth = ctype = None

    for tag, body in read_chunks(data):
        if tag == b"IHDR":
            w, h, depth, ctype, comp, filt, interlace = \
                struct.unpack(">IIBBBBB", body)
            if depth != 8:
                die("%d bits per channel; only 8 is supported" % depth)
            if interlace:
                die("interlaced; save it without Adam7")
            if comp or filt:
                die("unknown compression or filter method")
        elif tag == b"PLTE":
            pal = body
        elif tag == b"tRNS":
            trns = body
        elif tag == b"IDAT":
            idat += body
        elif tag == b"IEND":
            break

    if w is None:
        die("no IHDR")

    channels = {0: 1, 2: 3, 3: 1, 6: 4, 4: 2}.get(ctype)
    if channels is None:
        die("colour type %d is not supported" % ctype)

    raw = unfilter(zlib.decompress(idat), w, h, channels)

    px = []
    for y in range(h):
        row = []
        for x in range(w):
            i = (y * w + x) * channels
            if ctype == 0:
                v = raw[i]
                row.append((255, v, v, v))
            elif ctype == 4:
                v = raw[i]
                row.append((raw[i + 1], v, v, v))
            elif ctype == 2:
                row.append((255, raw[i], raw[i + 1], raw[i + 2]))
            elif ctype == 6:
                row.append((raw[i + 3], raw[i], raw[i + 1], raw[i + 2]))
            else:
                n = raw[i]
                if pal is None:
                    die("colour type 3 with no PLTE")
                a = trns[n] if (trns and n < len(trns)) else 255
                row.append((a, pal[n * 3], pal[n * 3 + 1], pal[n * 3 + 2]))
        px.append(row)
    return w, h, px


def main():
    if len(sys.argv) not in (3, 5):
        sys.stderr.write(
            "usage: mkcursor.py <in.png> <out.c> [hotx hoty]\n")
        return 1

    src, dst = sys.argv[1], sys.argv[2]
    hotx = int(sys.argv[3]) if len(sys.argv) == 5 else 0
    hoty = int(sys.argv[4]) if len(sys.argv) == 5 else 0

    w, h, px = decode(src)

    if w < 1 or h < 1 or w > 64 or h > 64:
        die("%dx%d: a pointer should be between 1x1 and 64x64" % (w, h))
    if not (0 <= hotx < w and 0 <= hoty < h):
        die("hotspot %d,%d is outside a %dx%d image" % (hotx, hoty, w, h))

    out = []
    out.append("/* Generated by tools/mkcursor.py from %s. Do not edit.\n"
               " *\n"
               " * The mouse pointer the DOS build draws, as straight-alpha\n"
               " * 0xAARRGGBB -- an mr_surface's own pixel format, so the\n"
               " * engine's blitter composites it with no conversion.\n"
               " *\n"
               " * To change it, change the PNG: CURSOR_PNG in Makefile.dos.\n"
               " */\n" % src)
    out.append('#include "cursor.h"\n\n')
    out.append("const int mr_cursor_w = %d;\n" % w)
    out.append("const int mr_cursor_h = %d;\n" % h)
    out.append("const int mr_cursor_hotx = %d;\n" % hotx)
    out.append("const int mr_cursor_hoty = %d;\n\n" % hoty)
    out.append("const mr_u32 mr_cursor_px[%d] = {\n" % (w * h))

    for y in range(h):
        line = "   "
        for x in range(w):
            a, r, g, b = px[y][x]
            line += " 0x%02X%02X%02X%02Xu," % (a, r, g, b)
            if len(line) > 68:
                out.append(line + "\n")
                line = "   "
        if line.strip():
            out.append(line + "\n")
    out.append("};\n")

    with open(dst, "w") as f:
        f.write("".join(out))

    opaque = sum(1 for row in px for p in row if p[0])
    print("%s: %dx%d, hotspot %d,%d, %d of %d pixels not transparent"
          % (dst, w, h, hotx, hoty, opaque, w * h))
    return 0


if __name__ == "__main__":
    sys.exit(main())

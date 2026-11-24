"""Prototype: PNG -> VGA text mode using CP437 block glyphs.

usage: python3 -I cp437.py in.png out_prefix rows palette bg diffusion
  rows      25 (8x16 font) | 50 (8x8 font)
  palette   grey16  16 even greys (DAC reprogrammed)
            ega     stock greys 0/8/7/15
            vga16   stock 16-colour text palette
            adapt16 16 colours picked from the image (DAC reprogrammed)
  bg        grey level 0-255 behind transparent pixels
  diffusion 0-1, cell-level error diffusion strength
  [qmethod] adapt16 only: MEDIANCUT (default) | MAXCOVERAGE | FASTOCTREE
Writes out_prefix.png (source | result, at 4:3), out_prefix.bin (char,attr
pairs, 80 columns, row-major) and, for reprogrammed palettes, out_prefix.pal
(16 x RGB, 6-bit DAC values, in attribute order).
"""
import sys
import numpy as np
from PIL import Image

src, out, rows = sys.argv[1], sys.argv[2], int(sys.argv[3])
pal_name = sys.argv[4] if len(sys.argv) > 4 else 'grey16'
BGL = int(sys.argv[5]) if len(sys.argv) > 5 else 0
DIFF = float(sys.argv[6]) if len(sys.argv) > 6 else 0.0
QMETHOD = sys.argv[7] if len(sys.argv) > 7 else 'MEDIANCUT'
CW, CH = 8, (16 if rows == 25 else 8)
COLS = 80

def rowpat(a, b, h):
    return np.array([[(r >> (7 - x)) & 1 for x in range(8)]
                     for r in ([a, b] * h)[:h]], float)

def glyphs(h):
    g = {}
    g[0x20] = np.zeros((h, 8))
    g[0xDB] = np.ones((h, 8))
    m = np.zeros((h, 8)); m[:h // 2] = 1; g[0xDF] = m
    g[0xDC] = 1 - m
    m = np.zeros((h, 8)); m[:, :4] = 1; g[0xDD] = m
    g[0xDE] = 1 - m
    g[0xB0] = rowpat(0x11, 0x44, h)   # IBM VGA ROM patterns
    g[0xB1] = rowpat(0x55, 0xAA, h)
    g[0xB2] = rowpat(0xDD, 0x77, h)
    return g

G = glyphs(CH)
codes = list(G)
masks = np.stack([G[c] for c in codes])            # (n, CH, CW)

def to_lin(s): return np.where(s <= .04045, s / 12.92, ((s + .055) / 1.055) ** 2.4)
def to_srgb(l): return np.where(l <= .0031308, l * 12.92, 1.055 * l ** (1 / 2.4) - .055)

VGA16 = [0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
         0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF]

# 640x400 shown on 4:3 -> pixels are 1.2x taller than wide.
im = Image.open(src).convert('RGBA')
bgc = Image.new('RGBA', im.size, (BGL, BGL, BGL, 255))
bgc.alpha_composite(im)
g = bgc.convert('RGB')
if pal_name in ('grey16', 'ega'):
    g = g.convert('L').convert('RGB')
H = rows * CH
W = round(H * 1.2 * g.width / g.height)
g = g.resize((W, H), Image.LANCZOS)
canvas = Image.new('RGB', (COLS * CW, H), (BGL, BGL, BGL))
canvas.paste(g, ((COLS * CW - W) // 2, 0))
S = to_lin(np.asarray(canvas, float) / 255.0)       # (H, W, 3)

if pal_name == 'grey16':
    pal_s = np.repeat(np.arange(16)[:, None] / 15.0, 3, 1)
    pal_idx = list(range(16))
elif pal_name == 'ega':
    pal_s = np.repeat(np.array([[0], [85], [170], [255]]) / 255.0, 3, 1)
    pal_idx = [0, 8, 7, 15]
elif pal_name == 'vga16':
    pal_s = np.array([[(c >> 16) & 255, (c >> 8) & 255, c & 255] for c in VGA16]) / 255.0
    pal_idx = list(range(16))
else:
    # Quantise the source (method from argv[7]); snap to the DAC's 6 bits per channel.
    q = canvas.quantize(16, method=getattr(Image.Quantize, QMETHOD))
    p = np.array(q.getpalette()[:48], float).reshape(16, 3)
    pal_s = np.round(p / 255.0 * 63) / 63
    pal_idx = list(range(16))
pal = to_lin(pal_s)                                  # (NP, 3)
NP = len(pal)

# Error is weighted towards green, roughly as the eye weights it.
WGT = np.array([.6, 1.0, .35])

# Compare at 2x2-pooled resolution: dither patterns are judged by the tone
# they make, edges still count.
def pool(a):
    sh = (a.shape[0] // 2, 2, a.shape[1] // 2, 2) + a.shape[2:]
    return a.reshape(sh).mean(axis=(1, 3))

pm = np.stack([pool(m) for m in masks]).reshape(len(codes), -1)    # (n, P)
# cand[f, b, n, P, 3] = bg + mask * (fg - bg)
cand = (pal[None, :, None, None, :] +
        pm[None, None, :, :, None] * (pal[:, None, None, None, :] - pal[None, :, None, None, :]))
cand = cand.reshape(-1, pm.shape[1] * 3)
wv = np.tile(WGT, pm.shape[1])
cand_idx = [(f, b, c) for f in range(NP) for b in range(NP) for c in range(len(codes))]

out_chars = np.zeros((rows, COLS), np.uint8)
out_attr = np.zeros((rows, COLS), np.uint8)
render = np.zeros_like(S)
err = S.copy()
for cy in range(rows):
    for cx in range(COLS):
        ys, xs = slice(cy * CH, (cy + 1) * CH), slice(cx * CW, (cx + 1) * CW)
        blk = err[ys, xs]
        tgt = pool(np.clip(blk, 0, 1)).ravel()
        k = int(np.argmin((((cand - tgt) ** 2) * wv).sum(1)))
        f, b, c = cand_idx[k]
        r = pal[b] + masks[c][:, :, None] * (pal[f] - pal[b])
        render[ys, xs] = r
        out_chars[cy, cx] = codes[c]
        out_attr[cy, cx] = (pal_idx[b] << 4) | pal_idx[f]
        if DIFF:
            # Floyd-Steinberg on the cell's mean error, at cell granularity.
            e = (blk - r).mean(axis=(0, 1))
            for dx, dy, w in ((1, 0, 7), (-1, 1, 3), (0, 1, 5), (1, 1, 1)):
                nx, ny = cx + dx, cy + dy
                if 0 <= nx < COLS and ny < rows:
                    err[ny * CH:(ny + 1) * CH, nx * CW:(nx + 1) * CW] += e * w / 16 * DIFF

prev = Image.fromarray((to_srgb(render) * 255).round().astype(np.uint8), 'RGB')
prev = prev.resize((COLS * CW, round(H * 1.2)), Image.NEAREST)
orig = canvas.resize(prev.size, Image.LANCZOS)
both = Image.new('RGB', (prev.width * 2 + 8, prev.height), (128, 128, 128))
both.paste(orig, (0, 0)); both.paste(prev, (prev.width + 8, 0))
both.save(out + '.png')
np.stack([out_chars, out_attr], -1).tofile(out + '.bin')
if pal_name in ('grey16', 'adapt16'):
    np.round(pal_s * 63).astype(np.uint8).tofile(out + '.pal')
counts = {hex(c): int((out_chars == c).sum()) for c in codes}
print(counts)

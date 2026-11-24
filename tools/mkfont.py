#!/usr/bin/env python3
"""Bake the game's fonts into an 8-bit coverage atlas the engine can blit.

Windows 3.1 GDI renders TrueType without antialiasing, and there is no
FreeType on the target, so glyphs are rasterised here on the build host and
shipped as a coverage bitmap plus metrics. The runtime then alpha-blends them,
which both matches Ren'Py's antialiased look and costs nothing at load time.

The default face mirrors what the game's own gui.rpy does:

    fg.add("DejaVuSans.ttf",        0x0020, 0x2e7f)
    fg.add("NotoSansJP-Regular.ttf", 0x2e80, 0xffff)

The character set is taken from the scripts rather than guessed, so the atlas
carries the accented Latin the French text needs and the handful of kanji in
the extras menu, and nothing else.

Each character is baked in four variants -- regular, bold, italic and bold
italic -- so that {b} and {i} render as real faces rather than a shear or a
double strike. A variant is left out when its face is the same file as the
regular one, which is the case for all of NotoSansJP: the runtime falls back
to the regular glyph, so the CJK block is stored once rather than four times.

A game may put a *second* face on part of its interface -- Hinata sets

    define gui.interface_text_font = "Pecita-0owz.otf"

which style.c's expand_text already resolves into the `font` property of every
interface style. Those faces are discovered the same way as the sizes: from
the game's own `gui.*_font` defines, with DejaVu+Noto as the fallback when a
game declares nothing. Each gets its own atlas, `font-<slug><size>.dat`, and
`fonts.lst` lists it on a `face` line keyed by the filename gui.rpy named, so
the engine matches on that string and never has to know about slugs.

A named face carries *only* what it covers, and the runtime falls back to the
default atlas of the same size for the rest (mr_font_set_fallback). That is
what keeps the cost down: the CJK block is 80% of an atlas and Pecita has no
kanji, so duplicating it per face would have added megabytes to a resident
allocation. Coverage is probed rather than assumed -- see notdef_mask.

Usage:
    python3 tools/mkfont.py <game-dir> <out-dir> [sizes...]
"""

import os
import re
import struct
import sys

from PIL import Image, ImageFont

MAGIC = 0x4E46524D          # 'MRFN' little-endian
VERSION = 2                 # v2 keys glyphs by (codepoint, style)

SYSTEM_FONT_DIRS = [
    "/usr/share/fonts/truetype/dejavu",
    "/usr/share/fonts/dejavu",
    "/usr/share/fonts/TTF",
]

# Index order must match the runtime's: bit 0 is bold, bit 1 is italic.
STYLE_NAMES = ["", "-Bold", "-Oblique", "-BoldOblique"]
NSTYLES = len(STYLE_NAMES)

CJK_START = 0x2E80


def find_face(game_dir, filename, suffix):
    """<stem><suffix><ext>, in the game dir or the host's font dirs.

    Returns None if that variant is not there. The suffix convention is
    DejaVu's, which is also Ren'Py's default font; a face named with some
    other scheme (`-Italic`, `-SemiBold`) simply finds no variants and renders
    upright, which is what it did before any of this was configurable.
    """
    stem, ext = os.path.splitext(filename)
    name = "%s%s%s" % (stem, suffix, ext)
    local = os.path.join(game_dir, name)
    if os.path.exists(local):
        return local
    for d in SYSTEM_FONT_DIRS:
        p = os.path.join(d, name)
        if os.path.exists(p):
            return p
    return None


def face_paths(game_dir, filename, required):
    """One path per style index for `filename`, variants falling back to
    regular. Returns None when the regular face itself cannot be found."""
    out = []
    for suffix in STYLE_NAMES:
        p = find_face(game_dir, filename, suffix)
        if p is None:
            if not suffix:
                if required:
                    raise SystemExit("%s not found; put a copy in the game dir"
                                     % filename)
                return None
            # Missing variant: fall back to the regular face, which the
            # baker then leaves out of the file entirely.
            print("note: %s%s not found, %s text in %s will be upright"
                  % (os.path.splitext(filename)[0], suffix,
                     suffix.lstrip("-").lower(), filename))
            p = out[0]
        out.append(p)
    return out


def slug_for(filename):
    """A short lowercase tag for a font filename, for use in a path.

    "Pecita-0owz.otf" -> "pecita". The slug only has to be stable and unique
    among the game's faces; the engine matches faces by the filename gui.rpy
    gave, which fonts.lst carries verbatim, so nothing depends on the exact
    spelling here.
    """
    base = os.path.splitext(os.path.basename(filename))[0]
    s = re.sub(r"[^A-Za-z0-9]+", "", base.split("-")[0]).lower()
    return s or "face"


def collect_faces(game_dir):
    """The faces the game's GUI names, as (filename, resolved-path) pairs.

    Scrapes `gui.*_font = ...` out of the scripts. The value may be a literal
    ("DejaVuSans.ttf"), another gui variable (gui.button_text_font =
    gui.interface_text_font), or a call that wraps one -- Miwa has

        define gui.text_font = _with_jp("DejaVuSans.ttf")

    so what is taken is the first quoted string, after following gui.* chains.
    That is enough for every form the two games use and degrades to "no named
    face" rather than to a wrong one.

    Commented-out defines are skipped, which matters: Hinata has three
    `gui.interface_text_font` lines and only the last is live.

    gui.text_font is the default face and is returned separately, because that
    is the one baked as plain font<size>.dat.
    """
    exprs = {}

    pat = re.compile(r"^\s*(?:define\s+)?(gui\.[A-Za-z_]*font)\s*=\s*(.+?)\s*$")

    paths = []
    for root, _dirs, names in os.walk(game_dir):
        for name in names:
            if name.endswith(".rpy") or name.endswith(".rpym"):
                paths.append(os.path.join(root, name))

    for path in sorted(paths):
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.lstrip().startswith("#"):
                    continue
                m = pat.match(line)
                if m:
                    exprs[m.group(1)] = m.group(2)

    def resolve(var, depth=0):
        """The filename `var` ends up naming, or None."""
        if depth > 8 or var not in exprs:
            return None
        expr = exprs[var]
        q = re.search(r'["\']([^"\']+\.(?:ttf|otf|ttc))["\']', expr, re.I)
        if q:
            return q.group(1)
        ref = re.search(r"\b(gui\.[A-Za-z_]*font)\b", expr)
        if ref and ref.group(1) != var:
            return resolve(ref.group(1), depth + 1)
        return None

    default = resolve("gui.text_font")

    named = []
    seen = set()
    for var in sorted(exprs):
        fn = resolve(var)
        if not fn:
            continue
        key = fn.lower()
        if default and key == default.lower():
            continue                      # the default face, not a second one
        if key in seen:
            continue
        # The game dir first, then the host's font dirs -- the same search the
        # default face gets. A font need not be shipped beside the script to
        # be usable at bake time, since nothing but the atlas reaches the
        # target.
        local = find_face(game_dir, fn, "")
        if local is None:
            print("note: %s names %s, which was not found; interface text "
                  "there will use the default face" % (var, fn))
            continue
        seen.add(key)
        named.append((fn, local))

    return default, named


def collect_sizes(game_dir):
    """Every text size the game's GUI asks for, from its own gui.rpy.

    Taken from the script rather than passed in, because a hand-maintained
    list has to agree with ui.c's and with the Makefile's, and there was no
    way to notice when it stopped: mr_ui_font picks the nearest baked size, so
    a missing one is a slightly wrong-looking label rather than an error.

    Only `*_text_size` and `*_size` *text* properties count. gui.bar_size,
    gui.scrollbar_size and gui.slider_size are bar thicknesses in pixels, not
    font sizes, and baking a 12px and a 25px atlas for them would be waste.

    Per-device override blocks are skipped. They are Ren'Py's way of retuning
    the GUI for phones and tablets, and they never apply on this target.
    Scanning them was pulling in sizes nothing ever asked for, and every baked
    atlas is loaded resident at startup (ui.c:240), so the waste is memory on
    the target, not just pak bytes.

    There are *two* spellings and both have to be recognised. The newer one is
    a decorator:

        @gui.variant
        def small():

    the older one, which the Ren'Py 7 templates generated and which Hinata
    still carries, is a plain conditional inside `init python`:

        if renpy.variant("small"):

    Only the decorator was handled at first, which is why Hinata baked a 20, a
    25, a 34 and a 36 -- about 1.1MB of its 2.9MB of atlases -- for its phone
    variant. Miwa uses the decorator, so both games had to be looked at before
    this looked like one construct with one spelling.
    """
    sizes = set()

    # Matches `define gui.text_size = 22` and the bare `gui.text_size = 22`
    # that appears inside init blocks.
    pat = re.compile(r"^\s*(?:define\s+)?gui\.([A-Za-z_]*?size)\s*=\s*(\d+)")
    skip = ("bar_size", "scrollbar_size", "slider_size")
    # `if renpy.variant("small"):`, the pre-decorator spelling.
    vpat = re.compile(r"^\s*if\s+renpy\.variant\s*\(")

    paths = []
    for root, _dirs, names in os.walk(game_dir):
        for name in names:
            if name.endswith(".rpy") or name.endswith(".rpym"):
                paths.append(os.path.join(root, name))

    for path in sorted(paths):
        with open(path, encoding="utf-8", errors="replace") as f:
            lines = f.readlines()

        # None outside a variant; "def" once the decorator has been seen and
        # its `def` line is still to come; otherwise the indent of that `def`,
        # whose body is everything indented further.
        #
        # The decorator and its `def` sit at the *same* indent, so the state
        # cannot simply be "the decorator's indent" -- the `def` line then
        # looks like the end of the block and the body is scanned after all,
        # which is how the first version of this still picked up the phone
        # variant's sizes.
        variant = None
        for line in lines:
            stripped = line.strip()

            if variant == "def":
                if stripped.startswith("def "):
                    variant = len(line) - len(line.lstrip())
                continue

            if variant is not None:
                # Blanks end nothing, and neither do comments: gui.rpy has a
                # commented-out `gui.text_size` at column 0 *inside* the
                # variant body, and treating that as a dedent ended the block
                # early and scanned the rest of it after all.
                if not stripped or stripped.startswith("#"):
                    continue
                if len(line) - len(line.lstrip()) <= variant:
                    variant = None                # dedented out of the body
                else:
                    continue

            if stripped.startswith("@gui.variant"):
                variant = "def"
                continue

            # The conditional form is its own block header, so the body is
            # whatever is indented past *this* line -- no `def` to wait for.
            if vpat.match(line):
                variant = len(line) - len(line.lstrip())
                continue

            m = pat.match(line)
            if m and m.group(1) not in skip:
                sizes.add(int(m.group(2)))

    return sorted(sizes)


def collect_charset(game_dir):
    """Every character the game can display, from its own scripts."""
    chars = set()

    # Always include printable ASCII: the UI and numbers need it even where
    # the scripts happen not to use a particular character.
    for c in range(0x20, 0x7F):
        chars.add(chr(c))

    # Walk the whole tree, not just the top level. The translations live in
    # tl/<language>/, and a language needs glyphs the original never used:
    # scanning only game/*.rpy silently left "tori" spelled with a macron in
    # the English script, and the arrow symbols in common.rpy, with no glyph
    # baked for them. DejaVu has all three -- they were simply never asked
    # for. .rpym is included because tl/None/ uses it for the original
    # language's strings.
    paths = []
    for root, _dirs, names in os.walk(game_dir):
        for name in names:
            if name.endswith(".rpy") or name.endswith(".rpym"):
                paths.append(os.path.join(root, name))

    for path in sorted(paths):
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()
        # Only the contents of quoted strings can reach the screen.
        for m in re.finditer(r'"([^"\\]*(?:\\.[^"\\]*)*)"', text):
            chars.update(m.group(1))
        for m in re.finditer(r"'([^'\\]*(?:\\.[^'\\]*)*)'", text):
            chars.update(m.group(1))

    # Drop control characters and anything the renderer handles itself.
    chars = {c for c in chars if ord(c) >= 0x20 and ord(c) != 0x7F}
    chars.add("…")   # ellipsis, used in the GUI
    chars.add("—")   # em dash
    chars.add("’")   # right single quote
    return sorted(chars, key=ord)


# Codepoints in unassigned planes, used to fingerprint a face's notdef box.
# They have to be genuinely unassigned, not merely obscure: the first version
# of this probed U+E000 on the theory that no text font defines private use,
# and Pecita defines a whole run of PUA ligature glyphs. The probe quietly
# reported "no notdef", the skip did nothing, and every Hebrew and Thai
# codepoint in the charset was baked as a box.
NOTDEF_PROBES = ["\U0002ffff", "\U000effff", "\U0010fffd"]


def notdef_mask(font):
    """(size, bytes) of this face's missing-glyph box, or None.

    PIL will happily rasterise a codepoint the face does not have: FreeType
    substitutes glyph 0, the notdef box, and nothing in the API says so. So
    several unassigned codepoints are rendered and compared; if they all come
    out identical that is the notdef, and any glyph matching it can be left
    out of the atlas for the runtime to find in the fallback face instead.

    If they differ the face is doing something unusual and the probe reports
    None, which bakes everything. Erring that way costs atlas bytes; erring
    the other way would drop glyphs the face really has.
    """
    try:
        masks = [font.getmask(c, mode="L") for c in NOTDEF_PROBES]
    except Exception:                                   # noqa: BLE001
        return None

    first = (masks[0].size, bytes(masks[0]))
    for m in masks[1:]:
        if (m.size, bytes(m)) != first:
            return None
    if not first[0][0] or not first[0][1]:
        return None                 # a blank notdef is indistinguishable
    return first


def bake(size, chars, ranges, out_path, skip_uncovered=False):
    """One atlas.

    `ranges` is the routing table, a list of (lo, hi, paths) where `paths` has
    one font file per style index. A codepoint is baked from the first range
    that contains it, and one not in any range is left out -- that is how a
    named face carries only the block it covers.
    """
    faces = [[ImageFont.truetype(p, size) for p in paths]
             for _lo, _hi, paths in ranges]

    ascent, descent = faces[0][0].getmetrics()
    line_height = ascent + descent

    notdefs = [[notdef_mask(f) if skip_uncovered else None for f in row]
               for row in faces]
    uncovered = 0

    glyphs = []
    for ch in chars:
        ri = -1
        for i, (lo, hi, _paths) in enumerate(ranges):
            if lo <= ord(ch) <= hi:
                ri = i
                break
        if ri < 0:
            continue                 # outside every range; the fallback has it

        paths = ranges[ri][2]

        for style in range(NSTYLES):
            # A variant that resolves to the same file as the regular face
            # adds nothing; the runtime falls back to style 0.
            if style and paths[style] == paths[0]:
                continue

            font = faces[ri][style]
            try:
                mask = font.getmask(ch, mode="L")
                bbox = font.getbbox(ch)
                advance = font.getlength(ch)
            except Exception as exc:                   # noqa: BLE001
                print("  skipping %r: %s" % (ch, exc))
                continue

            w, h = mask.size
            if bbox is None:
                bbox = (0, 0, 0, 0)

            data = bytes(mask) if w and h else b""

            nd = notdefs[ri][style]
            if nd and nd[0] == mask.size and nd[1] == data:
                uncovered += 1
                continue             # the face has no such glyph
            glyphs.append({
                "key": (ord(ch) << 2) | style,
                "w": w,
                "h": h,
                "ox": int(bbox[0]),
                "oy": int(bbox[1]),
                "adv": int(round(advance)),
                "data": data,
            })

    if not glyphs:
        # font.c rejects a zero-glyph file, and rightly: there is nothing to
        # look up. Report it and leave no file behind, so the runtime falls
        # back to the default face for this size rather than logging a load
        # failure every startup.
        print("  %s: no glyphs, not written"
              % os.path.basename(out_path))
        return False

    # Pack into rows. A simple shelf packer is plenty for a few hundred
    # glyphs and keeps the atlas narrow enough to stay cache friendly.
    atlas_w = 512
    x = y = row_h = 0
    for g in glyphs:
        if x + g["w"] > atlas_w:
            x = 0
            y += row_h + 1
            row_h = 0
        g["x"] = x
        g["y"] = y
        x += g["w"] + 1
        row_h = max(row_h, g["h"])
    atlas_h = y + row_h + 1

    atlas = Image.new("L", (atlas_w, atlas_h), 0)
    for g in glyphs:
        if g["w"] and g["h"]:
            atlas.paste(Image.frombytes("L", (g["w"], g["h"]), g["data"]),
                        (g["x"], g["y"]))

    glyphs.sort(key=lambda g: g["key"])         # runtime binary-searches this

    with open(out_path, "wb") as f:
        f.write(struct.pack("<IIiiiiIII",
                            MAGIC, VERSION,
                            size, ascent, descent, line_height,
                            atlas_w, atlas_h, len(glyphs)))
        for g in glyphs:
            f.write(struct.pack("<Ihhhhhhhh",
                                g["key"],
                                g["x"], g["y"], g["w"], g["h"],
                                g["ox"], g["oy"], g["adv"], 0))
        f.write(atlas.tobytes())

    print("  %s: %d glyphs, atlas %dx%d, %d bytes%s"
          % (os.path.basename(out_path), len(glyphs),
             atlas_w, atlas_h, os.path.getsize(out_path),
             ", %d uncovered" % uncovered if uncovered else ""))
    return True


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)

    game_dir = sys.argv[1]
    out_dir = sys.argv[2]
    sizes = [int(s) for s in sys.argv[3:]]
    if not sizes:
        sizes = collect_sizes(game_dir)
        print("sizes: %s (from the game's gui.*_text_size)"
              % " ".join(str(s) for s in sizes))
    if not sizes:
        raise SystemExit("no gui.*_text_size found in %s, and no sizes given "
                         "on the command line" % game_dir)

    default_name, named = collect_faces(game_dir)
    if not default_name:
        default_name = "DejaVuSans.ttf"
        print("note: no gui.text_font found, defaulting to %s" % default_name)

    latin_paths = face_paths(game_dir, default_name, required=True)

    # NotoSansJP ships one weight here, so all four styles resolve to it.
    #
    # Optional: a game with no Japanese in it has no reason to carry a 5.7MB
    # font, and this used to be a hard exit. The default face then covers only
    # the Latin block and anything above it is dropped with a count, which is
    # visible in the log rather than silent.
    cjk_regular = os.path.join(game_dir, "NotoSansJP-Regular.ttf")
    cjk_paths = None
    if os.path.exists(cjk_regular):
        cjk_paths = []
        for suffix in STYLE_NAMES:
            p = os.path.join(game_dir, "NotoSansJP%s.ttf"
                             % (suffix if suffix else "-Regular"))
            cjk_paths.append(p if os.path.exists(p) else cjk_regular)

    chars = collect_charset(game_dir)
    cjk_count = sum(1 for c in chars if ord(c) >= CJK_START)
    print("charset: %d characters (%d CJK), default face %s"
          % (len(chars), cjk_count, default_name))

    # The default face's routing table, and the one extension point: a
    # language outside both blocks -- Thai below 0x2e80 where DejaVu has
    # nothing, Hangul above it where NotoSansJP has nothing -- needs another
    # entry here and a font file, and nothing else changes.
    default_ranges = [(0x20, CJK_START - 1, latin_paths)]
    if cjk_paths:
        default_ranges.append((CJK_START, 0x10FFFF, cjk_paths))
    elif cjk_count:
        print("note: no NotoSansJP-Regular.ttf; %d character%s at or above "
              "U+%04X will not be drawn"
              % (cjk_count, "" if cjk_count == 1 else "s", CJK_START))

    os.makedirs(out_dir, exist_ok=True)
    for size in sizes:
        bake(size, chars, default_ranges,
             os.path.join(out_dir, "font%d.dat" % size))

    # Named faces. Each is baked at every size the GUI asks for, because which
    # size reaches which face depends on the style graph in screens.rpy --
    # gui.quick_button_text_size is 14 and quick_button_text inherits its font
    # from button_text, so pairing by `gui.<kind>_` prefix alone would miss
    # it. That is affordable only because a named face carries just the block
    # it covers: Pecita at Hinata's seven sizes is a fraction of one default
    # atlas, where duplicating the CJK block per size would not have been.
    faces = []
    for filename, _local in named:
        paths = face_paths(game_dir, filename, required=False)
        if not paths:
            continue
        slug = slug_for(filename)
        ranges = [(0x20, CJK_START - 1, paths)]
        baked = []
        print("face %s (%s):" % (filename, slug))
        for size in sizes:
            out = os.path.join(out_dir, "font-%s%d.dat" % (slug, size))
            if bake(size, chars, ranges, out, skip_uncovered=True):
                baked.append(size)
            elif os.path.exists(out):
                os.remove(out)
        if baked:
            faces.append((filename, slug, baked))

    # Drop atlases for sizes or faces no longer asked for. Without this an
    # earlier bake's font20.dat and font36.dat stayed behind and went into the
    # pak -- unread, because ui.c only loads what the manifest lists, but
    # still carried to the target.
    keep = set("font%d.dat" % s for s in sizes)
    for filename, slug, baked in faces:
        keep.update("font-%s%d.dat" % (slug, s) for s in baked)
    for name in sorted(os.listdir(out_dir)):
        if re.match(r"^font(-[a-z0-9]+)?\d+\.dat$", name) and name not in keep:
            os.remove(os.path.join(out_dir, name))
            print("removed stale %s" % name)

    # What was baked, for the engine to read at startup instead of carrying
    # its own copy of the list.
    #
    # A bare decimal is a default-face size, which is all this file used to
    # hold; ui.c skips any line it cannot parse as one, so an engine built
    # before faces existed reads a new manifest as the old one and simply
    # ignores them. A `face` line carries the filename gui.rpy named, so the
    # engine matches the `font` style property on that string and never needs
    # to know how the slug was spelled.
    manifest = os.path.join(out_dir, "fonts.lst")
    with open(manifest, "w", encoding="ascii") as f:
        f.write("# baked by tools/mkfont.py\n")
        f.write("# <size>, or: face <slug> <size> <font file named by gui.*>\n")
        for size in sizes:
            f.write("%d\n" % size)
        for filename, slug, baked in faces:
            for size in baked:
                f.write("face %s %d %s\n" % (slug, size, filename))
    print("wrote %s" % manifest)


if __name__ == "__main__":
    main()

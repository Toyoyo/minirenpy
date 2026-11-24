#!/usr/bin/env python3
"""Pack the game directory into a flat MRPK archive.

Windows 3.1 uses FAT with 8.3 filenames, so almost none of a Ren'Py game's
files can exist on the target filesystem: `script_declares.rpy`, `miwa school
biwa talk.png` and the `00-renpythm/` directory are all illegal. The archive
stores the full long names internally, so the host filesystem only ever sees
one 8.3 name -- MRPY.PAK by default, and whatever the second argument says.

Layout:

    magic   "MRPK"
    u32     version
    u32     entry count
    u32     directory offset
    directory[count] of:
        char name[NAME_LEN]  lowercased, '/' separated, NUL padded
        u32  offset
        u32  size
    file data, each entry aligned to ALIGN

Nothing is compressed and the directory is a sorted flat array, so the runtime
loads the whole directory once (NAME_LEN + 8 bytes an entry, so 78KB for 304
files) and every lookup is a binary search followed by one seek and read.

Usage:
    python3 tools/mkpak.py <game-dir> <out.pak>
"""

import os
import struct
import sys

MAGIC = b"MRPK"
# v2 widened the name field from 64 to 256. The reader checks the version and
# refuses anything else, because the directory is one fread into a struct
# array -- the entry size is part of the format, so a v1 archive read as v2
# would come out as garbage rather than as short names. Keep in step with
# MR_PATH_MAX in src/mr.h.
VERSION = 2
NAME_LEN = 256

# Sector- and CD-sector-aligned. Costs under 1% on this data set and keeps
# every read aligned on slow media.
ALIGN = 2048

# Directories that hold nothing the engine reads at runtime.
SKIP_DIRS = {"saves", "cache"}

# .rpyc is Ren'Py's own compiled form, which we do not use; we parse the .rpy
# sources. Font files are baked into the atlases at build time and there is no
# rasteriser on the target, so neither .ttf nor .otf is of any use in the pak
# -- .otf was missing here, and Hinata's 864KB Pecita-0owz.otf was shipped
# unread.
SKIP_EXTS = {".rpyc", ".bak", ".ttf", ".otf", ".ico", ".icns"}


def collect(game_dir):
    entries = []
    # followlinks: an archive cannot hold a symlink, so a linked directory
    # has to be walked into and its contents stored. Without this, a
    # directory symlink is silently skipped -- os.walk does not descend one
    # by default and does not report it either.
    #
    # Found the hard way. tools/harness/fixgame links fonts/ and gui/ into
    # tools/harness/common/, and the fixture pak came out with neither. The
    # symptom was nowhere near the cause: no fonts meant every text width
    # measured 0, so the rhythm minigame's give-up dialog sized itself to
    # nothing and rgtest reported that the dialog was not framed and that
    # its buttons could not be clicked.
    #
    # Safe against cycles here because nothing in a game directory links
    # back into itself; if that ever changes this will loop.
    for root, dirs, files in os.walk(game_dir, followlinks=True):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for fn in files:
            ext = os.path.splitext(fn)[1].lower()
            if ext in SKIP_EXTS:
                continue
            full = os.path.join(root, fn)
            rel = os.path.relpath(full, game_dir).replace(os.sep, "/").lower()
            if len(rel.encode("utf-8")) >= NAME_LEN:
                raise SystemExit("name too long for the archive: %s" % rel)
            entries.append((rel, full))

    # Sorted so the runtime can binary search. Sorting by the encoded bytes
    # matches the byte-wise comparison the C side does.
    entries.sort(key=lambda e: e[0].encode("utf-8"))

    # A duplicate would make a binary search ambiguous.
    seen = set()
    for rel, _ in entries:
        if rel in seen:
            raise SystemExit("duplicate name after lowercasing: %s" % rel)
        seen.add(rel)

    return entries


def build(game_dir, out_path):
    entries = collect(game_dir)

    header_size = 4 + 4 + 4 + 4
    dir_size = len(entries) * (NAME_LEN + 4 + 4)

    # Data starts after the header and directory, rounded up to ALIGN.
    data_start = (header_size + dir_size + ALIGN - 1) // ALIGN * ALIGN

    placed = []
    offset = data_start
    for rel, full in entries:
        size = os.path.getsize(full)
        placed.append((rel, full, offset, size))
        offset = (offset + size + ALIGN - 1) // ALIGN * ALIGN

    with open(out_path, "wb") as out:
        out.write(MAGIC)
        out.write(struct.pack("<III", VERSION, len(placed), header_size))

        for rel, _full, off, size in placed:
            name = rel.encode("utf-8")
            out.write(name + b"\0" * (NAME_LEN - len(name)))
            out.write(struct.pack("<II", off, size))

        for rel, full, off, size in placed:
            out.seek(off)
            with open(full, "rb") as f:
                while True:
                    chunk = f.read(1 << 16)
                    if not chunk:
                        break
                    out.write(chunk)

        # Pad the tail so the file length is a whole number of blocks.
        out.seek(0, 2)
        tail = out.tell() % ALIGN
        if tail:
            out.write(b"\0" * (ALIGN - tail))

    total = os.path.getsize(out_path)
    raw = sum(s for _r, _f, _o, s in placed)
    print("%s: %d files, %.1f MB payload, %.1f MB archive (%.1f%% padding)"
          % (os.path.basename(out_path), len(placed),
             raw / 1048576.0, total / 1048576.0,
             (total - raw) * 100.0 / max(total, 1)))
    return placed


def warn_missing(game_dir, placed):
    """Flag images the scripts show that the archive does not contain."""
    names = {r for r, _f, _o, _s in placed}
    have = {os.path.splitext(n[len("images/"):])[0]
            for n in names if n.startswith("images/")}

    if not have:
        print("  warning: no images/ entries in the archive")
        return

    # Cheap check: every `show`/`scene` target should resolve to either an
    # images/ basename or an `image` statement. Only the former is checked
    # here; the engine logs the rest at runtime.
    import re
    referenced = set()
    for fn in os.listdir(game_dir):
        if not fn.endswith(".rpy"):
            continue
        with open(os.path.join(game_dir, fn), encoding="utf-8",
                  errors="replace") as f:
            for line in f:
                m = re.match(r"\s*(?:show|scene)\s+([^\n:#]+)", line)
                if not m:
                    continue
                tail = m.group(1)
                for kw in (" at ", " with ", " behind ", " as ", " onlayer "):
                    if kw in tail:
                        tail = tail.split(kw)[0]
                referenced.add(tail.strip().lower())

    missing = sorted(n for n in referenced if n and n not in have)
    if missing:
        print("  %d shown names not found as images/<name>.png "
              "(may be `image` statements):" % len(missing))
        for n in missing[:12]:
            print("    %s" % n)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    game_dir, out_path = sys.argv[1], sys.argv[2]
    placed = build(game_dir, out_path)
    warn_missing(game_dir, placed)


if __name__ == "__main__":
    main()

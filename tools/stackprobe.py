#!/usr/bin/env python3
"""Every large stack frame in the shipped binary must be preceded by a probe.

A function that allocates its frame with `sub esp,<more than a page>` moves esp
past the stack's guard page without touching it. Win32s commits the whole stack
up front and tolerates that; Windows 9x, NT and 10 grow the stack lazily one
guard page at a time, so the first write into the skipped page faults and the
OS never sees the guard-page hit it needs in order to extend. The engine ran on
Win32s and Wine and died instantly on Win98 and Windows 10.

Open Watcom's -sg emits a call to __GRO, which walks the frame writing one
dword per 4KB page. -ox implies -s, which removes it. Nothing in the build
warns, and no harness can see it: the harnesses are gcc builds on Linux, and
the two environments where it works are the two the game was developed against.

So this checks the only artefact that can answer the question -- mrpy.exe
itself. Run it after any change to CFLAGS.

    tools/stackprobe.py mrpy.exe
"""
import struct
import sys

PAGE = 0x1000

# __GRO's entry, then its page-walking loop. Matched as bytes because the
# linker does not put it in the map.
GRO_ENTRY = bytes.fromhex("5053" "8b44240c" "bbfcffffff")
GRO_LOOP = bytes.fromhex("891c1c" "81eb00100000")


def sections(d):
    pe = struct.unpack("<I", d[0x3C:0x40])[0]
    nsec = struct.unpack("<H", d[pe + 6:pe + 8])[0]
    optsz = struct.unpack("<H", d[pe + 20:pe + 22])[0]
    base = struct.unpack("<I", d[pe + 24 + 28:pe + 24 + 32])[0]
    off = pe + 24 + optsz
    out = []
    for i in range(nsec):
        b = d[off + 40 * i:off + 40 * (i + 1)]
        name = b[0:8].rstrip(b"\0").decode("latin1")
        vsz, va, rsz, ro = struct.unpack("<IIII", b[8:24])
        out.append((name, base + va, ro, min(rsz, vsz) or rsz))
    return out


def main(path):
    d = open(path, "rb").read()
    secs = sections(d)
    code = next((s for s in secs if s[2] and s[0] in ("AUTO", ".text")), None)
    if not code:
        print("no code section found")
        return 2
    _, cva, cro, csz = code

    gro = d.find(GRO_ENTRY)
    if gro < 0 or d.find(GRO_LOOP) < 0:
        print("__GRO is not in this binary at all: no frame is probed.")
        print("Add -sg to CFLAGS (and see the comment there).")
        return 1
    gro_va = cva + (gro - cro)
    print("__GRO at %08x" % gro_va)

    # The backstop for every other mid-instruction match, present and future:
    # a frame larger than the whole stack is not a frame. The bound is read
    # from the header the linker wrote rather than assumed, because mrpy.lnk
    # sets it and may change it. Filtering can only drop candidates that would
    # fault on any Windows for a different reason, so it cannot hide a real
    # missing probe.
    pe = struct.unpack("<I", d[0x3C:0x40])[0]
    stack = struct.unpack("<I", d[pe + 24 + 72:pe + 24 + 76])[0]
    print("stack reserve %#x; ignoring any `sub esp` above it" % stack)

    body = d[cro:cro + csz]
    big, unprobed = 0, []

    i = 0
    while i + 6 <= len(body):
        # This is a linear byte scan, not a disassembler, so it can start
        # reading in the middle of an instruction and find `81 EC` inside
        # something else. One source of that is worth handling, because it
        # happens: a `call rel32` whose displacement contains those two bytes.
        # `e8 81 ec fe ff` read one byte late is "sub esp,0x248afffe", a
        # 613MB frame, reported as unprobed -- which is what this printed
        # after an unrelated edit to interp.c moved the code along.
        #
        # Skipping the whole call resynchronises the scan, and never hides a
        # real frame: the aligned case is `call __GRO` followed by the `sub`,
        # and skipping the call lands exactly on it.
        if body[i] == 0xE8 or body[i] == 0xE9:
            i += 5
            continue

        # The second offender, and the same shape of accident: a *displacement*
        # rather than a branch target. `c7 81 ec 9d 72 00 01 00 00 00` is
        # "mov dword [ecx+0x729dec],1" -- a write to a large array in DGROUP --
        # and one byte late it reads as "sub esp,0x100729d", a 16MB frame.
        # That is what this printed when blendmmx's globals moved DGROUP along.
        #
        # Skip only the mod=10 form (modrm 0x80..0xBF), which is the one
        # carrying a disp32 big enough to contain 81 EC; its length is
        # 2 + 4 + 4. The mod=00/01 forms have no displacement to hide in.
        # As with the call, skipping it cannot hide a frame: a real `sub esp`
        # is not inside a mov.
        if body[i] == 0xC7 and 0x80 <= body[i + 1] <= 0xBF:
            i += 10
            continue

        # sub esp,imm32 == 81 EC imm32
        if body[i] == 0x81 and body[i + 1] == 0xEC:
            size = struct.unpack("<I", body[i + 2:i + 6])[0]
            if size > PAGE and size <= stack:
                big += 1
                va = cva + i
                # Look back a short way for `call rel32` landing on __GRO.
                found = False
                for back in range(5, 40):
                    j = i - back
                    if j < 0:
                        break
                    if body[j] == 0xE8:
                        rel = struct.unpack("<i", body[j + 1:j + 5])[0]
                        if cva + j + 5 + rel == gro_va:
                            found = True
                            break
                if not found:
                    unprobed.append((va, size))
            i += 6
            continue
        i += 1

    print("%d frames larger than one page; %d without a __GRO probe"
          % (big, len(unprobed)))
    for va, size in unprobed[:20]:
        print("  %08x  sub esp,%#x  (%d bytes, %d pages)"
              % (va, size, size, (size + PAGE - 1) // PAGE))
    return 1 if unprobed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "mrpy.exe"))

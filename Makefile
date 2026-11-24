# Open Watcom 2.0 makefile for minirenpy
#
# Builds a Win32s executable that runs on Windows 3.1x with Win32s 1.30, and
# equally on any later 32-bit Windows. Build on Linux with the Linux-hosted
# Open Watcom tools:
#
#     export WATCOM=/home/fosco/bin/WATCOM
#     export PATH=$WATCOM/binl:$PATH
#     export INCLUDE=$WATCOM/h:$WATCOM/h/nt
#     wmake
#
# Note the Linux-hosted wcc386 emits .o rather than .obj, which is why the
# object list below uses .o.
#
# -6r is the calling convention (register calling, trailing-underscore
# symbols). zlib and libpng are built from source by this makefile to match,
# so changing it here rebuilds them rather than producing a link failure:
#
#     wmake          engine, plus any of the three libraries that is missing
#     wmake libs     just the libraries (zlib, libpng, xvidcore)
#     wmake libclean throw the libraries away, including the two files this
#                    makefile copies into their trees
#     wmake clean    engine objects and outputs; leaves the libraries alone
#
# To update zlib or libpng, drop the new release in libs/, point ZLIB or PNG
# at it, and `wmake libclean libs`. See the notes above the `libs` target for
# the two things that have to be kept outside the library trees for that to
# work.

ZLIB = libs/zlib-1.4
PNG  = libs/libpng-1.6.59
XVID = libs/xvidcore-1.3.7
LIBS = libs

ZLIBLIB = $(ZLIB)/zlib_f.lib
PNGLIB  = $(PNG)/libpng.lib
XVIDLIB = $(XVID)/build/win32/xvidcore.lib

CC     = wcc386
LINK   = wlink
RC     = wrc

# -bt=nt   target the Win32 API
# -6r      Pentium Pro scheduling, register calling convention
# -fp5     Pentium FPU code; the target is a Pentium III
# -otexan  full speed optimisation
# -sg      grow the stack: see below, this one is not optional
# -fpi87   inline 80x87, no emulator fallback. Note this does not *enable*
#          anything: -fpi, the default, already emits identical inline x87
#          (fld/fmul/fdivrp) and -fp5 already schedules it for Pentium, so
#          minimp3's float paths were always using the real FPU. All it drops
#          is the reference to __init_387_emulator, which under -bt=nt is a
#          one-byte `ret` stub. Kept because it states the requirement -- the
#          target is a Pentium III and there is no emulation to fall back on
#          either way.
# -zq      quiet
# -w4 -e25 warnings on, stop after 25 errors
# -d1      line-number debug info, harmless in the shipped binary
#
# -sg is why the engine runs on anything other than Win32s.
#
# -ox implies -s, which *removes stack overflow checks*, so -otexan emitted no
# stack probe at all. A function that allocates its frame with `sub esp,1d60H`
# -- 7520 bytes, nearly two pages -- then moves esp past the guard page
# without touching it. Win32s commits the whole stack up front and does not
# care; Windows 9x, NT and 10 grow the stack *lazily*, one guard page at a
# time, so the first write into the skipped page is an access violation and
# the OS never sees the guard-page hit it needs in order to extend. Win98
# reported it at the `call` that pushes a return address, the first stack
# write after the sub; Windows 10 just closed the window.
#
# -sg emits a call to __GRO, which walks down the frame writing one dword per
# 4KB page so every page is touched in order. It appears only in functions
# whose frame exceeds a page -- 3000 bytes gets nothing, 7000 gets the call --
# so the cost is confined to the handful that need it. -s / default -sc give
# __CHK instead, which only compares against the thread's stack limit and is
# not the same thing at all.
# -DMR_YUV_MMX turns on the hand-written MMX colour conversion in
# src/yuvmmx.c, which is the inner loop of video playback. The Pentium III
# baseline has MMX, so it is on for both Watcom targets and never defined for
# the gcc harness build -- that one could not compile Watcom inline assembly,
# and a kernel the suite appears to cover but does not is worse than one it
# plainly does not. mrpy.ini's `simd=0` turns it off at run time; see yuvmmx.h
# for how it is checked, and probe/mmxcheck.c for the part only the target can
# answer.
# -DMR_BLEND_MMX is the same arrangement one layer down, for the MMX alpha
# blend in src/blendmmx.c -- the textbox panel's 148,000 partial-alpha pixels
# were 126.8 ns/px and 18.8ms on every full frame, the last compositor loop
# left that was compute-bound rather than memory-bound. Same `simd` key, read
# at startup in main.c rather than at movie-open time because this kernel runs
# from the first frame. src/blendmmx.h for the lane arithmetic,
# probe/blendchk.c for the transcription.
CFLAGS = -bt=nt -6r -fp5 -otexan -sg -zq -w4 -e25 -d1 -mf -fpi87 &
         -DMR_YUV_MMX -DMR_BLEND_MMX &
         -i=$(PNG) -i=$(ZLIB) -i=$(LIBS) -i=$(XVID)/src

# src/platw32.o, src/present.o and src/wavew32.o are the platform half;
# everything else in this list is portable C89. See src/plat.h. Makefile.dos
# is the second target and it does exactly that: the same list with those
# three swapped for platdos/presdos/wavedos, plus the files DOS needs and
# Windows supplies (VBE mode setting, DMA conversion, low memory, a drawn
# mouse pointer).
OBJS = src/main.o &
       src/platw32.o &
       src/input.o &
       src/gfx.o &
       src/present.o &
       src/scale.o &
       src/pack.o &
       src/png.o &
       src/image.o &
       src/lex.o &
       src/parse.o &
       src/expr.o &
       src/scene.o &
       src/interp.o &
       src/audio.o &
       src/wavew32.o &
       src/font.o &
       src/ui.o &
       src/bandcache.o &
       src/vfs.o &
       src/mp3impl.o &
       src/plmimpl.o &
       src/avi.o &
       src/xviddec.o &
       src/viddec.o &
       src/yuv.o &
       src/yuvmmx.o &
       src/blendmmx.o &
       src/cpu.o &
       src/movie.o &
       src/slang.o &
       src/slparse.o &
       src/style.o &
       src/slrender.o &
       src/save.o &
       src/history.o &
       src/serial.o &
       src/datefmt.o &
       src/stmtid.o &
       src/tl.o &
       src/md5.o &
       src/persist.o &
       src/musicroom.o &
       src/pyfn.o &
       src/rhythm.o &
       src/list.o &
       src/config.o &
       src/ansi.o

# --- what game this builds -------------------------------------------------
#
# Three macros say which game is being built: TARGET, PAK (just below the
# `test` targets) and GAME beside it. Each is set only if it is not set
# already, which is what lets Makefile.hinata and friends be four lines --
# they define theirs and then `!include Makefile`.
#
# Those files used to be whole copies of this one, differing in exactly these
# three values. They drifted, as copies do: all three still said
# `TARGET = miwa.exe` and `PAK = miwa.pak` long after this file moved to
# mrpy.*, so `wmake -f Makefile.azami data` wrote a pak named for a different
# game. One file now, so the next change here reaches all four.
#
# A command-line macro -- `wmake PAK=other.pak` -- still wins over both: in
# wmake those are immutable, so the !ifndef below sees it as defined.
!ifndef TARGET
TARGET = mrpy.exe
!endif

# Every object depends on every header.
#
# Coarse on purpose. The alternative -- real per-file dependencies -- needs a
# generator, and the whole tree compiles in seconds, so the only thing
# precision would buy is a faster rebuild after touching a header, which is
# rare. What it costs is the thing this prevents, and that cost is severe:
#
# `mr_value` carries its string inline (`char s[MR_VSTR_MAX]`, expr.h), so
# changing that constant changes sizeof(mr_value) and every offset in every
# structure holding one. With no header dependency here, `wmake` after such an
# edit recompiled *nothing*: the objects from before the change were linked
# against the ones built after it, each using a different struct layout. The
# result was a binary that started, ran the intro, and took a page fault with
# a null instruction pointer on the first screen that passed values across
# two stale/fresh modules -- the load page. Nothing in the build said a word,
# and no harness could reproduce it, because gcc rebuilds every source on
# every run.
#
# A struct size living in a header is not unusual, so assume the next one will
# be just as quiet.
HDRS = src/audio.h src/config.h src/expr.h src/font.h src/history.h &
       src/image.h src/interp.h src/lex.h src/list.h src/md5.h src/mr.h &
       src/musicroom.h src/persist.h src/present.h src/pyfn.h src/rhythm.h &
       src/save.h src/scale.h src/scene.h src/script.h src/serial.h &
       src/slang.h &
       src/slparse.h src/slrender.h src/stmtid.h src/style.h src/tl.h &
       src/ui.h src/bandcache.h src/blendmmx.h src/cpu.h src/vfs.h &
       src/datefmt.h &
       src/plat.h src/presw32.h src/input.h src/keys.h src/pack.h src/wave.h &
       src/viddec.h src/xviddec.h src/avi.h

all: .SYMBOLIC hdrstamp $(TARGET)

# How the header dependency is actually enforced, and why it is not the
# obvious `$(OBJS): $(HDRS)`.
#
# That line works, but it costs the dependency wmake was supplying for free:
# giving an object an explicit dependent list stops the extension rule
# inferring `src/audio.o: src/audio.c`. The result rebuilds an object when a
# *header* changes and never when its own source does -- `touch src/audio.c;
# wmake` did nothing whatsoever, and a C fix could sit in the tree, pass every
# harness (those build with gcc, which has no such notion), and never reach
# mrpy.exe. Adding `$^*.c` to the list does not help: it is not expanded in a
# dependency list, and that spelling broke both halves. `.AUTODEPEND` on the
# extension rule does not help either -- wmake looks for its recorded
# dependency information in `.obj`, and the Linux-hosted compiler emits `.o`.
#
# So the headers are enforced by invalidation instead: if any of them is newer
# than the stamp, every object is deleted, and the implicit rule -- still
# intact, because no object has an explicit dependent list -- rebuilds the lot
# from the .c files. Coarse, which is fine; the whole tree compiles in
# seconds. `hdrstamp` comes before $(TARGET) in `all` so the deletion happens
# before the link decides what is up to date.
# Not `.hdrstamp`: a target starting with a dot is a path or extension
# specifier to wmake, and the name alone produced eight "E09: Ignoring out of
# place Path Specifier" errors and a dead build.
hdrstamp: $(HDRS)
	rm -f $(OBJS)
	touch hdrstamp

# --- the bundled libraries -------------------------------------------------
#
# zlib and libpng are built from their own sources rather than kept as
# committed .lib files, so updating either is a matter of dropping in a new
# release and running `wmake libs`. Two things have to survive that, and
# neither is kept inside the library tree for exactly that reason:
#
#   libs/Makefile.libpng
#       libpng ships no Watcom makefile. It used to live in the libpng tree,
#       where a new release would have taken it away.
#
#   PNG_API_RULE 2
#       libpng's pnglibconf.h has to say "use Watcom calling conventions"
#       (pnglibconf.dfa:165). The shipped scripts/pnglibconf.h.prebuilt says
#       0, which is plain __cdecl, and a libpng built that way does not link
#       against a -6r engine. So the header is *derived* from the current
#       release's prebuilt with that one value patched, rather than kept as a
#       copy. It was a copy once, and the tree ended up with a 1.6.54 header
#       sitting inside 1.6.58.
#
# zlib's own watcom_f.mak uses -3r where we use -6r. That is only instruction
# scheduling; the `r` is the part that matters and it agrees. Do not "fix" it.

libs: .SYMBOLIC $(ZLIBLIB) $(PNGLIB) $(XVIDLIB)

$(ZLIBLIB):
	cd $(ZLIB) && wmake -h -f watcom/watcom_f.mak

# xvidcore, decode only, with the MMX and MMXEXT assembly in -- which is the
# whole reason it is here. See libs/xvidcore-1.3.7/build/win32/Makefile.watcom
# for what is compiled and what is left out (the encoder, and the SIMD paths a
# Pentium III cannot execute, which are assembled anyway because xvid.c takes
# their addresses unconditionally).
#
# **This one needs nasm**, which the other two do not. That is worth knowing
# before the error: a tree without nasm on the path fails in the middle of
# this rule and not at the start of the build.
#
# The grep is a guard, and it is the same idiom as the PNG_API_RULE check
# below for the same reason: fail loudly here rather than confusingly later.
# An *empty* xvidcore.lib -- 1536 bytes, a valid archive with no modules in it
# -- satisfies this target's existence and then fails the engine link with
# "xvid_global_ is an undefined reference", which reads as a code fault in
# src/xviddec.c and is not one. That happened once, from a DOS build that
# cleaned this directory, and cost a round of investigation. If the library
# cannot offer its own entry point, that is a build failure now.
#
# `-a` is load-bearing. Without it grep treats a .lib as binary, declines to
# report a match through `-q`, and the guard rejects a *good* library -- which
# is the failure mode a guard must not have. Verified both ways against a real
# library and an empty one.
$(XVIDLIB):
	cd $(XVID)/build/win32 && wmake -h -f Makefile.watcom
	grep -aq xvid_global $(XVIDLIB)

$(PNG)/Makefile: $(LIBS)/Makefile.libpng
	cp $(LIBS)/Makefile.libpng $@

# The sed expression deliberately does not mention `define`: a '#' starts a
# comment in wmake, so spelling out the whole directive silently truncates the
# command and sed dies on an unterminated expression. The grep is a guard --
# if a future libpng stops matching, this must fail loudly here rather than
# produce a __cdecl library that fails to link much later.
$(PNG)/pnglibconf.h: $(PNG)/scripts/pnglibconf.h.prebuilt
	sed -e "s/PNG_API_RULE 0/PNG_API_RULE 2/" $(PNG)/scripts/pnglibconf.h.prebuilt > $@
	grep -q "PNG_API_RULE 2" $@

$(PNGLIB): $(PNG)/Makefile $(PNG)/pnglibconf.h $(ZLIBLIB)
	cd $(PNG) && wmake -h

# Throw both libraries away, including the two files copied into their trees,
# so the next build reconstructs them. Kept out of `clean`: rebuilding them
# takes far longer than the engine and is not what `clean` is usually for.
libclean: .SYMBOLIC
	cd $(ZLIB) && wmake -h -f watcom/watcom_f.mak clean
	rm -f $(ZLIBLIB)
	rm -f $(PNG)/*.o $(PNG)/*.err $(PNGLIB)
	rm -f $(PNG)/Makefile $(PNG)/pnglibconf.h
	cd $(XVID)/build/win32 && wmake -h -f Makefile.watcom clean
	rm -rf $(XVID)/build/dos $(XVID)/build/dossimd $(XVID)/build/host

# The version block and the icon. Bound into the exe after the link rather
# than handed to the linker: wrc is the tool that knows the .ico format, and
# binding is what it does to a finished PE.
mrpy.res: mrpy.rc rsc/icon.ico
	$(RC) -q -r -bt=nt -fo=$^@ mrpy.rc

# mrpy.lnk defines `system mrpy`: OpenWatcom's own win32s system names twenty
# default import libraries and four of them are not shipped, which made every
# link print four W1008 warnings about files nothing references.
# The libraries come first in the dependency list, not last: src/png.c
# includes png.h, which includes pnglibconf.h, so the header has to be
# derived before any engine object is compiled.
$(TARGET): $(ZLIBLIB) $(PNGLIB) $(XVIDLIB) $(OBJS) mrpy.lnk mrpy.res
	$(LINK) @mrpy.lnk system mrpy name $@ &
		file {$(OBJS)} &
		library $(PNGLIB) &
		library $(ZLIBLIB) &
		library $(XVIDLIB) &
		option map=mrpy.map, eliminate, quiet
	$(RC) -q -bt=nt mrpy.res $@

.c: src

# The source is named from the target ($^* is the target without its
# extension), not taken from the dependent list. $< is "all dependents" and
# $[@ did not resolve to the .c either, and since the objects now also depend
# on $(HDRS), both handed the header list to the compiler: "E1139: Command
# line contains more than one file to compile".
#
# This rule must keep its inferred `src/x.o: src/x.c` dependency, which is
# why the headers are handled by the hdrstamp target above rather than by
# giving $(OBJS) a dependent list. .AUTODEPEND was tried here and does
# nothing: wmake reads Watcom's recorded dependencies out of a `.obj`, and the
# Linux-hosted compiler emits `.o`.
.c.o:
	$(CC) $(CFLAGS) -fo=$^@ $^*.c

# minimp3 is C99 (declarations after statements), so its one translation unit
# needs -za99. Everything else stays C89.
src/mp3impl.o: src/mp3impl.c
	$(CC) $(CFLAGS) -za99 -w1 -fo=$^@ $^*.c

# pl_mpeg is C99 too, and more so -- `//` comments throughout, declarations in
# `for`. Same treatment, and for the same reason: one translation unit sees the
# vendor header, and the engine's own files stay C89 so that the Watcom build
# and the gcc harness build agree about what is legal.
#
# This is why src/plmdec.h exists. movie.c -- where every decision about
# timing and frame dropping is made -- must be readable under the C89 rule,
# and it cannot be if it includes pl_mpeg.h. plmimpl.c is the only file that
# does, and it hands the rest of the engine three byte planes and 16-bit PCM.
#
# -w1 because it is somebody else's code and -w4 on it is noise. Note that it
# needs *no* source modification: see the header of plmimpl.c, which explains
# the two includes that have to precede it and why they are not patched in.
src/plmimpl.o: src/plmimpl.c
	$(CC) $(CFLAGS) -za99 -w1 -fo=$^@ $^*.c

# Host-side parser test. Builds with gcc, not Watcom, and runs the real lexer
# and parser over the game's scripts so parse regressions show up here rather
# than on a Windows 3.1 machine.
test: .SYMBOLIC
	gcc -O1 -g -Wall -I src -I tools/hostshim -o tools/dumpscript &
		tools/dumpscript.c src/lex.c src/parse.c src/vfs.c src/expr.c &
		src/style.c src/slang.c src/slparse.c src/tl.c src/md5.c
	./tools/dumpscript "$(GAME)"

# Host-side screen language check: parses every screen in a .rpy and reports
# any construct the tables do not know about.
screens: .SYMBOLIC
	gcc -O1 -g -Wall -I src -I tools/hostshim -o tools/dumpscreens &
		tools/dumpscreens.c src/slang.c src/slparse.c src/lex.c src/vfs.c
	./tools/dumpscreens "$(GAME)" screens.rpy

# Host-side audio check: runs the real audio.c against a fake waveOut and
# writes what it would have played, for comparison with ffmpeg.
audiotest: .SYMBOLIC
	gcc -O1 -g -I src -I tools/hostshim -I libs -o tools/audiotest &
		tools/audiotest.c src/audio.c src/vfs.c src/mp3impl.c -lm
	./tools/audiotest "$(GAME)" audio/biwa-no-nagekigoe.mp3 /tmp/a.wav 4

# The game's directory. See the TARGET note above: set only if a wrapper
# makefile or the command line has not said otherwise.
!ifndef GAME
GAME = game
!endif

# Data build. The font atlases live inside the game directory, so they have to
# be baked before the archive is packed or they will not be in it.
!ifndef PAK
PAK = mrpy.pak
!endif

# Sizes are no longer listed here. mkfont.py takes them from the game's own
# `gui.*_text_size` defines and writes fonts/fonts.lst alongside the atlases,
# which is what ui.c reads -- so one script edit is enough and there is no
# list in the Makefile or in ui.c to keep in step. Pass sizes as extra
# arguments to override.
#
# Baking only the two the dialogue box uses would leave the engine with no
# body font at all, and mr_ui_init then bails before registering the screen
# drawer -- a wholly black window.
fonts: .SYMBOLIC
	python3 tools/mkfont.py "$(GAME)" "$(GAME)/fonts"

pak: .SYMBOLIC
	python3 tools/mkpak.py "$(GAME)" $(PAK)

# Stand-ins for `renpy.movie_cutscene()`: the video's first frame and its
# soundtrack, both named after the video so the engine can re-derive them at
# runtime. Quiet no-op for a game with no cutscene, and a warning rather than
# an error when ffmpeg is missing.
cutscenes: .SYMBOLIC
	python3 tools/mkcutscene.py "$(GAME)"

data: .SYMBOLIC fonts cutscenes pak

# Run under Wine for a quick look on the build host. Wine happily loads a
# 3.10-subsystem PE, so this exercises the real code path; only the Win32s
# thunking behaviour differs from the real target.
run: .SYMBOLIC $(TARGET)
	wine $(TARGET)

clean: .SYMBOLIC
	rm -f src/*.o src/*.err $(TARGET) mrpy.map log.txt mrpy.res hdrstamp
	rm -f tools/dumpscript tools/audiotest tools/dumpscreens dump.txt d.txt
	rm -f *.png
	rm -rf dist

# The stack-probe check runs here rather than on every build: it is the
# shipping path, and an unprobed large frame is invisible on Win32s and under
# Wine but fatal on Windows 9x, NT and 10. It exits non-zero, which stops the
# target. See the -sg note above CFLAGS.
dist: .SYMBOLIC $(TARGET)
	python3 tools/stackprobe.py $(TARGET)
	mkdir -p dist
	cp $(TARGET) dist/
	upx --strip-relocs=0 dist/$(TARGET)
	cp $(PAK) dist/
	# The ini's `pak=` has to name the archive that just got copied, and
	# with a pak per game it is not always mrpy.pak. main.c reads the key
	# (mr_config_str("pak", MR_PAK_NAME), main.c:432) and falls back to
	# "mrpy.pak", so a Hinata dist carrying hinata.pak beside an unedited
	# mrpy.ini finds no archive at all and the window comes up empty.
	# Rewritten on the way out rather than kept per game, so there is one
	# ini in the tree and no copy to forget.
	sed -e "s/^pak=.*/pak=$(PAK)/" mrpy.ini > dist/mrpy.ini
	grep -q "^pak=$(PAK)" dist/mrpy.ini

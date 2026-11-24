# GNU make, for the diagnostic probes in this directory.
#
#   make -C probe            build every host probe
#   make -C probe pickrank   just one
#   make -C probe clean
#
# GNU make, and in this directory rather than at the top of the tree,
# because the two makefiles up there are Watcom wmake files -- `Makefile` is
# the Win32 build and `Makefile.dos` the DOS one, and running GNU make on
# either produces a page of syntax errors rather than a useful message.
# `make -C probe` cannot be confused for them.
#
# These are not tests. A harness asserts and goes red; a probe measures, or
# prints, or proves a diagnostic works before it is trusted on hardware.
# They are kept because the questions they answer come back: see NOTES, "Not
# with Jemm", for what sccheck and memwalk were for, and "Three Sound
# Blasters" for sbsnr and sbfile.
#
# Each one's own header says what it is for and how to drive it. What was
# missing until this file existed was how to *build* them: two of them said
# "<parse chain>" and left thirty-three source files to be reconstructed by
# hand.

CC      = gcc
CFLAGS  = -O0 -g -std=c89 -Wall
INCS    = -I../src -I../tools/hostshim -I../libs
LIBS    = -lm

# What it takes to load a script and nothing more.
#
# Not tools/enginesrc.lst, which is the whole portable engine and is what
# run.sh links. Two reasons it is a different list here, and the first one
# is a hard error rather than a preference:
#
#   * interp.c, ui.c and input.c reference mr_stage, mr_invalidate and
#     mr_quit, which live in main.c on the real target. Every harness
#     defines its own three -- that is how it gets to assert on them -- and
#     neither probe does, so linking the full engine fails on fourteen
#     undefined references. Adding stubs to the probes to satisfy modules
#     they never call would be the wrong way round.
#   * A probe that watches for a stray write into the statement array wants
#     the smallest program that can hold a script, not the largest. Less
#     linked in is less that could have done the writing.
#
# Derived by linking and reading what was missing, not by reasoning about
# the include graph. pyfn came last and only the linker knew.
SCRIPT_MODS = vfs config lex parse expr stmtid serial list ansi md5 \
              style slang slparse tl datefmt pyfn
SCRIPT      = $(patsubst %,../src/%.c,$(SCRIPT_MODS))

# plathost.c, for exactly one symbol: mr_plat_watch_write. src/plat.h is the
# seam and the host has no debug registers behind it. No wavehost.c, because
# audio.c is not in the list above, and no -lpng, because image.c and png.c
# are not either.
HOST = ../tools/hostshim/plathost.c

# The host probes, in rough order of how often they have been wanted.
#
# memwalk is NOT here: it is a DOS program, built with Watcom against
# int 21h and DPMI. It does not compile on this host at all and a target that
# pretended otherwise would only produce a confusing error. It is built from
# the tree root, by the DOS makefiles, and gets the extender from whichever
# one is invoked:
#
#   wmake -f Makefile.dos memwalk     memwalk.exe,   DOS/32A
#   wmake -f Makefile.cw  memwalk     memwalkcw.exe, CauseWay
PROBES = pickrank sbsnr sbfile sccheck stmtdump

all: $(PROBES)

# --- probes that need one engine module ------------------------------------

# The mode picker against a real card's mode list.
pickrank: pickrank.c ../src/vbe.c
	$(CC) $(CFLAGS) $(INCS) -o $@ $^ $(LIBS)

# Signal-to-noise of the 8-bit converter against an ideal reference.
sbsnr: sbsnr.c ../src/sbconv.c
	$(CC) $(CFLAGS) $(INCS) -o $@ $^ $(LIBS)

# A raw file through sbconv, exactly as the sink would convert it.
sbfile: sbfile.c ../src/sbconv.c
	$(CC) $(CFLAGS) $(INCS) -o $@ $^ $(LIBS)

# --- probes that load a real script ----------------------------------------
#
# -O0 is not laziness in these two: both exist to catch a write into memory
# that nothing should be writing to, and an optimiser that keeps a copy of
# the thing being watched in a register is the one thing that would make
# them lie.

# Proves mr_script_snapshot/mr_script_verify actually catch a write into the
# statement array, before that pair is trusted on hardware.
sccheck: sccheck.c $(SCRIPT) $(HOST)
	$(CC) $(CFLAGS) $(INCS) -o $@ $^ $(LIBS)

# Prints statements around an index, plus a sweep over every text pointer.
stmtdump: stmtdump.c $(SCRIPT) $(HOST)
	$(CC) $(CFLAGS) $(INCS) -o $@ $^ $(LIBS)

# --- housekeeping ----------------------------------------------------------

# The binaries only. The .py probes are source, memwalk.do is a Watcom
# object from the DOS build and is not ours to remove, and probe/*.c is the
# point of the directory.
clean:
	rm -f $(PROBES)

.PHONY: all clean

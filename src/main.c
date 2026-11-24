/* main.c - bringing the game up, the frame loop, and error reporting.
 *
 * Portable C89. Nothing in here creates a window, reads a message queue or
 * calls a platform clock directly; all of that is behind plat.h, and the
 * process entry point lives on the platform side with it. What is left is
 * the part that is the same on every target: what a frame does, when to skip
 * one, and the order in which the subsystems come up and go down.
 *
 * The file is still called main.c because the frame loop is still the centre
 * of the program -- it just no longer owns the machine.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

#include "mr.h"
#include "plat.h"
#include "present.h"
#include "movie.h"     /* a cutscene draws itself; see frame() */
#include "script.h"
#include "scene.h"
#include "image.h"
#include "interp.h"
#include "audio.h"
#include "ui.h"
#include "vfs.h"
#include "slrender.h"
#include "save.h"
#include "config.h"
#include "expr.h"      /* config.name, for the window caption */
#include "ansi.h"      /* ...which is UTF-8, and the caption is not */
#include "tl.h"
#include "history.h"   /* mr_rollback_trace, for MR_ROLLTRACE */
#include "bandcache.h" /* the composited dialogue strip, kept between frames */
#include "blendmmx.h"  /* the MMX alpha blend switch, read at startup */
#include "cpu.h"       /* ...which now asks the processor first */

/* The default when mrpy.ini says nothing. 8.3: the target filesystem cannot
 * hold a longer name, and neither can whatever an ini names instead. */
#define MR_PAK_NAME "mrpy.pak"

/* Ren'Py's name for the same thing, and in the same place -- beside the exe,
 * not in the game directory. */
#define MR_LOG_FILE "log.txt"

/* The frame rate the loop aims at, when neither the script nor mrpy.ini says
 * otherwise. Transitions are time-based rather than frame-based, so the rate
 * costs smoothness and never pacing: a `move` takes 500ms at any rate, and
 * all the rate decides is how many positions it is drawn at on the way.
 *
 * This was a bare `#define MR_FRAME_MS 33` -- 30fps, chosen for the target and
 * applied to every machine. The flaw in that is the direction a cap works in:
 * a cap only ever binds a machine that was fast enough to exceed it. The PIII
 * is nowhere near 30fps during a transition (a full 1280x720 composite is
 * about a third of a second), so the number did nothing there at all, and the
 * only machines it did anything to were the ones that could have been smooth.
 * Reported as "even on a very fast machine a long `move` is a bit choppy",
 * which is exactly what 15 steps across most of a 1280-pixel screen looks
 * like.
 *
 * Ren'Py's own figure is `config.framerate = 100` (config.py:250), and it is
 * a config variable, so it is read from the script with this as the fallback
 * -- the rule about not hardcoding what the script can declare. `fps` in
 * mrpy.ini overrides both, because how much of its own time a machine spends
 * drawing is the person at that machine's business, not the script's.
 *
 * 60 and not 30, and the measurement is what makes that cheap rather than a
 * guess: two MR_PROF runs on the PIII at 30 and at 60 moved no phase cost at
 * all -- a transition frame is ~165ms either way -- because nothing on that
 * machine is near either interval except the dialogue band. Nothing here is
 * frame-driven either, so the extra frames cannot make anything run slower:
 * the typewriter runs off text_cps and a transition off elapsed milliseconds.
 * What 60 costs the target is *headroom*, in the one case it had any: the
 * typing frame is 28ms, so it idled 5ms of every 33 at 30fps and idles none at
 * 60, buying about five more typewriter frames a second for 25% more frames
 * drawn. The figures are in NOTES.md under "The frame rate was a cap".
 *
 * So the default follows the machines rather than the target: nearly every
 * machine this will run on is fast enough for 60 to be the difference between
 * a smooth `move` and a choppy one, and the PIII is unaffected in every phase
 * but one. `fps=30` in mrpy.ini is the way back for a machine that wants its
 * slack, which is also the setting to try first if a slow machine's audio
 * breaks up -- the mixer is refilled from this loop.
 *
 * Not Ren'Py's 100: past 60 the rate is buying frames no display of the era
 * shows, and on anything slow it is only spending the same headroom faster.
 * A script that genuinely wants 100 can say `config.framerate = 100` and be
 * obeyed. The value chosen is logged, so a run can be asked which it used. */
#define MR_FPS_DEFAULT 60

/* Interval between frames, in milliseconds: 1000 / the rate. Held rather than
 * recomputed because 1000/fps is a division by a variable, and a per-frame
 * division is the thing src/mr.h exists to avoid. */
static unsigned int g_frame_ms = 1000 / MR_FPS_DEFAULT;

static mr_surface *g_stage;
static int         g_running = 1;
static int         g_dirty = 1;
static long        g_frames_drawn;
static int         g_stats_frames;

/* MR_PROF=1: where a drawn frame's time goes, totalled over 30 drawn frames
 * and then logged. The platform clock resolves to a millisecond, which is too
 * coarse for one phase of one frame but averages out over thirty of them --
 * and on the target the phases are tens of milliseconds each, not fractions.
 *
 * The point of measuring on the target rather than on a host is the handover
 * to the display: on Win32s SetDIBitsToDevice runs through a 16-bit thunk
 * into the display driver, and nothing about a host build predicts what that
 * costs. */
static int  g_prof;
static long g_prof_update, g_prof_scene, g_prof_ui, g_prof_present;
static long g_prof_drawn, g_prof_skipped;

/* Averaging every drawn frame together hid the thing worth knowing: a
 * transition frame composites the scene twice and blends it, a typing frame
 * composites it once, and a frame that loads an image is neither. Kept apart,
 * because they want different fixes. */
static long g_prof_trans_n, g_prof_trans_ms;
static long g_prof_still_n, g_prof_still_ms;
static long g_prof_band_n, g_prof_band_ms;
static long g_prof_worst;

/* MR_REDRAWPROBE=1: how much of the full-stage repainting is avoidable.
 *
 * A full stage repaint costs 119.5ms on the target against 29ms for a
 * dialogue-only frame on the same content (NOTES.md, "How much of the
 * full-stage repainting is avoidable"). The narrow question is whether the
 * pixels outside the dialogue band actually differ from the ones already on
 * the stage, and the only way to answer it is to hash them.
 *
 * **The comparison is unfiltered, and that is the whole design.** If the stage
 * outside the band matches the previous frame, the repaint produced nothing
 * new -- which is true whatever asked for it and whatever was animating at the
 * time. Everything else is reported as a *breakdown of the wasted frames*
 * rather than as a precondition for counting them.
 *
 * Two earlier versions filtered first and measured second, and each cost a
 * 40-second session on the target to return nothing. The first counted only
 * frames `g_stepped` forced, and found 1 in 226: in skip mode a say converts
 * straight to ST_RUN (interp.c:3449), which answers ALL several tests above
 * the ST_SAY branch that filter was built on. The second added "and nothing is
 * animating", and found 0 in 171. Neither failure said anything about pixels;
 * both measured the filter. The lesson is general enough to be worth the
 * comment: **a filter assembled from the suspected cause cannot discover that
 * the cause is something else**, and a probe that can come back empty has a
 * failure mode indistinguishable from the answer being zero.
 *
 * So this version reports every number it can compute from one session, and
 * the denominators are chosen so that none of them can be empty while frames
 * are being drawn at all.
 *
 * It measures and changes nothing. A high count says a narrower test is worth
 * designing; a low one kills the idea for the price of a log line.
 *
 * Two things it cannot tell you, which the reported line repeats so they
 * travel with the number. The timings in a probe run are *unusable*: the
 * digest reads ~685,000 pixels a drawn frame and is not in any phase timer, so
 * it inflates the gap between `frame` and the per-kind averages -- measured at
 * 126-140ms a still frame without it and 152-256ms with it. And an unchanged
 * region is not on its own permission to skip the repaint -- the focus list is
 * rebuilt during rendering (scene.h), so what the probe prices is the
 * opportunity, not the change. */
static int      g_rprobe;
static long     g_rp_drawn;    /* drawn frames the probe saw at all */
static long     g_rp_full;     /* ... of those, MR_REDRAW_ALL ones with a
                                * dialogue rect to define "outside" with */
static long     g_rp_all;      /* ... of those, ones whose digest was
                                * comparable against the previous frame's.
                                * Reported separately from g_rp_full so that a
                                * zero result can be told apart from a probe
                                * that compared nothing -- the ambiguity that
                                * wasted the third session. */
static long     g_rp_same;     /* ... of those, identical outside the band:
                                * THE MEASUREMENT. No other filter. */

/* Why each identical-outside-the-band frame was asked for, and what stood in
 * the way of its having been a band frame. Not exclusive -- a frame can be
 * both dirtied and animating -- so these are four independent tallies over
 * g_rp_same and not a partition of it. Printed as counts rather than
 * percentages for that reason. */
static long     g_rp_s_dirty;  /* the platform had dirtied the window */
static long     g_rp_s_moving; /* mr_scene_animating said something moves */
static long     g_rp_s_trans;  /* a transition was running */
static long     g_rp_s_step;   /* nostep would have said DIALOGUE: the subset
                                * that dropping g_stepped alone would reach */
static long     g_rp_s_clear;  /* none of the four above: directly actionable,
                                * and the number that decides whether any of
                                * this is worth designing for */

static mr_u32   g_rp_prev;     /* last drawn frame's digest */
static int      g_rp_have;     /* g_rp_prev is from the immediately previous
                                * drawn frame, over g_rp_y/g_rp_h */
static int      g_rp_y, g_rp_h;

/* The compositor buckets (mr.h), accumulated separately for the two phases
 * that do the compositing. `scene` is a few large blits and `ui` is hundreds
 * of small ones, so the per-pixel costs that come out of them are different
 * populations and totalling them answers nothing -- which is the whole reason
 * the phase figures alone were not enough.
 *
 * Read between the phases rather than at the end of the frame, because
 * mr_gfx_profile zeroes as it reads and that is exactly the split wanted. */
static mr_gfx_prof g_prof_gscene, g_prof_gui;

/* Fold one read into a running total. The buckets are a flat struct of longs
 * rather than an array, so this is written out once here instead of at the
 * two call sites. */
static void gprof_add(mr_gfx_prof *acc, const mr_gfx_prof *d)
{
    acc->copy_ms    += d->copy_ms;
    acc->copy_px    += d->copy_px;
    acc->copy_n     += d->copy_n;
    acc->blend_ms   += d->blend_ms;
    acc->blend_px   += d->blend_px;
    acc->blend_n    += d->blend_n;
    acc->fill_ms    += d->fill_ms;
    acc->fill_px    += d->fill_px;
    acc->fill_n     += d->fill_n;
    acc->lerp_ms    += d->lerp_ms;
    acc->lerp_px    += d->lerp_px;
    acc->lerp_n     += d->lerp_n;
    acc->stretch_ms += d->stretch_ms;
    acc->stretch_px += d->stretch_px;
    acc->stretch_n  += d->stretch_n;
}

static mr_script  *g_script;
static unsigned int g_start_ms;

/* One session's worth, like Ren'Py's own log.txt: the file is truncated by
 * the first line written and appended to thereafter, so it cannot grow
 * without bound across runs and what is in it always describes the run being
 * asked about.
 *
 * Done on the first write rather than at startup because mr_log is reachable
 * from anything the startup calls, and a truncate that happens after the
 * first interesting line would throw away the part worth reading.
 *
 * The timestamp is <time.h> rather than the platform's own clock call: every
 * target has a C library, and datefmt.c already depends on localtime. */
void mr_log(const char *fmt, ...)
{
    static int started;
    va_list ap;
    FILE *f;

    f = fopen(MR_LOG_FILE, started ? "a" : "w");
    if (!f)
        return;
    if (!started) {
        time_t     now = time(NULL);
        struct tm *lt = localtime(&now);
        started = 1;
        if (lt)
            fprintf(f, "%s %04d-%02d-%02d %02d:%02d:%02d\n", MR_TITLE,
                    lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday,
                    lt->tm_hour, lt->tm_min, lt->tm_sec);
        else
            fprintf(f, "%s\n", MR_TITLE);
    }
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

void mr_fatal(const char *fmt, ...)
{
    va_list ap;
    char buf[512];

    va_start(ap, fmt);
    vsprintf(buf, fmt, ap);
    va_end(ap);

    mr_log("FATAL: %s", buf);

    /* Give back the two things we take from the system that an abrupt exit
     * does not reliably reclaim -- Windows 3.1 is not NT -- before the
     * platform puts the message up and tears down whatever it owns. The wave
     * device is the worse of the two to leak: it locks the sound driver
     * against every other application until reboot.
     *
     * Both shutdowns are safe before their own init, which matters because
     * this can fire from inside mr_present_init itself. */
    mr_audio_shutdown();
    mr_present_shutdown();

    mr_plat_fatal(buf);
    exit(1);
}

mr_surface *mr_stage(void)
{
    return g_stage;
}

void mr_invalidate(void)
{
    g_dirty = 1;
}

void mr_quit(void)
{
    g_running = 0;
}

/* Follow `_preferences.display`, exactly as mr_tl_sync follows the language:
 * the radio buttons write the preference and a restore can change it, so it
 * is read once a frame rather than hooked at every place that writes it. A
 * string compare per frame is nothing beside compositing the stage.
 *
 * Being a poll is also what applies a *persisted* preference: a player who
 * quit in fullscreen has the variable restored before the first frame, and
 * this turns it into a display change without anyone having clicked. */
static void display_sync(void)
{
    mr_value v;
    int want = mr_var_get("_preferences.display", &v) &&
               v.type == MR_V_STR && !strcmp(v.s, "fullscreen");

    /* Unconditionally, every frame: mr_plat_set_fullscreen is defined to
     * ignore a call that asks for the state it is already in. Remembering
     * the last value here as well would be a second source of truth that
     * could disagree with the platform's -- and the platform's is the one
     * that is right, because it is the one that can decline. */
    mr_plat_set_fullscreen(want);
}

/* One phase's compositor buckets, as totals over the thirty frames rather
 * than per-frame averages: the ratio between the buckets is the thing being
 * asked about, and dividing a 1ms-resolution clock by thirty throws away
 * precision for a number nobody reads.
 *
 * `kpx` is thousands of pixels, which makes the useful quotient trivial to do
 * by eye -- ms divided by kpx is nanoseconds per pixel -- and keeps every
 * product inside the 32-bit long the target has. A full stage is 922kpx, so
 * thirty frames of compositing is tens of thousands and nowhere near the
 * limit.
 *
 * `n` is there to say whether the ms can be trusted at all. The clock is read
 * twice per call, so a bucket with thousands of calls in thirty frames is
 * partly measuring the measurement; one with tens is not. Printed rather than
 * corrected for, because the correction would be a guess at what a
 * timeGetTime costs on a machine we are trying to characterise. */
/* Nanoseconds per pixel, which is the only figure from these lines that can be
 * compared across two batches -- the totals cannot, because the pixel counts
 * differ with the content. It used to be left for the reader to work out, and
 * the banner's description of how ("ms/kpx is ns per pixel") was wrong by a
 * factor of a thousand. Every per-pixel comparison in NOTES.md's compositor
 * sections is one of these, computed by hand; one of them was computed wrongly.
 *
 * Derived from the *printed* kpx rather than from the raw pixel count, so that
 * the arithmetic can be checked against the rest of the line. That also means
 * a bucket under a thousand pixels reports `0kpx 0ns`, which is consistent
 * rather than misleading.
 *
 * ms*1000/kpx and not ms*1000000/px: ms reaches four digits on a heavy batch
 * (1881 measured) and Watcom's long is 32 bits, so the second spelling is
 * within a factor of two of overflowing. The constant divisor is fine here --
 * this runs once per thirty drawn frames, not per pixel.
 *
 * Integer and truncating, so a figure NOTES.md gives as 81.4 prints as 80 or
 * 81 depending on the batch. That is deliberate: one ns/px of resolution is
 * plenty to tell 50 from 130, and the precise figure comes from averaging
 * several batches anyway, which is what those write-ups did. */
static long gprof_nspx(long ms, long px)
{
    long kpx = px / 1000;

    if (kpx <= 0)
        return 0;
    return ms * 1000L / kpx;
}

static void gprof_log(const char *phase, const mr_gfx_prof *g)
{
    mr_log("prof: %s gfx | copy %ldms %ldkpx %ldn %ldns"
           " | blend %ldms %ldkpx %ldn %ldns"
           " | fill %ldms %ldkpx %ldn %ldns"
           " | lerp %ldms %ldkpx %ldn %ldns"
           " | stretch %ldms %ldkpx %ldn %ldns",
           phase,
           g->copy_ms,    g->copy_px / 1000,    g->copy_n,
           gprof_nspx(g->copy_ms, g->copy_px),
           g->blend_ms,   g->blend_px / 1000,   g->blend_n,
           gprof_nspx(g->blend_ms, g->blend_px),
           g->fill_ms,    g->fill_px / 1000,    g->fill_n,
           gprof_nspx(g->fill_ms, g->fill_px),
           g->lerp_ms,    g->lerp_px / 1000,    g->lerp_n,
           gprof_nspx(g->lerp_ms, g->lerp_px),
           g->stretch_ms, g->stretch_px / 1000, g->stretch_n,
           gprof_nspx(g->stretch_ms, g->stretch_px));
}

/* Average the last thirty drawn frames into the log and start again. Each
 * figure is milliseconds per *drawn* frame; the skipped count says how many
 * frames cost nothing but the interpreter update. */
static void prof_report(void)
{
    long scale = 0, pack = 0, blit = 0;
    long decode = 0;
    int  loads = 0;
    long n = g_prof_drawn ? g_prof_drawn : 1;
    long all = g_prof_update + g_prof_scene + g_prof_ui + g_prof_present;

    mr_present_profile(&scale, &pack, &blit);
    mr_image_profile(&decode, &loads);

    mr_log("prof: %ld drawn, %ld skipped | frame %ld.%02ldms = "
           "update %ld.%02ld + scene %ld.%02ld + ui %ld.%02ld + "
           "present %ld.%02ld (scale %ld.%02ld, pack %ld.%02ld, gdi %ld.%02ld)",
           g_prof_drawn, g_prof_skipped,
           all / n, (all * 100 / n) % 100,
           g_prof_update / n, (g_prof_update * 100 / n) % 100,
           g_prof_scene / n, (g_prof_scene * 100 / n) % 100,
           g_prof_ui / n, (g_prof_ui * 100 / n) % 100,
           g_prof_present / n, (g_prof_present * 100 / n) % 100,
           scale / n, (scale * 100 / n) % 100,
           pack / n, (pack * 100 / n) % 100,
           blit / n, (blit * 100 / n) % 100);

    mr_log("prof: transition %ld frames avg %ldms | still %ld frames avg %ldms"
           " | dialogue-only %ld frames avg %ldms"
           " | worst frame %ldms | %d images decoded, %ldms",
           g_prof_trans_n,
           g_prof_trans_ms / (g_prof_trans_n ? g_prof_trans_n : 1),
           g_prof_still_n,
           g_prof_still_ms / (g_prof_still_n ? g_prof_still_n : 1),
           g_prof_band_n,
           g_prof_band_ms / (g_prof_band_n ? g_prof_band_n : 1),
           g_prof_worst, loads, decode);

    gprof_log("scene", &g_prof_gscene);
    gprof_log("ui   ", &g_prof_gui);

    /* Only when asked for, and it says what it does not know: the timings in
     * this run are inflated by the digest itself, and an unchanged region is
     * an opportunity rather than a change that can simply be made. */
    if (g_rprobe) {
        mr_log("probe: %ld drawn, %ld full with a window, %ld of those "
               "comparable, %ld painted nothing new outside the band (%ld%%) "
               "-- timings in this run are invalid, the digest is unmetered",
               g_rp_drawn, g_rp_full, g_rp_all, g_rp_same,
               g_rp_all ? g_rp_same * 100 / g_rp_all : 0);

        /* Only when there is something to break down, so a run that finds no
         * waste says so in one line instead of four zeroes. The tallies
         * overlap; `clear` is the one that matters. */
        if (g_rp_same)
            mr_log("probe: of those %ld -- %ld dirtied, %ld animating, "
                   "%ld in transition, %ld blocked by none of those, "
                   "%ld reachable by dropping g_stepped alone",
                   g_rp_same, g_rp_s_dirty, g_rp_s_moving, g_rp_s_trans,
                   g_rp_s_clear, g_rp_s_step);
    }

    memset(&g_prof_gscene, 0, sizeof(g_prof_gscene));
    memset(&g_prof_gui, 0, sizeof(g_prof_gui));

    g_prof_update = g_prof_scene = g_prof_ui = g_prof_present = 0;
    g_prof_drawn = g_prof_skipped = 0;
    g_prof_trans_n = g_prof_trans_ms = 0;
    g_prof_still_n = g_prof_still_ms = 0;
    g_prof_band_n = g_prof_band_ms = 0;
    g_prof_worst = 0;
    g_rp_drawn = g_rp_full = g_rp_all = g_rp_same = 0;
    g_rp_s_dirty = g_rp_s_moving = g_rp_s_trans = 0;
    g_rp_s_step = g_rp_s_clear = 0;
}

/* One frame. Rendering is skipped outright when nothing has changed since the
 * last one, which matters more than it sounds: audio is polled from this loop,
 * so every millisecond spent laying out a menu that has not moved is a
 * millisecond the mixer is not refilling its buffers. A static game menu used
 * to cost a full composite thirty times a second for no visible difference.
 *
 * Skipping also leaves the focus list alone -- it is rebuilt during drawing,
 * so clearing it without redrawing would break hit testing. */
static void frame(long now)
{
    unsigned int t0 = 0;
    unsigned int tframe = 0;
    int   moving = 0;
    int   kind;
    int   band = 0, band_y = 0, band_h = 0;
    int   cached = 0;
    int   rp_full = 0, rp_dirty = 0, rp_moving = 0, rp_trans = 0, rp_step = 0;

    if (g_prof)
        tframe = t0 = mr_plat_ticks();

    /* Before anything can ask for an image, and once only: every mr_image_get
     * in this frame has to agree on which frame of an animation is showing,
     * including the ones behind a transition snapshot. */
    mr_image_set_time(now);

    mr_interp_update(now);

    if (g_prof)
        g_prof_update += (long)(mr_plat_ticks() - t0);

    /* A cutscene owns the screen. mr_interp_update has already decoded and
     * presented this frame of it, straight into the presenter's picture
     * buffer, and the 1280x720 stage has not been touched -- which is the
     * whole reason a movie plays at a watchable rate here (present.h).
     *
     * So there is nothing for the rest of this function to do, and real harm
     * in letting it try: compositing the stage would cost about a third of a
     * second and then paint the scene over the picture.
     *
     * The mixer is still polled. A cutscene's soundtrack goes through the
     * same mixer as everything else and the queue still has to be topped up;
     * skipping this is a gap in the sound, which is the one failure the whole
     * of movie.c is arranged to avoid. */
    if (mr_movie_active()) {
        mr_audio_poll();
        g_frames_drawn++;
        return;
    }

    moving = mr_scene_animating(now);
    kind = mr_interp_redraw_kind();

    if (!g_dirty && kind == MR_REDRAW_NONE && !moving) {
        if (g_prof)
            g_prof_skipped++;
        return;
    }

    /* A line typing itself out changes a couple of hundred pixels inside the
     * dialogue box and nothing else. Repainting and presenting just that box
     * turns the commonest frame in the game from a full 921,600-pixel
     * composite into a quarter of one -- on the target the difference between
     * a third of a second and a few tens of milliseconds.
     *
     * Everything has to agree: the interpreter that nothing outside the box
     * changed, the scene that nothing is animating, the UI that the box is
     * where it thinks it is, and the platform that the output has not been
     * resized or uncovered (g_dirty). Any doubt and the whole stage is
     * redrawn. */
    if (!g_dirty && !moving && kind == MR_REDRAW_DIALOGUE) {
        int bx, bw;
        if (mr_ui_dialogue_rect(&bx, &band_y, &bw, &band_h))
            band = 1;
    }

    /* Snapshot the four facts here, because g_dirty is cleared on the next
     * line and would read 0 by the time the digest is taken. They are recorded
     * and not applied: whether this frame's repaint was wasted is a question
     * about pixels, and these only say what would have stood in the way of
     * acting on it. Filtering on them is what made two earlier probe runs
     * return nothing. */
    if (g_rprobe) {
        rp_full   = kind == MR_REDRAW_ALL;
        rp_dirty  = g_dirty != 0;
        rp_moving = moving != 0;
        rp_trans  = mr_scene_in_transition() != 0;
        rp_step   = mr_interp_redraw_kind_nostep() == MR_REDRAW_DIALOGUE;
    }

    g_dirty = 0;
    g_frames_drawn++;
    if (g_stats_frames && (g_frames_drawn % 30) == 0)
        mr_log("main: %ld frames drawn at t=%ldms", g_frames_drawn, now);

    /* Defensive: an unbalanced clip push must not leak into the next frame
     * and blank the screen. */
    mr_clip_reset();

    mr_sl_focus_reset();

    if (band)
        mr_clip_push(0, band_y, MR_STAGE_W, band_h);

    /* The strip from under the last glyphs, if a previous frame left one that
     * still applies. Everything up to the textbox art is then already on the
     * stage and does not have to be composited again -- which on a typing
     * frame is nearly all of the work. bandcache.h has the reasoning; the
     * decision to keep a strip is at the offer below. */
    if (g_prof)
        t0 = mr_plat_ticks();

    /* Inside the `scene` timer, because that is what it stands in for. It was
     * outside it at first and the strip restore became invisible: the logged
     * `frame` figure is the *sum* of the four phases, not a measured total,
     * so 7.3ms a frame showed up only as the gap between it and the
     * dialogue-only average -- which is a figure nobody would think to
     * subtract one from the other to find. */
    if (band)
        cached = mr_band_cache_try(g_stage, band_y, band_h);

    if (!cached)
        mr_interp_render(g_stage, now);
    else
        /* The extent is "what the screens painted this time" and
         * mr_interp_render is where it is reset, so skipping the render
         * skips the reset. Only overlay screens can be up on a cached frame
         * -- the offer refuses to keep a strip otherwise -- and the overlay
         * is still rendered below, so resetting here leaves the extent
         * meaning exactly what it means on every other frame instead of
         * holding the previous frame's rect. */
        mr_sl_extent_reset();
    if (g_prof) {
        mr_gfx_prof gp;

        g_prof_scene += (long)(mr_plat_ticks() - t0);
        /* Before mr_ui_draw, so the scene's few large blits and the dialogue
         * window's many small ones stay in separate accumulators. */
        mr_gfx_profile(&gp);
        gprof_add(&g_prof_gscene, &gp);
    }

    /* Top the mixer up between the phases of an expensive frame rather than
     * only once per frame: compositing and presenting a 1280x720 stage can
     * each take long enough on the target to drain the queue. */
    mr_audio_poll();

    if (g_prof)
        t0 = mr_plat_ticks();
    /* Every frame, transition or not. The dialogue window is not part of the
     * scene and takes no part in the effect: a `with dissolve` between two
     * lines leaves it exactly where it was.
     *
     * In two halves, with the band cache taken between them: the box art is
     * the same 148,000 blended pixels on every frame of a line and the glyphs
     * are what change. The order is unchanged -- panel then front is exactly
     * what mr_ui_draw itself does. */
    if (!cached)
        mr_ui_draw_panel(g_stage);

    /* The stage is now composited up to the seam, so this is where a strip is
     * taken. `keep` is the whole of the policy, and it asks the narrow
     * question rather than the broad one: not "could any of this change?" but
     * "is what is on the stage now still what belongs under the next line's
     * glyphs?".
     *
     *   moving             something on the scene is mid-animation, so the
     *                      strip would be one frame of it
     *   in_transition      likewise, and it ends by committing a different
     *                      display list
     *   a screen under the their pixels would be inside the strip, and
     *   window             replaying one blends it over itself while skipping
     *                      it drops its focus entries (scene.h)
     *   no dialogue rect   the window is down; there is no strip to speak of
     *
     * Everything else that could invalidate a strip arrives as a full frame
     * through g_dirty or mr_invalidate, and lands here with the stage freshly
     * composited, replacing it. A cached frame holds the identical pixels
     * already, so it re-copies nothing.
     *
     * The first two are redundant today and are kept deliberately. A strip is
     * only ever *read* on a band frame, and a band frame already demands
     * !moving and !g_dirty -- so "nothing has advanced since the last drawn
     * frame" is the same sentence that makes the band path correct at all,
     * and bandtest confirms it: removing both changes no pixel and no claim.
     * They stay because without them the strip's correctness would rest
     * entirely on a condition forty lines up that has been loosened once
     * already -- interp.c:3165 explains the choice-menu case, where a screen
     * being up stopped refusing a partial frame. The next such loosening
     * should not silently become a stale-pixel bug here.
     *
     * One interaction worth naming: a cached frame skips mr_interp_render, so
     * it never fetches an image and never updates what mr_image_anim_changed
     * compares against. That record therefore stays at the animation frame
     * the strip was taken from, which is exactly what makes the *next*
     * frame's `moving` the right question rather than a stale one.
     *
     * For MR_PROF: the copy is a memcpy per row rather than a blit, so it
     * lands in `ui` wall time and in none of the compositor buckets. */
    if (!cached) {
        int ox = 0, oy = 0, ow = 0, oh = 0;
        int keep = !moving &&
                   !mr_scene_in_transition() &&
                   mr_scene_screen_count_under_window() == 0 &&
                   mr_ui_dialogue_rect(&ox, &oy, &ow, &oh);

        (void)ox;
        (void)ow;
        mr_band_cache_offer(g_stage, oy, oh, keep);
    }

    mr_ui_draw_front(g_stage);

    /* Over the dialogue window: the quick menu is `zorder 100`, and it runs
     * along the bottom edge where the textbox art is. Drawn underneath, its
     * text came out #363636 instead of #aaaaaa. */
    mr_interp_render_overlay(g_stage);

    if (g_prof) {
        mr_gfx_prof gp;

        g_prof_ui += (long)(mr_plat_ticks() - t0);
        mr_gfx_profile(&gp);
        gprof_add(&g_prof_gui, &gp);
    }

    /* After the last thing that draws and before the present, so the digest is
     * of the finished stage -- and on every drawn frame, not only the
     * candidates, because the comparison needs the previous frame's value.
     * A band frame leaves the rows outside the band untouched rather than
     * wrong, so its digest is still the truth about what is on the stage.
     *
     * The rect has to match the one the previous digest skipped or the two
     * numbers are not comparable, which is why it is stored alongside: the
     * textbox height is read from the script (gui.textbox_height) and a game
     * is free to change it mid-run. A mismatch is not counted either way. */
    if (g_rprobe) {
        int px, py, pw, ph;

        g_rp_drawn++;

        if (!mr_ui_dialogue_rect(&px, &py, &pw, &ph)) {
            /* No window, so "outside the band" is not a defined region and the
             * stored digest no longer describes a comparable rectangle. Not
             * counted either way, and reported as the gap between the drawn
             * count and the compared count so it cannot hide. */
            g_rp_have = 0;
        } else {
            mr_u32 d = mr_surface_digest_outside(g_stage, py, ph);
            int same_rect = g_rp_have && py == g_rp_y && ph == g_rp_h;

            if (rp_full)
                g_rp_full++;

            if (rp_full && same_rect) {
                g_rp_all++;

                if (d == g_rp_prev) {
                    g_rp_same++;
                    if (rp_dirty)  g_rp_s_dirty++;
                    if (rp_moving) g_rp_s_moving++;
                    if (rp_trans)  g_rp_s_trans++;
                    if (rp_step)   g_rp_s_step++;
                    if (!rp_dirty && !rp_moving && !rp_trans)
                        g_rp_s_clear++;
                }
            }

            g_rp_prev = d;
            g_rp_y = py;
            g_rp_h = ph;
            g_rp_have = 1;
        }
    }

    mr_audio_poll();

    if (band)
        mr_clip_pop();

    if (g_prof)
        t0 = mr_plat_ticks();
    if (band)
        mr_present_band(g_stage, band_y, band_h);
    else
        mr_present(g_stage);
    if (g_prof)
        g_prof_present += (long)(mr_plat_ticks() - t0);

    mr_audio_poll();

    if (g_prof) {
        long ms = (long)(mr_plat_ticks() - tframe);

        if (moving) {
            g_prof_trans_n++;
            g_prof_trans_ms += ms;
        } else if (band) {
            g_prof_band_n++;
            g_prof_band_ms += ms;
        } else {
            g_prof_still_n++;
            g_prof_still_ms += ms;
        }
        if (ms > g_prof_worst)
            g_prof_worst = ms;

        if (++g_prof_drawn >= 30)
            prof_report();
    }
}

/* Locate the game data. Ren'Py's own layout puts a `game` directory beside the
 * executable; a command line argument overrides it for testing against a
 * checkout that lives elsewhere. */
static void find_game_dir(const char *cmdline, char *out, int outsz)
{
    if (!cmdline)
        cmdline = "";

    while (*cmdline == ' ')
        cmdline++;

    if (*cmdline) {
        int n = 0;
        const char *p = cmdline;
        int quoted = (*p == '"');

        if (quoted)
            p++;
        while (*p && n < outsz - 1 && (quoted ? *p != '"' : *p != ' '))
            out[n++] = *p++;
        out[n] = '\0';
        return;
    }

    strncpy(out, "game", (size_t)outsz - 1);
    out[outsz - 1] = '\0';
}

/* The caption comes from the script. Ren'Py's precedence, from
 * 00library.rpy:467 and display/core.py:1077: config.window_title, else
 * config.name, else a generic string -- and the result goes through the
 * string translations, because options.rpy writes
 * `define config.name = _("...")`.
 *
 * This has to come after mr_interp_start, not after mr_interp_init: the init
 * phase that evaluates the `define`s runs in *start*. Placed after init it
 * found no config.name at all and silently kept the built-in caption, which
 * is what the log line below was added to catch.
 *
 * MR_TITLE remains the caption the platform created its window with, and the
 * fallback for a game that sets neither.
 */
static void apply_title(void)
{
    mr_value v;
    const char *title = NULL;

    if (mr_var_get("config.window_title", &v) && v.type == MR_V_STR && v.s[0])
        title = mr_tl_string(v.s);
    else if (mr_var_get("config.name", &v) && v.type == MR_V_STR && v.s[0])
        title = mr_tl_string(v.s);

    if (title && *title) {
        /* The platform's caption call is the 8-bit one -- Win32s has no
         * usable wide entry point -- while the script it came from is UTF-8.
         * Handing the raw bytes over put `Le départ de Kalea` in the title
         * bar as `Le dÃ©part de Kalea`, CP1252's reading of C3 A9. The
         * converted form is never longer than the source, so the buffer is
         * sized from it. */
        char ansi[MR_VSTR_MAX];

        mr_utf8_to_ansi(title, ansi, (int)sizeof(ansi));
        mr_plat_set_title(ansi);
        mr_log("main: window caption '%s'", ansi);
    } else {
        mr_log("main: no config.name or config.window_title; "
               "caption stays '%s'", MR_TITLE);
    }
}

int mr_main_startup(const char *cmdline)
{
    char gamedir[256];
    const char *pak = mr_config_str("pak", MR_PAK_NAME);

    /* `simd` again, and here rather than only in movie.c, because the blend
     * kernel runs from the first frame and movie.c reads the key when a
     * cutscene opens -- which for this one would be far too late, and for a
     * game with no cutscene would be never. Same key deliberately: a machine
     * that wants one hand-written kernel off wants them all off.
     *
     * Three values now, not two, and the middle one is the default:
     *
     *   0  off. Both kernels scalar, whatever the processor has.
     *   1  ask the processor (cpu.h). What ships.
     *   2  on regardless -- xvidcore's XVID_CPU_FORCE, same purpose: the
     *      detector is the newest thing here and its failure is silent, so
     *      there has to be a way back if it says no on a machine that can.
     *
     * The force has to happen before anything asks, which is why it is the
     * first thing in startup and not beside the other ini reads.
     *
     * **Both lines are logged always, including when everything is fine.**
     * They started out logging only the off cases, on the reasoning that a
     * normal run should stay quiet -- which was wrong twice over. "no CPUID",
     * "no mmx" and "mmx" are three different bug reports that look identical
     * from a chair, and this is the only word we ever get from a machine we do
     * not have. And silence cannot distinguish "the fast path is running" from
     * "the line that would have told you is missing from this build", which is
     * precisely the question asked of a detector that has just been added. A
     * log that only speaks up when something is broken cannot confirm that
     * anything works. Two lines a run is a cheap price.
     *
     * The colour conversion is deliberately *not* reported here. It is gated
     * on the same flag, but movie.c sets it when a cutscene opens and logs the
     * state it actually used on the `movie:` line -- so a line here would be a
     * prediction about a kernel that may never run, which is worth less than
     * the fact printed later. */
    {
        int simd = (int)mr_config_int("simd", 1, 0, 2);

        if (simd >= 2)
            mr_cpu_force_mmx();
        mr_blend_set_simd(simd != 0);

        mr_log("cpu: %s", mr_cpu_desc());

        if (mr_blend_simd())
            mr_log("gfx: MMX alpha blend on");
        else if (!mr_blend_have_simd())
            mr_log("gfx: no MMX blend -- not in this build, or this "
                   "processor has none; blends are scalar C");
        else
            mr_log("gfx: MMX blend available but simd=%d; blends are "
                   "scalar C", simd);
    }

    g_stage = mr_surface_new(MR_STAGE_W, MR_STAGE_H);
    if (!g_stage)
        mr_fatal("could not allocate the %dx%d stage (%ld KB)",
                 MR_STAGE_W, MR_STAGE_H,
                 (long)(MR_STAGE_W * MR_STAGE_H * 4L / 1024));
    mr_surface_fill(g_stage, MR_RGB(0, 0, 0));

    find_game_dir(cmdline, gamedir, sizeof(gamedir));

    /* The archive wins if present. On the real target it has to be: FAT 8.3
     * cannot represent names like script_declares.rpy. */
    if (!mr_vfs_init(pak, gamedir))
        mr_fatal("found neither %s nor a readable '%s' directory",
                 pak, gamedir);
    mr_log("main: data from %s", mr_vfs_using_pak() ? pak : gamedir);

    mr_image_init(gamedir);
    if (!mr_scene_init())
        mr_fatal("could not allocate the transition buffers");
    if (!mr_audio_init(gamedir))
        mr_log("main: audio unavailable, continuing silently");
    if (!mr_ui_init(gamedir))
        mr_log("main: no fonts in fonts/font*.dat -- nothing will be "
               "drawn at all, screens included; run `wmake data`");

    /* Before the load, so the per-file checks inside it run too. */
    mr_script_check_enable((int)mr_config_int("scriptcheck", 0, 0, 1));

    g_script = mr_script_load(gamedir);
    if (!g_script || g_script->n == 0)
        mr_fatal("no script found in '%s'", gamedir);

    /* `scriptcheck=1` only: it costs a copy of the statement array, about
     * 2MB here. See script.h. */
    if (mr_config_int("scriptcheck", 0, 0, 1))
        mr_script_snapshot(g_script);

    mr_interp_init(g_script, gamedir);
    mr_script_verify(g_script, "mr_interp_init");

    if (getenv("MR_LAYOUT"))
        mr_sl_dump_next();
    if (getenv("MR_STATS")) {
        mr_sl_stats(1);
        g_stats_frames = 1;
    }
    /* One line per rollback and rollforward. For a "rolling back shows the
     * wrong thing" report: ask for a session with this set and read the last
     * dozen lines before it went wrong. */
    if (getenv("MR_ROLLTRACE"))
        mr_rollback_trace(1);
    if (getenv("MR_PROF")) {
        g_prof = 1;
        mr_present_profile_enable(1);
        mr_image_profile_enable(1);
        mr_gfx_profile_enable(mr_plat_ticks);
        mr_log("prof: timing each phase; figures are ms per drawn frame");
        mr_log("prof: gfx lines are totals over the thirty, not averages;"
               " the trailing ns is per pixel, the only field comparable"
               " across batches");
    }

    /* Needs MR_PROF, because the counters are reported and reset by
     * prof_report and there is nowhere else they would be printed. Saying so
     * beats silently measuring nothing. */
    if (getenv("MR_REDRAWPROBE")) {
        if (!g_prof) {
            mr_log("probe: MR_REDRAWPROBE needs MR_PROF=1 as well "
                   "-- not measuring");
        } else {
            g_rprobe = 1;
            mr_log("probe: hashing the stage outside the dialogue band on "
                   "every drawn frame. This costs ~685,000 pixel reads a "
                   "frame and is in no phase timer, so TIMINGS IN THIS RUN "
                   "ARE INVALID. The counts are the measurement.");
        }
    }

    /* MR_LABEL jumps straight to a label instead of opening on the main
     * menu, so a late scene can be tested without replaying the game. */
    {
        const char *label = getenv("MR_LABEL");
        if (label && *label) {
            mr_log("main: starting at label '%s'", label);
            mr_interp_start(label);
        } else if (mr_script_find_label(g_script, "splashscreen") >= 0 &&
                   !getenv("MR_SKIP_SPLASHSCREEN")) {
            /* Ren'Py runs `label splashscreen` before the main menu, when the
             * game has one: 00start.rpy:258 guards on renpy.has_label and
             * calls _splashscreen, which jumps to it. Its `return` then falls
             * back into _start, which goes on to the main menu.
             *
             * The same thing happens here for free, because a RETURN at call
             * depth zero already ends in mr_interp_main_menu (see end_game).
             * So this only has to start at the right label.
             *
             * Without this the label was simply never executed, which is the
             * real reason this game's intro never appeared -- the missing
             * audio was a second, quieter fault behind it.
             *
             * MR_SKIP_SPLASHSCREEN mirrors RENPY_SKIP_SPLASHSCREEN, which
             * 00start.rpy:258 also honours; MR_LABEL takes the branch above
             * and skips it anyway. */
            mr_log("main: running the splashscreen");
            mr_interp_splashscreen();
        } else {
            mr_interp_start("start");
            mr_interp_main_menu();
        }

        /* MR_SCREEN opens straight onto a menu screen, so its layout can be
         * inspected without clicking through to it. */
        {
            const char *screen = getenv("MR_SCREEN");
            if (screen && *screen)
                mr_interp_show_screen(screen);
        }
    }

    /* Both of these need the init phase to have run, which happens in
     * mr_interp_start rather than mr_interp_init. */
    mr_ui_sync_fonts();
    mr_ui_sync_geometry();

    apply_title();
    return 1;
}

/* How often to draw, asked once.
 *
 * Here and not in mr_main_init because `config.framerate` is a `define`, and
 * a define is live only after the init phase -- which runs inside
 * mr_interp_start, below the point where the rest of the ini is read. The
 * same reason mr_ui_sync_fonts exists.
 *
 * mrpy.ini wins over the script, which is the opposite of the usual order and
 * is deliberate: every other setting the script declares is about how the game
 * should look, and this one is about how much work the machine in front of the
 * player is asked to do. A default of 0 for the ini key is "not written",
 * which is also where an unparseable or out-of-range value lands -- and
 * mr_config_int has already logged it by then.
 *
 * Clamped at 10fps below, because an interval long enough to be noticed as
 * lag is not a frame rate, and at 200 above, where the interval is 5ms and the
 * platform clock's own resolution of a millisecond starts to be most of the
 * answer. */
static void pick_frame_rate(void)
{
    long fps = mr_config_int("fps", 0, 10, 200);
    const char *why = "mrpy.ini";

    if (fps == 0) {
        fps = mr_var_get_int("config.framerate", 0);
        why = "config.framerate";
        if (fps < 10 || fps > 200) {
            fps = MR_FPS_DEFAULT;
            why = "the default";
        }
    }

    g_frame_ms = (unsigned int)(1000 / fps);
    /* Logged whichever way it was decided, not only when it is unusual: "the
     * fast path is running" and "this build has no such line" are otherwise
     * the same silence. */
    mr_log("main: aiming at %ldfps (%ums a frame), from %s",
           fps, g_frame_ms, why);
}

void mr_main_loop(void)
{
    unsigned int next_frame;
    unsigned int now;

    pick_frame_rate();

    g_start_ms = mr_plat_ticks();
    next_frame = g_start_ms;

    while (g_running) {
        if (!mr_plat_pump()) {
            g_running = 0;
            break;
        }

        mr_audio_poll();

        /* Before the frame rather than after: changing the display can
         * resize the output, and the frame that follows should already be
         * drawing at the new size. */
        display_sync();

        now = mr_plat_ticks();
        if ((long)(now - next_frame) >= 0) {
            frame((long)(now - g_start_ms));
            next_frame += g_frame_ms;
            /* If we fell far behind, resynchronise instead of trying to catch
             * up frame by frame. Four intervals and not a fixed number of
             * milliseconds, so the slack scales with the rate: at a high rate
             * a machine that cannot keep up resynchronises sooner, which is
             * what should happen -- the frames it missed are not worth
             * drawing late. */
            if ((long)(mr_plat_ticks() - next_frame) > (long)(g_frame_ms * 4))
                next_frame = mr_plat_ticks() + g_frame_ms;
        } else {
            mr_plat_yield();
        }
    }
}

void mr_main_shutdown(void)
{
    mr_log("main: %ld frames drawn", g_frames_drawn);
    mr_save_shutdown();
    mr_ui_shutdown();
    mr_interp_shutdown();
    mr_script_free(g_script);
    g_script = NULL;
    mr_audio_shutdown();
    mr_scene_shutdown();
    mr_image_shutdown();
    mr_vfs_shutdown();
    mr_present_shutdown();
    mr_surface_free(g_stage);
    g_stage = NULL;
}

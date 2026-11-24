/* movie.c - cutscene playback. See movie.h.
 *
 * viddec.h does the codec; this file decides *when*. That is the whole of the
 * interesting content here, so it is worth saying plainly what the clock is
 * and why it is not the obvious one.
 *
 * --- the sound card is the clock -------------------------------------------
 *
 * The obvious construction is a timer: note the millisecond the cutscene
 * started, and show the frame whose timestamp matches the time elapsed since.
 * That is wrong on this target in a way that is easy to miss on a fast host.
 *
 * Audio, once started, plays at a rate set by a crystal on the sound card.
 * Nothing can speed it up or slow it down, and nothing may interrupt it: a
 * gap in the sound is audible and awful, where a late frame is a stutter
 * nobody can name. So the audio is the thing that must be kept fed, and the
 * picture is what gives way. A timer-driven picture would be correct against
 * the wall clock and wrong against the sound -- and on a machine whose
 * waveOut clock and whose millisecond timer are two different oscillators,
 * it would drift apart from it over a three-minute cutscene even if every
 * frame arrived on time.
 *
 * So: the clock is mr_audio_output_position_ms(MR_CH_MOVIE) -- how much of
 * this cutscene's soundtrack the card says it has actually *played*. The
 * picture is a function of the sound. Frames whose moment has passed are
 * decoded and thrown away rather than shown late, which is the only
 * degradation available and is the right one.
 *
 * A silent movie has no such clock -- the channel reports 0 forever -- so
 * that case, and only that case, falls back to mr_plat_ticks. Checked at
 * open, not per frame; see g_has_audio.
 *
 * --- what runs ahead of what ----------------------------------------------
 *
 * Audio is decoded *before* video every update, and bounded by the room in
 * the mixer's ring rather than by a frame count. That ordering is the whole
 * underrun strategy: if a frame of this engine takes 200ms because a
 * transition or a disk seek got in the way, the mixer has already been given
 * everything it can hold before the expensive part began.
 *
 * Because the clock is the *played* position and the mixer queues ahead of
 * the speaker, the picture necessarily lags the mixer by the queue depth.
 * That is correct -- it matches sound to picture as heard -- but the default
 * runway is over a second, which would hold the first frame on screen for
 * that long before the second arrived. So playback shortens the queue, the
 * way the rhythm game does and for a related reason, and puts it back
 * afterwards. mrpy.ini's `movie_latency_ms` is the knob, because the floor
 * is a property of the machine and cannot be found from here.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mr.h"
#include "movie.h"
#include "yuv.h"
#include "audio.h"
#include "config.h"
#include "plat.h"
#include "present.h"
#include "vfs.h"
#include "script.h"
#include "viddec.h"

/* No vendor header here, deliberately -- not pl_mpeg's, not xvidcore's.
 * viddec.h is the whole of this file's view of a decoder, and it names no
 * codec: viddec.c sniffs the file and picks one, and plmimpl.c is the only
 * translation unit that includes pl_mpeg at all, compiled with -za99 for it.
 * That keeps this file -- where every decision about timing and dropping is
 * made -- under the engine's C89 rule, where the host build and the Watcom
 * build agree about what is legal.
 *
 * It is also what made a second codec cheap. Nothing below this line changed
 * when xvid was added except the spelling of four calls: the clock, the
 * drop policy and the handover to the presenter are properties of *when* a
 * frame is shown and have nothing to do with what decoded it. */

/* How much of the file to hand the demuxer at a time.
 *
 * The program stream is read in chunks through the VFS, which on DOS means a
 * read off a hard disk while the decoder waits. ending.mpg is 3.8 Mbit/s, so
 * a second of movie is about 480KB and this is a tenth of that -- small
 * enough that no single read stalls a frame for long, large enough that the
 * per-read overhead and any filesystem seek are amortised over several
 * frames.
 *
 * It is not a decode buffer and does not need to hold a frame: pl_mpeg's own
 * ring buffer accumulates until it has a whole picture, and asks for more
 * through the load callback when it does not. */
#define MOVIE_CHUNK  (48L * 1024L)

/* The most video frames one update will decode before giving up and showing
 * what it has.
 *
 * This is the frame-dropping cap, and the number is set by the audio ring
 * rather than chosen for its own sake. RING_FRAMES is 8192 frames at 44.1kHz
 * -- about 186ms, or four and a half frames of 25fps video -- and audio is
 * topped up once per update, before this loop runs. Decoding more than four
 * video frames in one update therefore risks spending longer than the mixer
 * can cover, which trades a stutter for a gap in the sound: exactly the wrong
 * way round.
 *
 * So a cutscene that falls badly behind does not try to catch up in one go.
 * It catches up at four frames an update, or it stays behind and the picture
 * runs slow against the sound -- which is visible, and is reported in the
 * log, and is better than silence. */
#define MOVIE_CATCHUP  4

/* 16-bit stereo scratch for one decoded MP2 frame. pl_mpeg hands over 1152
 * samples per channel as floats; this is where they land on the way to the
 * mixer's ring. */
#define MOVIE_ASAMP  MR_VID_AUDIO_FRAME

static mr_vid   *g_vid;
static mr_file  *g_file;

static int   g_active;
static int   g_ended;           /* the demuxer has run out of video */
static int   g_has_audio;
static int   g_rate;

static long  g_duration_ms;
static long  g_frame_ms;        /* nominal, from the framerate */

/* The timestamp of the frame currently on screen, and when the next one is
 * due, both in milliseconds from the start of the movie. */
static long  g_shown_ms;
static long  g_next_due_ms;

/* The wall clock at the start, for a movie with no soundtrack. */
static unsigned int g_start_ticks;

/* What to put the mixer's queue back to when the cutscene ends. 0 means it
 * was at the default and the default is what to restore. */
static long  g_saved_queue_ms;

static long  g_shown;
static long  g_dropped;

/* Decode cost, so that the split between decoding and displaying is measured
 * rather than inferred from the frame counts. The first log of this on the
 * target settled a question the host could not even pose: the presenter's
 * figures were measured and the decoder's were being backed out of a
 * subtraction that also absorbed the audio, which is why the audio decode is
 * counted separately here rather than folded in.
 *
 * mr_plat_ticks resolves to a millisecond and one frame's decode is of that
 * order, so neither of these means much per frame -- they are totals, divided
 * only when they are reported. */
static long  g_vdec_ms;
static long  g_vdec_n;
static long  g_adec_ms;

/* The frame currently on screen, kept so a repaint that did not come from the
 * frame loop can put it there again -- see mr_movie_repaint.
 *
 * The planes point into the decoder's own buffers and are valid until the
 * next decode, which is exactly the window this is used in: WM_PAINT arrives
 * between updates, never during one. Holding it for longer than that would
 * not be safe, and nothing does. */
static mr_yuv_frame g_last;
static int          g_have_last;

/* The path this was opened with, so mr_movie_replay can open it again.
 * MR_PATH_MAX because it is a VFS name, which is what the statement's
 * argument is. */
static char  g_path[MR_PATH_MAX];

static short g_apcm[MOVIE_ASAMP * 2];

/* Silence the mixer emitted for want of being fed, in milliseconds.
 *
 * Declared up here because mr_movie_close logs it, and must read it *before*
 * it stops the channel -- mr_audio_stop closes the source and the count goes
 * with it. The division is by 44 rather than 44.1: this is a log line, and a
 * tenth of a percent on a figure whose interesting values are 0 and
 * "hundreds" is not worth a float on this target. */
static long movie_starved_ms(void)
{
    return mr_audio_push_starved(MR_CH_MOVIE) / 44;
}

/* --- the file, through the VFS -------------------------------------------
 *
 * The decoder never opens anything. PLM_NO_STDIO is defined in plmimpl.c
 * precisely so that it cannot, and these three callbacks are the only way it
 * reaches a byte. A movie is an asset like any other and lives inside
 * mrpy.pak, and `MR_PAK selects the game and nothing else does` (CLAUDE.md)
 * stops being true the moment one asset is reachable by a second route.
 *
 * seek and tell are supplied for the container's duration, and for the sniff
 * that decides which decoder gets the file -- viddec.c reads twelve bytes and
 * rewinds. The duration is worth two disk seeks at open: the alternative is a
 * cutscene whose length is unknown until it ends, which is what the
 * poster-frame stand-in had to live with. An MP3 records no duration, so that
 * path started its wait at a floor and extended it once the decoder had seen
 * a frame; both container formats have the figure in them, so the wait is
 * right from the first frame.
 *
 * The two get it differently, which is worth knowing when one of them
 * reports 0. A program stream has no field for it, so pl_mpeg seeks near the
 * end and reads forward for the last timestamp -- two seeks, and a wrong
 * answer on a stream with no PTS. AVI records the frame count and the rate in
 * its stream header, so avi.c reads it out of the headers it is walking
 * anyway, exactly and without seeking.
 */
static long movie_read(void *user, void *buf, long bytes)
{
    (void)user;
    return g_file ? mr_vfs_read(g_file, buf, bytes) : 0;
}

static void movie_seek(void *user, long offset)
{
    (void)user;
    if (g_file)
        mr_vfs_seek(g_file, offset, SEEK_SET);
}

static long movie_tell(void *user)
{
    (void)user;
    return g_file ? mr_vfs_tell(g_file) : 0;
}

static void movie_release(void)
{
    if (g_vid) {
        mr_vid_close(g_vid);
        g_vid = NULL;
    }
    if (g_file) {
        mr_vfs_close(g_file);
        g_file = NULL;
    }
}

int mr_movie_open(const char *path)
{
    mr_vid_io   io;
    mr_vid_info info;
    double fps;

    if (!path || !*path)
        return 0;

    /* mrpy.ini's `movie=0` refuses every movie, which sends interp.c to the
     * poster-frame stand-in instead.
     *
     * This is the escape hatch, and it is the reason the stand-in was kept
     * rather than deleted when the decoder arrived. A machine that cannot
     * keep up with a particular file -- too slow a disk, a frame size nobody
     * tested, a cutscene that stutters where the rest of the game is fine --
     * needs a way out that does not involve re-encoding assets or editing a
     * pak. One line in an ini file, and the cutscene becomes a held frame
     * with its soundtrack, which is what the port did for months and is
     * perfectly watchable.
     *
     * Checked before the file is opened so that it costs nothing when set,
     * and so that `movie=0` behaves exactly as a missing .mpg does -- one
     * fallback path, not two. */
    if (!mr_config_int("movie", 1, 0, 1)) {
        mr_log("movie: '%s' not played; mrpy.ini has movie=0", path);
        return 0;
    }

    mr_movie_close();

    g_file = mr_vfs_open(path);
    if (!g_file) {
        mr_log("movie: no '%s'", path);
        return 0;
    }

    io.read = movie_read;
    io.seek = movie_seek;
    io.tell = movie_tell;
    io.size = mr_vfs_size(g_file);
    io.user = NULL;

    /* mrpy.ini's `simd`. Read here, at the one place that is about to use it,
     * rather than at startup: nothing outside video playback touches a SIMD
     * kernel, and reading it in main.c would mean a setting whose only effect
     * is in a file main.c does not know about.
     *
     * Default on, because the Pentium III baseline has MMX. The switch is not
     * for tuning -- it is so that "the picture is wrong" and "a SIMD kernel
     * is wrong" can be told apart on the machine, in one run, without a
     * rebuild. That is the only way to tell them apart at all, since neither
     * kernel can be reached from the host suite; see yuvmmx.h and xviddec.h.
     *
     * One setting, two kernels now: our colour conversion and xvidcore's
     * decoder. **Both calls must precede mr_vid_open**, and that is the whole
     * reason this block sits above it rather than beside the other ini reads
     * below -- xvidcore latches its detected cpu features into function
     * pointer tables in a one-time global init, so a flag set afterwards
     * would apply to the next cutscene and not this one. A diagnostic switch
     * that takes effect one movie late is worse than none. */
    {
        /* 0..2, matching main.c: 2 is "force, do not believe the detector".
         * Clamping to 1 here would have quietly turned `simd=2` into `simd=1`
         * for the cutscene path only, which is the sort of difference nobody
         * finds. The force itself is main.c's -- it has to happen before the
         * first blit, long before this -- so all that is needed here is to
         * read the same range and treat anything non-zero as on. */
        int simd = (int)mr_config_int("simd", 1, 0, 2) != 0;

        mr_yuv_set_simd(simd);
        mr_vid_set_simd(simd);
    }

    memset(&info, 0, sizeof(info));
    g_vid = mr_vid_open(&io, &info);
    if (!g_vid) {
        /* Neither codec would take it, or there was no video track in it, or
         * memory ran out. All three are "this will not play", and the
         * caller's poster-frame fallback is a better answer than a black
         * screen with sound, so they are not worth distinguishing to the
         * player -- only in the log. */
        mr_log("movie: '%s' will not decode as MPEG-1 or as xvid in AVI",
               path);
        movie_release();
        return 0;
    }

    g_has_audio = info.has_audio;

    fps = info.fps;
    if (fps < 1.0 || fps > 120.0) {
        /* A sequence header with a reserved frame-rate code. The stream is
         * still playable -- every frame carries its own timestamp -- and the
         * nominal rate only decides when the *next* frame is due, so a sane
         * default costs nothing but a little jitter. */
        mr_log("movie: '%s' reports %.2f fps; assuming 25", path, fps);
        fps = 25.0;
    }
    g_frame_ms = (long)(1000.0 / fps + 0.5);
    if (g_frame_ms < 1)
        g_frame_ms = 1;

    g_duration_ms = info.duration_ms;
    g_rate = info.sample_rate > 0 ? info.sample_rate : 44100;

    g_shown = 0;
    g_dropped = 0;
    g_vdec_ms = 0;
    g_vdec_n = 0;
    g_adec_ms = 0;
    g_have_last = 0;
    g_shown_ms = -1;
    g_next_due_ms = 0;
    g_ended = 0;
    g_active = 1;
    g_start_ticks = mr_plat_ticks();
    g_saved_queue_ms = 0;

    if (g_has_audio) {
        /* Shorten the mixer's runway for the duration.
         *
         * The clock is the *played* audio position, so the picture trails the
         * mixer by whatever is queued ahead of the speaker. At the default
         * depth that is over a second, which would show the first frame and
         * then hold it for a second before the second frame came due -- a
         * visible lurch at the start of every cutscene.
         *
         * Shallower trades against underruns, and an underrun during video
         * decoding is more likely than during anything else the engine does,
         * so this is not a value to pick from a desk. mrpy.ini's
         * movie_latency_ms exists so the floor can be found on the real
         * machine, exactly as rhythm_latency_ms was. The default is a little
         * over four frames of 25fps video.
         *
         * Clamped against mr_audio_max_queue_ms rather than a literal: the
         * runway has been changed three times and every hardcoded copy of the
         * figure went stale silently. */
        long want = mr_config_int("movie_latency_ms", 400, 0,
                                  mr_audio_max_queue_ms());

        g_saved_queue_ms = mr_audio_queue_ms();
        if (want > 0)
            mr_audio_set_queue_ms(want);

        mr_audio_push_open(MR_CH_MOVIE, g_rate, 1.0f);
    }

    strncpy(g_path, path, sizeof(g_path) - 1);
    g_path[sizeof(g_path) - 1] = '\0';

    /* The codec is in the line because the choice is made from the file's own
     * bytes rather than from its name, so it is not predictable from the
     * script. A cutscene that was re-encoded to AVI and silently fell back to
     * the MPEG decoder -- or did not fall back when it should have -- looks
     * exactly like a cutscene that is slow, and this is what separates them
     * without a second run. */
    mr_log("movie: %s %dx%d %.2ffps %ldms%s, %s, %ld-frame catch-up, "
           "queue %ldms", path, info.width, info.height, fps, g_duration_ms,
           g_has_audio ? "" : " (silent)",
           info.codec ? info.codec : "?", (long)MOVIE_CATCHUP,
           mr_audio_queue_ms());

    return 1;
}

void mr_movie_close(void)
{
    long scale = 0, pack = 0, blit = 0;
    long n;

    if (!g_active) {
        movie_release();
        return;
    }

    /* Read the starvation count before stopping the channel: mr_audio_stop
     * closes the source, and the count lives in it. */
    mr_log("movie: %ld frames shown, %ld dropped, %ldms of audio starved",
           g_shown, g_dropped, movie_starved_ms());

    /* Drain the presenter's profile accumulators here, and do it whether or
     * not anybody asked for a profile.
     *
     * main.c's prof_report never runs while a cutscene is playing -- frame()
     * returns early for an active movie, well before the 30-frame counter --
     * so the milliseconds mr_present_video added would otherwise sit in
     * present.c until the first ordinary report after the movie, and be
     * divided by thirty *stage* frames and added to the stage's own figures.
     * That is a wrong number in the direction that looks reasonable, which is
     * the worst kind. Draining it here keeps the two measurements apart.
     *
     * Per frame *shown*, not per frame decoded: a dropped frame is
     * reconstructed but never converted or blitted, so it contributes
     * nothing to either column and dividing by it would flatter both. */
    mr_present_profile(&scale, &pack, &blit);
    if (scale || pack || blit) {
        n = g_shown ? g_shown : 1;
        mr_log("movie: per shown frame -- convert %ld.%02ldms, "
               "pack %ld.%02ldms, blit %ld.%02ldms",
               scale / n, (scale * 100 / n) % 100,
               pack / n, (pack * 100 / n) % 100,
               blit / n, (blit * 100 / n) % 100);
    }

    /* Per frame *decoded*, against the display figures above which are per
     * frame shown. Two different denominators on purpose: that is what the
     * two costs are actually proportional to, and making them share one would
     * misreport whichever it was not. */
    if (g_vdec_ms || g_adec_ms) {
        n = g_vdec_n ? g_vdec_n : 1;
        mr_log("movie: per decoded frame -- video %ld.%02ldms "
               "(%ld frames, %ldms total); audio %ldms total",
               g_vdec_ms / n, (g_vdec_ms * 100 / n) % 100,
               g_vdec_n, g_vdec_ms, g_adec_ms);
    }

    if (g_has_audio) {
        /* Cut it off rather than letting the ring drain: this is reached
         * both when the movie ended and when the player skipped it, and a
         * skipped cutscene whose last fifth of a second of sound carried on
         * over the next scene would be a bug in the obvious case. Anything
         * that wanted the tail has already waited for it in mr_movie_update.
         *
         * The queue depth goes back to whatever it was, not to the default:
         * a cutscene inside a screen that had asked for a shallow queue must
         * not come out of it having silently deepened one. */
        mr_audio_stop(MR_CH_MOVIE, 0);
        mr_audio_set_queue_ms(g_saved_queue_ms);
    }

    movie_release();
    g_active = 0;
    g_has_audio = 0;
    g_ended = 0;

    /* The decoder is gone, so the planes g_last points into are freed. */
    g_have_last = 0;
}

int mr_movie_active(void)
{
    return g_active;
}

int mr_movie_repaint(void)
{
    if (!g_active || !g_have_last)
        return 0;

    mr_present_video(&g_last);
    return 1;
}

int mr_movie_replay(void)
{
    char path[MR_PATH_MAX];

    if (!g_path[0])
        return 0;

    /* Copied out first: mr_movie_open calls mr_movie_close, which does not
     * clear g_path -- but relying on that would make this depend on the order
     * of two functions that have no other reason to be coupled. */
    strcpy(path, g_path);
    return mr_movie_open(path);
}

long mr_movie_duration_ms(void)
{
    return g_duration_ms;
}

void mr_movie_stats(long *shown, long *dropped, long *starved_ms)
{
    if (shown)      *shown = g_shown;
    if (dropped)    *dropped = g_dropped;
    if (starved_ms) *starved_ms = movie_starved_ms();
}

long mr_movie_decoded(void)
{
    return g_vdec_n;
}

/* --- audio ---------------------------------------------------------------
 *
 * Decode the soundtrack into the mixer's ring, as much as there is room for
 * and no more -- MP2 out of a program stream or MP3 out of an AVI, which is
 * viddec.h's business and not this loop's. Asking for the room first is the
 * contract audio.h sets out: mr_audio_push truncates what will not fit, and a
 * truncated push is audio lost in the middle of a soundtrack, which is not
 * something anybody could diagnose from the speaker.
 *
 * Whatever conversion the samples needed to reach interleaved 16-bit stereo
 * is on the decoder's side of viddec.h, with the decoder that produced them:
 * pl_mpeg hands over floats and minimp3 hands over shorts, and neither fact
 * belongs here.
 *
 * The loop is bounded by the ring and not by a frame count, which is the
 * whole underrun strategy in one line: however many audio frames it takes to
 * fill the mixer, that is how many are decoded, and it happens before
 * anything expensive runs.
 */
static void feed_audio(void)
{
    while (mr_audio_push_space(MR_CH_MOVIE) >= MOVIE_ASAMP) {
        unsigned int t0 = mr_plat_ticks();
        int n = mr_vid_decode_audio(g_vid, g_apcm, MOVIE_ASAMP);

        g_adec_ms += (long)(unsigned int)(mr_plat_ticks() - t0);

        if (n <= 0)
            return;             /* nothing more available this update */

        mr_audio_push(MR_CH_MOVIE, g_apcm, n);
    }
}

/* Where the movie is, in milliseconds from its start. */
static long movie_clock_ms(void)
{
    if (g_has_audio)
        return mr_audio_output_position_ms(MR_CH_MOVIE);

    /* Unsigned subtraction, then widened: mr_plat_ticks wraps, and plat.h is
     * explicit that only differences may be taken. */
    return (long)(unsigned int)(mr_plat_ticks() - g_start_ticks);
}

int mr_movie_update(void)
{
    long clock;
    int decoded = 0;
    mr_yuv_frame frame;
    int have_frame = 0;

    if (!g_active)
        return 0;

    /* Audio first, always. See the header: by the time anything expensive
     * runs, the mixer has been given everything it can hold. */
    if (g_has_audio)
        feed_audio();

    if (g_ended) {
        /* The video is finished but the soundtrack may not be. Let it play
         * out -- a cutscene that ends on a cymbal should be allowed to finish
         * it -- and report running until the ring has drained. */
        if (g_has_audio && mr_audio_active(MR_CH_MOVIE))
            return 1;
        return 0;
    }

    clock = movie_clock_ms();

    /* Nothing due yet: what is on screen is still the right picture. This is
     * the branch that stops the engine's frame loop -- which is faster than
     * 25fps with nothing else to do -- from running the decoder flat out and
     * overtaking the sound. */
    if (g_shown_ms >= 0 && clock < g_next_due_ms)
        return 1;

    /* Decode forwards until the frame in hand is the one that belongs on
     * screen now. Frames whose moment has already passed are decoded and
     * dropped: they cannot be skipped, because a P or B picture is expressed
     * as a difference from its references and the references have to be
     * reconstructed whether or not anyone sees them. Dropping is therefore
     * only a saving on the conversion and the blit -- which, with the
     * conversion running at window size, is about a third of the cost of a
     * frame. That is the ceiling on how much catching up is possible, and it
     * is why MOVIE_CATCHUP is small. */
    for (;;) {
        long pts = 0;
        unsigned int t0 = mr_plat_ticks();
        int got = mr_vid_decode_video(g_vid, &frame, &pts);

        /* Timed around the decode alone, and counted for dropped frames as
         * well as shown ones -- a dropped frame costs a full reconstruction
         * and only saves the conversion and the blit. Averaging it over the
         * frames that reached the screen would make the decoder look several
         * times more expensive than it is.
         *
         * Only a decode that produced a frame is counted, so that g_vdec_n is
         * exactly shown + dropped and the average is per real frame. The one
         * failed call at end of stream is left out of both: it reads to EOF
         * rather than reconstructing a picture, and charging that to the
         * frame it did not produce would misreport the last frame of every
         * cutscene. */
        if (got) {
            g_vdec_ms += (long)(unsigned int)(mr_plat_ticks() - t0);
            g_vdec_n++;
        }

        if (!got) {
            g_ended = 1;
            if (g_has_audio)
                mr_audio_push_end(MR_CH_MOVIE);
            break;
        }

        decoded++;
        have_frame = 1;
        g_shown_ms = pts;

        /* A frame is the current one if the *next* one is not due yet. */
        if (g_shown_ms + g_frame_ms > clock)
            break;

        if (decoded >= MOVIE_CATCHUP)
            break;

        /* Already stale: go round and decode past it without converting or
         * blitting. The frame's planes are only valid until the next decode,
         * which is exactly why dropping is decided here and not by queueing
         * frames up and choosing later.
         *
         * Nothing is counted at this point -- see below. */
    }

    if (have_frame) {
        mr_present_video(&frame);
        g_last = frame;
        g_have_last = 1;
        g_shown++;
        g_next_due_ms = g_shown_ms + g_frame_ms;
    }

    /* Everything decoded this update except the one that reached the screen.
     *
     * Counted here rather than at the `continue` above, because a frame
     * passed over as stale can still turn out to be the last picture of the
     * movie: if the following decode hits end of stream, the loop breaks with
     * that frame still in hand and it is presented. The old count incremented
     * before knowing that, so such a frame was counted as dropped *and* as
     * shown, and the two totals in the log overlapped by one at the end of
     * every cutscene that finished mid-catch-up. movietest pins the identity
     * now: decoded == shown + dropped, with nothing counted twice. */
    g_dropped += decoded - (have_frame ? 1 : 0);

    if (g_ended) {
        if (g_has_audio && mr_audio_active(MR_CH_MOVIE))
            return 1;
        return 0;
    }

    return 1;
}

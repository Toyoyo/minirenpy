/* audio.c - waveOut mixing, with MP3 through minimp3 and PCM WAV.
 *
 * Two things drive the design.
 *
 * Win32s cannot be trusted to deliver waveOut completion notifications: the
 * w3vn port found callbacks unreliable and polled instead, so that is what
 * this does. mr_audio_poll runs from the frame loop, looks for buffers the
 * driver has finished with, refills them and queues them again. Nothing here
 * runs at interrupt time, which also means no reentrancy to worry about on a
 * cooperatively multitasked system.
 *
 * Everything is mixed to one output format (44100Hz, 16-bit, stereo) so the
 * driver is opened once and never reconfigured. Sources at other rates are
 * resampled; mono sources are duplicated. In practice the game's MP3s are
 * already 44100 stereo, so the common path is a straight copy.
 *
 * MP3s are streamed rather than loaded whole: the music tracks are several
 * megabytes each and the memory budget is tight. A 16KB input window is
 * refilled from the VFS and compacted as minimp3 consumes it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mr.h"
#include "audio.h"
#include "vfs.h"

/* No platform header here any more. The device half of this file -- the one
 * that knew what waveOut was -- is wave.h now, with one implementation per
 * target; everything left is decode, resample, mix and fade, which is
 * portable C89. The DOS port is what forced the split, and the Sound
 * Blaster sink reuses the whole of the rest of this file unchanged. */
#include "wave.h"
#include "minimp3.h"

#define OUT_RATE    MR_WAVE_RATE
#define OUT_CH      MR_WAVE_CH

/* Runway is NBUF * BUF_FRAMES: 24 x 2048 frames is 1114ms at 44.1kHz, in
 * 192KB. Four buffers was not enough -- a transition at 1280x720 on a
 * Pentium III can spend a couple of hundred milliseconds in a single frame,
 * and the mixer is only refilled between frames, so a burst of slow frames
 * drained the queue faster than it could be topped up. An underrun is far
 * more noticeable than latency.
 *
 * 743ms (16 buffers) was not enough either. It survived transitions but still
 * stuttered in heavy menus, where style resolution and screen layout make
 * several slow frames in a row rather than one; 24 removed the last of it on
 * the real machine. Call mr_audio_max_queue_ms rather than writing 1114 down
 * anywhere: the derived figure is what tracks a change to either constant.
 *
 * Except where the runway is not wanted -- see g_queue_max. The split into
 * smaller buffers buys nothing by itself; it is there so that the depth of
 * the queue can be chosen in 46ms steps rather than 93ms ones.
 *
 * The two numbers themselves are in wave.h, because the sink allocates the
 * buffers -- a DMA buffer has requirements malloc knows nothing about. The
 * reasoning stays here, where the mixer that suffered for it is. */
#define NBUF        MR_WAVE_BUFS
#define BUF_FRAMES  MR_WAVE_FRAMES
#define BUF_SAMPLES (BUF_FRAMES * OUT_CH)
#define BUF_BYTES   (BUF_SAMPLES * 2)

/* Decoded-but-not-yet-mixed PCM, always stereo at the source rate. */
#define RING_FRAMES 8192

#define IN_WINDOW   16384

typedef struct {
    int   active;
    int   is_mp3;

    /* A *pushed* source: nothing here opens it, reads it or knows how long it
     * is. movie.c decodes MP2 out of the same .mpg the video comes from and
     * hands the PCM over with mr_audio_push, because a cutscene's audio and
     * its picture are interleaved in one file and one demuxer -- there is no
     * second file for this mixer to open.
     *
     * Two behaviours differ from a file source, and both are about starving.
     * A file source that runs dry is finished: the track ended, so the
     * channel loops, starts what was queued, or goes inactive. A pushed
     * source that runs dry has merely not been fed yet -- the frame that
     * would have decoded more audio was late -- and ending the channel there
     * would kill a cutscene's sound for good on one slow frame, which on this
     * target is a transition or a disk seek. So an empty ring is silence and
     * the channel stays active until `push_done` says the stream really has
     * ended.
     *
     * It also cannot be rewound: there is no seek, and `loop` is meaningless.
     * Replaying a cutscene restarts the demuxer instead. */
    int   is_push;
    int   push_done;        /* the feeder says no more is coming */
    long  push_starved;     /* output frames of silence from an empty ring */

    /* The stream is over and the ring has run out, but the card has not
     * caught up yet: the mixer runs a whole queue ahead of the speaker, so
     * going inactive at that instant cut the last queue's worth of a
     * cutscene. `left` is how many output frames the device still has to
     * play before this source has been heard, counted down by its own
     * progress; `mixes` bounds the wait. See the drain in chan_mix. */
    int   push_draining;
    long  push_drain_left;
    long  push_drain_played;
    long  push_drain_mixes;

    mr_file      *f;
    unsigned char in[IN_WINDOW];
    int           in_len;
    int           eof;

    mp3dec_t mp3;
    int      xing_tried;   /* the Xing tag is looked for once, before decoding */

    /* The global mixed-frame count at the instant this source's first frame
     * entered the mix. Subtracting it from what the *device* reports having
     * played gives where the channel is in its track as heard, rather than as
     * mixed -- see mr_audio_output_position_ms. */
    long   start_mixed;

    /* Whole-file PCM for WAV. */
    short *wav;
    long   wav_frames;
    long   wav_pos;

    /* Source PCM waiting to be mixed, interleaved stereo. */
    short ring[RING_FRAMES * OUT_CH];
    long  ring_frames;
    long  ring_pos_fp;      /* 16.16 fractional read position */

    int   src_rate;

    int   loop;
    char  path[MR_PATH_MAX];

    /* Fades and volume, all Q8 (256 == unity). */
    int   gain;
    int   gain_target;
    int   gain_step;        /* per output frame, Q8 scaled by 65536 */
    long  gain_acc;
    int   stop_when_silent;

    char  queued[MR_PATH_MAX];
    int   has_queued;

    /* A track waiting for the current one to finish fading out. `play music
     * "X" fadeout 1.0` means "fade out whatever is playing, then start X",
     * which is what Ren'Py's play does: it calls fadeout() and then enqueues.
     * Distinct from `queued`, which is `queue music` and waits for the track
     * to end of its own accord. */
    char  pending[MR_PATH_MAX];
    int   pending_loop;
    long  pending_fadein;
    int   has_pending;

    /* Output frames emitted since this track started, which is elapsed time
     * in the track whatever the source rate, and the track's length. Both are
     * for the jukebox's position bar; nothing in the mixer reads them. */
    long  out_frames;
    long  duration_ms;
} mr_chan;

static mr_chan  g_ch[MR_CH_COUNT];
static float    g_volume[MR_CH_COUNT];
static int      g_mute[MR_CH_COUNT];

/* Total frames ever mixed to the device. Advances one buffer at a time in
 * fill_buffer, so it is the index the next frame mixed will occupy -- which
 * is what a channel records when its source starts. */
static long     g_mixed_frames;

/* The device, its buffers and their state are the sink's; see wave.h. All
 * that is left here is whether the mixer is running at all. */
static int      g_started;

/* How many buffers may be outstanding at once -- see mr_audio_set_queue_ms.
 * All of them unless a screen asks for less. */
static int      g_queue_max = NBUF;

/* Mix accumulator, wider than the output so overlapping sources can be
 * summed before clipping once. */
static long g_mix[BUF_SAMPLES];

static void set_fade(mr_chan *c, int from, int to, long ms);

/* --- source handling --- */

static void chan_close_source(mr_chan *c)
{
    if (c->f) {
        mr_vfs_close(c->f);
        c->f = NULL;
    }
    free(c->wav);
    c->wav = NULL;
    c->wav_frames = 0;
    c->wav_pos = 0;
    c->in_len = 0;
    c->eof = 0;
    c->ring_frames = 0;
    c->ring_pos_fp = 0;
    c->is_push = 0;
    c->push_done = 0;
    c->push_draining = 0;
}

/* Ceiling on the PCM a single WAV may decode to. The buffer is malloc'd to
 * fit the file rather than being a fixed part of the channel, so nothing can
 * overflow here -- but the size comes from a length field *in the file*, and
 * an unbounded allocation driven by file contents is not something to leave
 * standing on a machine with 16MB of RAM.
 *
 * The game's WAVs are the rhythm game's hit sounds: 40858 bytes of 16-bit
 * mono, which expand 2x to 79KB of 16-bit stereo. 500KB is ~6x that, so a
 * legitimate sound has room to grow and a corrupt header does not get to ask
 * for 24MB. A file over the limit is truncated to it and logged, so it still
 * plays -- badly, but audibly, which is easier to diagnose than silence. */
#define MR_WAV_MAX_BYTES (500L * 1024L)

/* Minimal RIFF/WAVE reader: 8 or 16 bit PCM, mono or stereo. The game's
 * effects are all small, so these are decoded up front. */
static int load_wav(mr_chan *c, const char *path)
{
    unsigned char *raw;
    long size, pos;
    int channels = 0, bits = 0;
    long rate = 0;
    long data_off = 0, data_len = 0;

    raw = (unsigned char *)mr_vfs_read_all(path, &size);
    if (!raw)
        return 0;

    if (size < 44 || memcmp(raw, "RIFF", 4) || memcmp(raw + 8, "WAVE", 4)) {
        mr_log("audio: %s is not a RIFF WAVE", path);
        free(raw);
        return 0;
    }

    pos = 12;
    while (pos + 8 <= size) {
        /* Read unsigned and clamp to what is actually left in the file. A
         * chunk length is a 32-bit unsigned field, and `long` is 32 bits on
         * the target: read into a signed long, a length with the top bit set
         * comes out negative, `pos += 8 + clen` walks *backwards* and the
         * loop never ends. On a 64-bit host long is wide enough that the same
         * bytes stay positive, so this hangs only on Win32s -- which is why
         * it is bounded here rather than trusted. */
        mr_u32 raw_len = (mr_u32)raw[pos + 4] | ((mr_u32)raw[pos + 5] << 8) |
                         ((mr_u32)raw[pos + 6] << 16) |
                         ((mr_u32)raw[pos + 7] << 24);
        long clen = (long)(raw_len > (mr_u32)(size - pos - 8)
                           ? (mr_u32)(size - pos - 8) : raw_len);

        if (!memcmp(raw + pos, "fmt ", 4) && clen >= 16) {
            channels = raw[pos + 10] | (raw[pos + 11] << 8);
            rate = (long)raw[pos + 12] | ((long)raw[pos + 13] << 8) |
                   ((long)raw[pos + 14] << 16) | ((long)raw[pos + 15] << 24);
            bits = raw[pos + 22] | (raw[pos + 23] << 8);
        } else if (!memcmp(raw + pos, "data", 4)) {
            data_off = pos + 8;
            data_len = clen;
            if (data_off + data_len > size)
                data_len = size - data_off;
        }
        pos += 8 + clen + (clen & 1);
    }

    if (!data_len || (bits != 8 && bits != 16) ||
        (channels != 1 && channels != 2)) {
        mr_log("audio: %s has an unsupported format (%d bit, %d ch)",
               path, bits, channels);
        free(raw);
        return 0;
    }

    {
        long frames = data_len / (channels * (bits / 8));
        long max_frames = MR_WAV_MAX_BYTES / (long)(OUT_CH * sizeof(short));
        long i;
        short *out;

        if (frames > max_frames) {
            mr_log("audio: %s decodes to %ldKB, over the %ldKB limit; "
                   "keeping the first %ldms",
                   path, frames * (long)(OUT_CH * sizeof(short)) / 1024,
                   MR_WAV_MAX_BYTES / 1024,
                   max_frames * 1000L / (rate ? rate : OUT_RATE));
            frames = max_frames;
        }

        out = (short *)malloc((size_t)frames * OUT_CH * sizeof(short));
        if (!out) {
            free(raw);
            return 0;
        }

        for (i = 0; i < frames; i++) {
            short l, r;
            if (bits == 8) {
                const unsigned char *p = raw + data_off + i * channels;
                l = (short)((p[0] - 128) << 8);
                r = (channels == 2) ? (short)((p[1] - 128) << 8) : l;
            } else {
                const unsigned char *p = raw + data_off + i * channels * 2;
                l = (short)(p[0] | (p[1] << 8));
                r = (channels == 2) ? (short)(p[2] | (p[3] << 8)) : l;
            }
            out[i * 2] = l;
            out[i * 2 + 1] = r;
        }

        c->wav = out;
        c->wav_frames = frames;
        c->wav_pos = 0;
        c->src_rate = (int)(rate ? rate : OUT_RATE);
        c->duration_ms = frames * 1000L / c->src_rate;
    }

    free(raw);
    return 1;
}

static int chan_open(mr_chan *c, const char *path)
{
    const char *dot;

    chan_close_source(c);

    dot = strrchr(path, '.');
    c->is_mp3 = (dot && (dot[1] == 'm' || dot[1] == 'M'));
    c->out_frames = 0;
    c->start_mixed = g_mixed_frames;
    c->duration_ms = 0;
    c->xing_tried = 0;

    if (c->is_mp3) {
        c->f = mr_vfs_open(path);
        if (!c->f) {
            mr_log("audio: cannot open %s", path);
            return 0;
        }
        mp3dec_init(&c->mp3);
        c->src_rate = OUT_RATE;   /* corrected by the first decoded frame */
    } else {
        if (!load_wav(c, path))
            return 0;
    }

    strncpy(c->path, path, sizeof(c->path) - 1);
    c->path[sizeof(c->path) - 1] = '\0';
    return 1;
}

/* The length of an MP3, in milliseconds, taken from the Xing/Info tag that
 * sits inside its first frame. 0 when there is no such tag.
 *
 * `buf` is the head of the file, which may still have an ID3v2 tag on it. The
 * tag's first field is the number of audio frames, each 1152 samples for
 * MPEG1 Layer III, so the length follows exactly -- including for a
 * variable-bitrate file, which is the whole point: see the comment at the
 * fallback in chan_fill_ring.
 *
 * Only the header is read, and only from bytes the decoder has already
 * buffered, so this costs no I/O.
 */
static long xing_duration_ms(const unsigned char *buf, int len)
{
    static const int RATES[4] = { 44100, 48000, 32000, 0 };
    int    i, hz, mode, side, tag;
    long   frames;
    mr_u32 samples;

    /* Find the first frame. An ID3v2 tag could be declared and stepped over,
     * but scanning for the sync word also copes with a tag whose declared
     * size is wrong, and with junk ahead of one. */
    for (i = 0; i + 4 <= len; i++) {
        if (buf[i] != 0xFF || (buf[i + 1] & 0xE0) != 0xE0)
            continue;
        if (((buf[i + 1] >> 3) & 3) != 3 || ((buf[i + 1] >> 1) & 3) != 1)
            continue;                      /* not MPEG1 Layer III */
        hz = RATES[(buf[i + 2] >> 2) & 3];
        if (hz)
            break;
    }
    if (i + 4 > len)
        return 0;

    /* The tag lives after the side information, whose size depends on the
     * channel mode -- mode 3 is single-channel. */
    mode = (buf[i + 3] >> 6) & 3;
    side = (mode == 3) ? 17 : 32;

    tag = i + 4 + side;
    if (tag + 12 > len)
        return 0;
    if (memcmp(buf + tag, "Xing", 4) != 0 &&
        memcmp(buf + tag, "Info", 4) != 0)
        return 0;

    /* Four flag bytes, big-endian; bit 0 says the frame count is present.
     * Everything in a Xing tag is big-endian regardless of the host. */
    if (!(buf[tag + 7] & 1))
        return 0;

    frames = (long)(((mr_u32)buf[tag + 8] << 24) |
                    ((mr_u32)buf[tag + 9] << 16) |
                    ((mr_u32)buf[tag + 10] << 8) |
                     (mr_u32)buf[tag + 11]);

    /* 1,000,000 frames is about seven hours. The cap is not about plausible
     * track lengths but about `samples` below staying inside 31 bits. */
    if (frames <= 0 || frames > 1000000L)
        return 0;

    /* 1152 samples per MPEG1 Layer III frame.
     *
     * Deliberately not `samples * 1000 / hz`, which is the obvious form and
     * which overflows: a long is 32 bits on the target, and three minutes of
     * audio is already 9.2 billion. The first version of this did exactly
     * that, and because the host harness is built with gcc -- where a long is
     * 64 bits -- every test passed while the game wrapped. A 209s track
     * reported 15s, and a 191s one reported *minus* 4s, which the jukebox
     * read as "no duration" and left its bar at zero.
     *
     * So: whole seconds first, then the remainder. Both terms stay small --
     * the remainder is below hz, so it peaks at 48,000,000.
     *
     * In mr_u32 rather than long so that the host and the target agree on the
     * width. That is the only reason a harness can test this at all: with
     * long, the same expression means different things in the two builds, and
     * the one that matters is the one no test runs in. */
    samples = (mr_u32)frames * 1152u;
    return (long)((samples / (mr_u32)hz) * 1000u +
                  (samples % (mr_u32)hz) * 1000u / (mr_u32)hz);
}

/* Append up to `want` source frames to the ring. Returns 0 at end of stream. */
static int chan_fill_ring(mr_chan *c, long want)
{
    /* A pushed source has no input to pull from: what is in the ring is all
     * there is until the feeder runs again. Answering honestly here -- "no, I
     * could not reach `want`" -- is what sends chan_mix into its starve
     * handling, which has the push case at the top of it. */
    if (c->is_push)
        return c->ring_frames >= want;

    while (c->ring_frames < want) {

        if (!c->is_mp3) {
            long avail = c->wav_frames - c->wav_pos;
            long n = want - c->ring_frames;

            if (avail <= 0)
                return 0;
            if (n > avail)
                n = avail;
            if (c->ring_frames + n > RING_FRAMES)
                n = RING_FRAMES - c->ring_frames;
            if (n <= 0)
                break;

            memcpy(c->ring + c->ring_frames * OUT_CH,
                   c->wav + c->wav_pos * OUT_CH,
                   (size_t)n * OUT_CH * sizeof(short));
            c->ring_frames += n;
            c->wav_pos += n;
            continue;
        }

        /* MP3: top up the input window, then decode one frame. */
        if (c->in_len < 2048 && !c->eof) {
            long got = mr_vfs_read(c->f, c->in + c->in_len,
                                   (long)IN_WINDOW - c->in_len);
            if (got > 0)
                c->in_len += (int)got;
            else
                c->eof = 1;
        }

        if (c->in_len <= 0)
            return 0;

        /* Asked once, of the head of the file, before anything is decoded:
         * the Xing tag is inside the first frame, and by the time that frame
         * has been decoded it has been shifted out of the window. */
        if (!c->xing_tried) {
            c->xing_tried = 1;
            c->duration_ms = xing_duration_ms(c->in, c->in_len);
        }

        {
            mp3dec_frame_info_t info;
            short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
            int samples;

            memset(&info, 0, sizeof(info));
            samples = mp3dec_decode_frame(&c->mp3, c->in, c->in_len,
                                          pcm, &info);

            if (info.frame_bytes > 0) {
                memmove(c->in, c->in + info.frame_bytes,
                        (size_t)(c->in_len - info.frame_bytes));
                c->in_len -= info.frame_bytes;
            } else if (c->eof) {
                return 0;           /* trailing garbage at end of file */
            } else {
                continue;           /* needs more input */
            }

            if (samples <= 0)
                continue;           /* a skipped or ID3 frame */

            c->src_rate = info.hz ? info.hz : OUT_RATE;

            /* No Xing tag: fall back to the file size and the bitrate of the
             * first frame. That is exact for a constant-bitrate file, and off
             * by whatever an ID3 tag adds, a second at most.
             *
             * For a variable-bitrate file it is not even close -- a VBR
             * encoder emits a quiet, low-bitrate frame first, so this read
             * 400s for a 192s song. Hence xing_duration_ms above, which is
             * tried first; this is only reached for a file with no tag at
             * all. Every track this game ships has one. */
            if (!c->duration_ms && info.bitrate_kbps > 0)
                c->duration_ms = mr_vfs_size(c->f) * 8L / info.bitrate_kbps;

            if (c->ring_frames + samples > RING_FRAMES)
                samples = (int)(RING_FRAMES - c->ring_frames);
            if (samples <= 0)
                break;

            {
                short *dst = c->ring + c->ring_frames * OUT_CH;
                int i;
                if (info.channels == 2) {
                    memcpy(dst, pcm, (size_t)samples * 2 * sizeof(short));
                } else {
                    for (i = 0; i < samples; i++) {
                        dst[i * 2] = pcm[i];
                        dst[i * 2 + 1] = pcm[i];
                    }
                }
                c->ring_frames += samples;
            }
        }
    }
    return 1;
}

/* Drop `frames` consumed source frames off the front of the ring. */
static void ring_consume(mr_chan *c, long frames)
{
    if (frames <= 0)
        return;
    if (frames >= c->ring_frames) {
        c->ring_frames = 0;
        return;
    }
    memmove(c->ring, c->ring + frames * OUT_CH,
            (size_t)(c->ring_frames - frames) * OUT_CH * sizeof(short));
    c->ring_frames -= frames;
}

/* Restart the current file, for looping. */
static int chan_rewind(mr_chan *c)
{
    /* A pushed source cannot be rewound: there is no file here to seek, only
     * a ring somebody else fills. Unreachable today -- `loop` is 0 for a
     * pushed channel and the starve path takes the push branch before this --
     * and the guard is here so that it stays a refusal rather than becoming a
     * silent rewind of somebody else's decoder state if either changes. */
    if (c->is_push)
        return 0;

    c->out_frames = 0;
    c->start_mixed = g_mixed_frames;
    if (!c->is_mp3) {
        c->wav_pos = 0;
        return 1;
    }
    if (!c->f)
        return 0;
    if (mr_vfs_seek(c->f, 0, SEEK_SET) != 0)
        return 0;
    mp3dec_init(&c->mp3);
    c->in_len = 0;
    c->eof = 0;
    return 1;
}

/* Begin the track a `play X fadeout` left waiting, replacing whatever the
 * channel is on. Returns 0 and leaves the channel inactive if X will not
 * open -- the same answer mr_audio_play_ex gives for a bad file.
 *
 * Two callers, and the difference between them is only *why* the outgoing
 * track is finished: the fade reached zero, or the source ran out first. */
static int start_pending(mr_chan *c)
{
    char file[MR_PATH_MAX];
    int  loop = c->pending_loop;
    long fadein = c->pending_fadein;

    strcpy(file, c->pending);
    c->has_pending = 0;
    chan_close_source(c);
    if (!chan_open(c, file)) {
        c->active = 0;
        return 0;
    }
    c->loop = loop;
    c->has_queued = 0;
    c->ring_pos_fp = 0;
    set_fade(c, fadein > 0 ? 0 : 256, 256, fadein);
    c->active = 1;
    return 1;
}

/* --- mixing --- */

static void chan_mix(mr_chan *c, int ch_index)
{
    long need_src;
    long step_fp;
    long out;
    int  base_gain;

    if (!c->active)
        return;

    /* A channel's *mixer* is not always itself. MR_CH_MOVIE is registered
     * against the music mixer (script.h, quoting common/00mixers.rpy:56), so
     * a cutscene's sound follows the music slider and the music mute -- there
     * is no movie slider in the preferences screen and Ren'Py has none
     * either. Read through the mapping rather than copying the volume across
     * when a cutscene starts: the player can move the slider *during* a
     * cutscene, and a copy would ignore them until it ended.
     *
     * Note what this does not do: it does not stop the channel on mute.
     * `stop_on_mute=False` in that registration, so muting music during a
     * cutscene silences it and the cutscene keeps running to its end. Nothing
     * in this mixer force-stops on mute, so that is free -- but it is the
     * behaviour, not an accident of ours. */
    {
        int mixer = (ch_index == MR_CH_MOVIE) ? MR_CH_MUSIC : ch_index;

        base_gain = (int)(g_volume[mixer] * 256.0f);
        if (g_mute[mixer])
            base_gain = 0;
    }

    /* Unsigned, because the target's long is 32 bits and 44100<<16 is
     * 2,890,137,600 -- past LONG_MAX but comfortably inside ULONG_MAX. As a
     * signed expression this went negative and silenced the mixer. */
    step_fp = (long)(((mr_u32)c->src_rate << 16) / (mr_u32)OUT_RATE);

    /* Source frames this output buffer will consume, plus two so the linear
     * interpolation below always has a next sample to reach for. */
    need_src = ((step_fp * BUF_FRAMES) >> 16) + 2;
    if (need_src > RING_FRAMES)
        need_src = RING_FRAMES;

    if (!chan_fill_ring(c, need_src)) {
        if (c->is_push) {
            /* Under-fed, not finished. Fall through and mix whatever is
             * there; the loop below stops at ring_frames and leaves the rest
             * of the output buffer as it found it, which is silence.
             *
             * The channel only ends when the feeder has said the stream is
             * over *and* the ring has drained -- otherwise the tail of a
             * cutscene's audio is dropped, up to a whole runway of it.
             *
             * push_starved counts the silence so movie.c can say in the log
             * how badly the decoder fell behind. A cutscene that sounds
             * broken on the real machine and a cutscene that is merely late
             * are the same symptom through a speaker; the count tells them
             * apart without a second run. */
            if (c->push_done && c->ring_frames == 0) {
                /* Mixed, but not yet heard.
                 *
                 * The mixer runs a whole queue ahead of the speaker -- that
                 * is what the queue is for -- so at the instant the ring runs
                 * out, everything from here back to `g_queue_max` buffers ago
                 * is still in front of the card. During a cutscene that is
                 * about 417ms. Going inactive here therefore told movie.c the
                 * sound had finished while nearly half a second of it had not
                 * been played, and movie.c in turn ended the cutscene and let
                 * interp.c composite the stage over the last picture. The
                 * symptom was the last few frames and the last words of a
                 * cutscene missing -- reported as "maybe 0.5s" on an eleven
                 * second test video, which is the queue depth to the eye.
                 *
                 * So the end of a pushed stream is where the *device* gets to
                 * the last frame that was mixed, not where the mixer does.
                 * Until then the channel stays active and contributes
                 * silence, which is what the loop below does with an empty
                 * ring anyway.
                 *
                 * Not counted as starvation: nothing is late here, the stream
                 * is simply over.
                 *
                 * How far the card has to go is taken as a *difference* read
                 * at one instant -- what has been mixed, less what has been
                 * played -- and then counted down by the device's own
                 * progress. Comparing the two counters directly would have
                 * been shorter and wrong: they are only in the same epoch
                 * while the device stays open, and a reopen rebases one and
                 * not the other. mr_audio_output_position_ms has that
                 * assumption buried in it; this does not need it. */
                long played = mr_wave_played_frames();

                if (!c->push_draining) {
                    c->push_draining = 1;
                    c->push_drain_left = g_mixed_frames - played;
                    if (c->push_drain_left < 0)
                        c->push_drain_left = 0;
                    /* The card cannot be holding more than the runway, so
                     * neither can this wait, whatever the two counters say.
                     * Belt and braces against exactly the epoch problem
                     * above. */
                    if (c->push_drain_left > (long)NBUF * BUF_FRAMES)
                        c->push_drain_left = (long)NBUF * BUF_FRAMES;
                    c->push_drain_played = played;
                    c->push_drain_mixes = 0;
                } else {
                    long d = played - c->push_drain_played;

                    if (d < 0)
                        d = 0;              /* the device was reopened */
                    c->push_drain_played = played;
                    c->push_drain_left -= d;
                    c->push_drain_mixes++;
                }

                /* Bounded, because a hang here would be a cutscene that never
                 * ends. A sink that cannot report a position (-1) ends the
                 * channel as it always did, and one that stops advancing is
                 * given the whole runway and a second of slack before being
                 * given up on -- counted in mixed buffers, so this needs no
                 * clock. */
                if (played < 0 ||
                    c->push_drain_left <= 0 ||
                    c->push_drain_mixes >
                        (long)NBUF + OUT_RATE / BUF_FRAMES) {
                    c->active = 0;
                    c->push_draining = 0;
                    chan_close_source(c);
                    return;
                }
            }
            /* Late, not over: an empty ring before push_done is the decoder
             * failing to keep up. After it, it is the drain above. */
            if (c->ring_frames == 0 && !c->push_done)
                c->push_starved += BUF_FRAMES;
        } else if (c->loop && chan_rewind(c)) {
            chan_fill_ring(c, need_src);
        } else if (c->ring_frames == 0) {
            /* Finished. Start whatever was queued behind it. */
            if (c->has_pending) {
                /* A `play X fadeout` arrived while this track was in its
                 * last moments: X is waiting for the fade to finish, and
                 * the source ran out first. Start X here or it is lost --
                 * the fade-out clears c->loop, so a looping track that
                 * would have wrapped falls through to this branch too.
                 *
                 * Found by juketest against a twelve-second fixture track:
                 * Previous() pressed 295ms from the end left the channel
                 * dead and the jukebox silent. In a game it is a chapter
                 * break whose `play music ... fadeout 1.0` lands in the
                 * last second of the outgoing track -- rare, silent, and
                 * it looks like the new track failed to load.
                 *
                 * The fade state is left alone: start_pending sets the
                 * fade-in the pending track asked for, and stop_when_silent
                 * is cleared because there is no longer anything to fade
                 * out. */
                c->stop_when_silent = 0;
                if (!start_pending(c))
                    return;
                chan_fill_ring(c, need_src);
            } else if (c->has_queued) {
                c->has_queued = 0;
                if (chan_open(c, c->queued)) {
                    c->loop = 0;
                    c->gain = c->gain_target = 256;
                    chan_fill_ring(c, need_src);
                } else {
                    c->active = 0;
                    return;
                }
            } else {
                c->active = 0;
                chan_close_source(c);
                return;
            }
        }
    }

    for (out = 0; out < BUF_FRAMES; out++) {
        long src_frame = c->ring_pos_fp >> 16;
        long g;

        if (src_frame >= c->ring_frames)
            break;              /* starved: the rest of the buffer stays silent */

        /* Advance the fade. gain_acc carries the sub-step remainder so a slow
         * ramp does not quantise to nothing. */
        if (c->gain != c->gain_target) {
            c->gain_acc += c->gain_step;
            c->gain += (int)(c->gain_acc >> 16);
            c->gain_acc &= 0xFFFF;
            if ((c->gain_step > 0 && c->gain > c->gain_target) ||
                (c->gain_step < 0 && c->gain < c->gain_target))
                c->gain = c->gain_target;
        }

        g = (long)c->gain * base_gain / 256;

        {
            const short *s = c->ring + src_frame * OUT_CH;

            if (step_fp == 0x10000L) {
                /* Source is already at the output rate, which is true of
                 * every file here except the five 48kHz tracks. */
                g_mix[out * 2]     += (long)s[0] * g >> 8;
                g_mix[out * 2 + 1] += (long)s[1] * g >> 8;
            } else if (src_frame + 1 < c->ring_frames) {
                /* frac is reduced to 8 bits first: a full-scale sample
                 * difference times a 16-bit fraction overflows 32 bits, and
                 * 8 bits of interpolation precision is inaudible here. */
                long frac = (c->ring_pos_fp & 0xFFFF) >> 8;
                long l = s[0] + (((long)s[2] - s[0]) * frac >> 8);
                long r = s[1] + (((long)s[3] - s[1]) * frac >> 8);
                g_mix[out * 2]     += l * g >> 8;
                g_mix[out * 2 + 1] += r * g >> 8;
            } else {
                g_mix[out * 2]     += (long)s[0] * g >> 8;
                g_mix[out * 2 + 1] += (long)s[1] * g >> 8;
            }
        }

        c->ring_pos_fp += step_fp;
    }

    c->out_frames += out;

    /* Retire the source frames we read. */
    {
        long consumed = c->ring_pos_fp >> 16;
        ring_consume(c, consumed);
        c->ring_pos_fp &= 0xFFFF;
    }

    if (c->stop_when_silent && c->gain == 0 && c->gain_target == 0) {
        c->stop_when_silent = 0;
        if (c->has_pending) {
            /* The fade-out is done; this is where the track that asked for it
             * begins. */
            start_pending(c);
        } else {
            c->active = 0;
            chan_close_source(c);
        }
    }
}

static void fill_buffer(short *dst)
{
    int i;

    memset(g_mix, 0, sizeof(g_mix));

    for (i = 0; i < MR_CH_COUNT; i++)
        chan_mix(&g_ch[i], i);

    g_mixed_frames += BUF_FRAMES;

    for (i = 0; i < BUF_SAMPLES; i++) {
        long v = g_mix[i];
        if (v > 32767) v = 32767;
        else if (v < -32768) v = -32768;
        dst[i] = (short)v;
    }
}

/* --- device ---
 *
 * What is left of it here. Opening the card, owning the buffers and knowing
 * which of them have played is wave.h's, one implementation per target; this
 * end only decides *when* to fill one and how deep to let the queue get.
 */

int mr_audio_init(const char *game_dir)
{
    int i;

    (void)game_dir;

    for (i = 0; i < MR_CH_COUNT; i++) {
        memset(&g_ch[i], 0, sizeof(g_ch[i]));
        g_volume[i] = 1.0f;
        g_mute[i] = 0;
    }

    if (!mr_wave_open())
        return 0;

    /* The device's played-frame count starts at the first buffer ever
     * submitted (wave.h), so the mixer's count has to start with it or the
     * two are in different epochs. Everything that compares them -- the
     * cutscene clock in mr_audio_output_position_ms, the drain in chan_mix --
     * is then reading a difference between a count of this session and a
     * count of the last one. Only ever visible across a shutdown and a second
     * init, which no game does and a harness does freely. */
    g_mixed_frames = 0;

    g_started = 1;
    return 1;
}

void mr_audio_shutdown(void)
{
    int i;

    mr_wave_close();

    for (i = 0; i < MR_CH_COUNT; i++)
        chan_close_source(&g_ch[i]);

    g_started = 0;
}

void mr_audio_poll(void)
{
    /* Called from the frame loop, and on DOS also from mr_plat_service
     * during long operations -- a decode that outlasts the whole queue is
     * otherwise a gap at the speaker, which is what the runway was supposed
     * to prevent and cannot when the loop stops entirely.
     *
     * That second caller is what the guard is for. The service hook is
     * sprinkled through the decoder and the renderer, and if either is ever
     * reached from inside fill_buffer this would re-enter the mixer in the
     * middle of advancing a channel. It costs a compare and it removes the
     * need to be sure of that forever. */
    static int polling;
    int outstanding;

    if (!g_started || polling)
        return;
    polling = 1;

    /* How much is already on its way out. Nothing started now can be heard
     * before all of it has played, which is the whole of g_queue_max's
     * reason for existing. */
    outstanding = mr_wave_outstanding();

    while (outstanding < g_queue_max) {
        short *buf = mr_wave_claim();

        if (!buf)
            break;

        fill_buffer(buf);
        if (!mr_wave_submit(buf))
            break;
        outstanding++;
    }

    polling = 0;
}

/* How much audio may be queued ahead of the speaker, in milliseconds.
 *
 * This is the latency of anything started from now on: a sound handed to the
 * mixer goes into a buffer that plays only once everything already queued
 * has. At the default depth that is the whole runway, over a second -- far
 * longer than the rhythm game's own 300ms hit window, which is why its hit
 * sounds arrived audibly after the key.
 *
 * Shallower means an underrun if a frame overruns the whole queue, so this is
 * not something to set globally: it is for a screen that knows it renders
 * cheaply and cares about response. Rounded up to whole buffers, floored at
 * two so there is always one in hand while another is being filled.
 *
 * Lowering it needs no reset and causes no gap. The limit only withholds new
 * writes, so the queue simply drains to the new depth -- and the rhythm
 * game's five-second lead-in means that has long finished before the first
 * note arrives.
 */
void mr_audio_set_queue_ms(long ms)
{
    long per = (long)BUF_FRAMES * 1000L / OUT_RATE;
    int  n;

    if (ms <= 0) {
        g_queue_max = NBUF;            /* back to the deep default */
        return;
    }

    n = (int)((ms + per - 1) / per);
    if (n < 2)
        n = 2;
    if (n > NBUF)
        n = NBUF;

    if (n != g_queue_max)
        mr_log("audio: queue depth %d buffers (%ldms)", n, (long)n * per);
    g_queue_max = n;
}

long mr_audio_queue_ms(void)
{
    return (long)g_queue_max * (long)BUF_FRAMES * 1000L / OUT_RATE;
}

/* The deepest the queue can go, which is the whole runway. Exposed so that
 * callers clamping a configured latency against it do not have to write the
 * figure down: rhythm.c had 743 hardcoded and it silently stopped meaning
 * "the whole queue" the moment NBUF changed. */
long mr_audio_max_queue_ms(void)
{
    return (long)NBUF * (long)BUF_FRAMES * 1000L / OUT_RATE;
}

/* --- control --- */

static void set_fade(mr_chan *c, int from, int to, long ms)
{
    c->gain = from;
    c->gain_target = to;
    c->gain_acc = 0;

    if (ms <= 0) {
        c->gain = to;
        c->gain_step = 0;
        return;
    }
    /* Q16 gain units per output frame. */
    /* (OUT_RATE / 1000) * ms rather than (ms * OUT_RATE) / 1000: the latter
     * overflows 32 bits once ms passes about 48 seconds. */
    c->gain_step = (int)(((long)(to - from) << 16) /
                         ((long)(OUT_RATE / 1000) * ms + 1));
    if (c->gain_step == 0)
        c->gain_step = (to > from) ? 1 : -1;
}

void mr_audio_play(mr_channel ch, const char *file, int loop,
                   long fadein_ms, float volume)
{
    mr_audio_play_ex(ch, file, loop, fadein_ms, 0, volume, 0);
}

void mr_audio_play_ex(mr_channel ch, const char *file, int loop,
                      long fadein_ms, long fadeout_ms, float volume,
                      int if_changed)
{
    mr_chan *c;

    if (ch < 0 || ch >= MR_CH_COUNT || !file)
        return;
    c = &g_ch[ch];

    /* Ren'Py's if_changed, which is *off* by default: `play music` restarts a
     * track even if that track is already playing. Only the main menu asks
     * for it, so that returning to the menu does not restart music that is
     * already looping there. */
    /* The loop flag used to be part of this test, so a request to play the
     * track that is already playing fell through to a reopen whenever the
     * channel had been started without looping. That is a reopen of a file
     * that is already decoding fine, on every rollback -- and if it fails,
     * as it can on the target, the channel is left dead and the music is
     * gone until something else plays. if_changed means "already playing,
     * leave it"; what looping it should do is a property to adopt, not a
     * reason to start again. */
    if (if_changed && c->active && !strcmp(c->path, file)) {
        c->loop = loop;
        /* Unless it is on its way out: a `stop music fadeout` leaves the
         * channel active, playing, and marked to close once silent, and
         * asking for the same track again means cancel that. */
        if (c->stop_when_silent) {
            c->stop_when_silent = 0;
            c->has_pending = 0;
            c->has_queued = 0;
            set_fade(c, c->gain, 256, fadein_ms);
        }
        return;
    }

    /* With something playing and a fadeout asked for, the new track waits for
     * the old one to fade rather than cutting it off. Ren'Py's play() is
     * fadeout() followed by enqueue(), which is the same shape. */
    if (c->active && fadeout_ms > 0) {
        strncpy(c->pending, file, sizeof(c->pending) - 1);
        c->pending[sizeof(c->pending) - 1] = '\0';
        c->pending_loop = loop;
        c->pending_fadein = fadein_ms;
        c->has_pending = 1;

        c->loop = 0;
        c->has_queued = 0;
        set_fade(c, c->gain, 0, fadeout_ms);
        c->stop_when_silent = 1;

        if (volume > 0.0f && volume != 1.0f)
            g_volume[ch] = volume;
        return;
    }

    if (!chan_open(c, file)) {
        c->active = 0;
        return;
    }

    c->loop = loop;
    c->has_queued = 0;
    c->has_pending = 0;
    c->stop_when_silent = 0;
    c->ring_pos_fp = 0;
    set_fade(c, fadein_ms > 0 ? 0 : 256, 256, fadein_ms);
    c->active = 1;

    if (volume > 0.0f && volume != 1.0f)
        g_volume[ch] = volume;
}

/* --- pushed sources (movie.c) ---
 *
 * The mixer normally owns its input: it is handed a name, opens it through the
 * VFS and pulls. A cutscene cannot work that way. Its audio is MP2 packets
 * interleaved with the video in one program stream, so the thing that knows
 * how to get at it is the demuxer, and the demuxer is driven by the frame
 * loop at the pace the *picture* needs. So for this one channel the direction
 * is reversed and audio.c becomes a sink.
 *
 * The contract is the ring and nothing else. movie.c asks how much room there
 * is, decodes that much and no more, and writes it; this file resamples,
 * fades, applies the music mixer's volume and mixes it exactly as it does a
 * file. Everything below the ring is shared, which is the point -- a cutscene
 * is not a second audio path.
 */
void mr_audio_push_open(mr_channel ch, int rate, float volume)
{
    mr_chan *c;

    if (ch < 0 || ch >= MR_CH_COUNT)
        return;
    c = &g_ch[ch];

    chan_close_source(c);
    memset(c->path, 0, sizeof(c->path));
    memset(c->queued, 0, sizeof(c->queued));
    memset(c->pending, 0, sizeof(c->pending));
    c->has_queued = 0;
    c->has_pending = 0;
    c->is_mp3 = 0;
    c->is_push = 1;
    c->push_done = 0;
    c->push_starved = 0;
    c->push_draining = 0;
    c->loop = 0;
    c->ring_pos_fp = 0;
    c->out_frames = 0;
    c->xing_tried = 1;          /* there is no file to find a Xing tag in */
    c->start_mixed = g_mixed_frames;
    c->src_rate = rate > 0 ? rate : OUT_RATE;

    /* A cutscene's length is not a property of its audio here, unlike the
     * poster-frame stand-in where the mp3 *was* the record of it. movie.c has
     * the container's duration from the demuxer, so this stays 0 and
     * mr_audio_duration_ms answers honestly rather than guessing from a
     * stream it cannot see the end of. */
    c->duration_ms = 0;

    /* Full gain, no fade. A cutscene starts when it starts; Ren'Py's
     * movie_cutscene has no fadein parameter. */
    c->gain = 256;
    c->gain_target = 256;
    c->gain_step = 0;
    c->gain_acc = 0;
    c->stop_when_silent = 0;

    c->active = 1;

    if (volume > 0.0f && volume != 1.0f)
        g_volume[ch] = volume;
}

long mr_audio_push_space(mr_channel ch)
{
    mr_chan *c;

    if (ch < 0 || ch >= MR_CH_COUNT)
        return 0;
    c = &g_ch[ch];
    if (!c->active || !c->is_push)
        return 0;

    return RING_FRAMES - c->ring_frames;
}

long mr_audio_push(mr_channel ch, const short *pcm, long frames)
{
    mr_chan *c;
    long room;

    if (ch < 0 || ch >= MR_CH_COUNT || !pcm || frames <= 0)
        return 0;
    c = &g_ch[ch];
    if (!c->active || !c->is_push)
        return 0;

    /* Truncate rather than grow. The ring is a fixed part of the channel and
     * the caller is expected to have asked for the room first; taking what
     * fits and reporting it lets a caller that did not ask lose audio instead
     * of overrunning a buffer. */
    room = RING_FRAMES - c->ring_frames;
    if (frames > room)
        frames = room;
    if (frames <= 0)
        return 0;

    memcpy(c->ring + c->ring_frames * OUT_CH, pcm,
           (size_t)frames * OUT_CH * sizeof(short));
    c->ring_frames += frames;
    return frames;
}

void mr_audio_push_end(mr_channel ch)
{
    if (ch < 0 || ch >= MR_CH_COUNT)
        return;
    /* Not the same as stopping. What is already in the ring still plays out;
     * see the push case in chan_mix. */
    g_ch[ch].push_done = 1;
}

int mr_audio_active(mr_channel ch)
{
    if (ch < 0 || ch >= MR_CH_COUNT)
        return 0;
    return g_ch[ch].active;
}

long mr_audio_push_starved(mr_channel ch)
{
    if (ch < 0 || ch >= MR_CH_COUNT)
        return 0;
    return g_ch[ch].push_starved;
}

void mr_audio_queue(mr_channel ch, const char *file, float volume)
{
    mr_chan *c;

    (void)volume;
    if (ch < 0 || ch >= MR_CH_COUNT || !file)
        return;
    c = &g_ch[ch];

    if (!c->active) {
        mr_audio_play(ch, file, 0, 0, 1.0f);
        return;
    }
    strncpy(c->queued, file, sizeof(c->queued) - 1);
    c->queued[sizeof(c->queued) - 1] = '\0';
    c->has_queued = 1;
}

void mr_audio_stop(mr_channel ch, long fadeout_ms)
{
    mr_chan *c;

    if (ch < 0 || ch >= MR_CH_COUNT)
        return;
    c = &g_ch[ch];

    if (!c->active)
        return;

    c->loop = 0;
    c->has_queued = 0;
    c->has_pending = 0;

    if (fadeout_ms > 0) {
        set_fade(c, c->gain, 0, fadeout_ms);
        c->stop_when_silent = 1;
    } else {
        c->active = 0;
        chan_close_source(c);
    }
}

void mr_audio_set_volume(mr_channel ch, float volume)
{
    if (ch >= 0 && ch < MR_CH_COUNT) {
        if (volume < 0.0f) volume = 0.0f;
        if (volume > 1.0f) volume = 1.0f;
        g_volume[ch] = volume;
    }
}

float mr_audio_get_volume(mr_channel ch)
{
    return (ch >= 0 && ch < MR_CH_COUNT) ? g_volume[ch] : 0.0f;
}

/* What a channel is playing, so a save can restore it. */
void mr_audio_current(mr_channel ch, char *out, int outsz, int *loop)
{
    out[0] = '\0';
    if (loop)
        *loop = 0;
    if (ch < 0 || ch >= MR_CH_COUNT || !g_ch[ch].active)
        return;

    /* Mid `play music X fadeout 1.0`, the outgoing track is still the one
     * playing but X is what the channel *is* -- the fade is how it gets
     * there. A save or a checkpoint taken in that second has to record X,
     * and looping as X asked for: the outgoing track's own loop flag was
     * cleared by the fadeout, so reporting it would store "not looping" for
     * a track that loops. */
    if (g_ch[ch].has_pending) {
        strncpy(out, g_ch[ch].pending, (size_t)outsz - 1);
        out[outsz - 1] = '\0';
        if (loop)
            *loop = g_ch[ch].pending_loop;
        return;
    }

    /* And mid `stop music fadeout 1.0`, with nothing waiting behind it, the
     * channel *is* stopped -- the fade is only how it gets there, exactly as
     * above. A say statement right after the stop takes its checkpoint while
     * the outgoing track is still audible, and recording that track there
     * means a rollback to that line brings back music the script had
     * stopped. Reported in a real save: roll back to just after
     * `stop music fadeout 1.0` and the previous track resumes.
     *
     * Below the has_pending branch, which sets stop_when_silent too: a
     * fadeout into another track is reported as that track, not as silence.
     */
    if (g_ch[ch].stop_when_silent)
        return;

    strncpy(out, g_ch[ch].path, (size_t)outsz - 1);
    out[outsz - 1] = '\0';
    if (loop)
        *loop = g_ch[ch].loop;
}

/* How far into the current track the channel is, and how long that track is.
 * Both in milliseconds; 0 when nothing is playing.
 *
 * The position counts frames handed to the driver, so it runs ahead of what
 * is coming out of the speakers by however much is still queued -- under a
 * second, and the only thing that reads it is the jukebox's position bar. */
long mr_audio_position_ms(mr_channel ch)
{
    if (ch < 0 || ch >= MR_CH_COUNT || !g_ch[ch].active)
        return 0;
    return g_ch[ch].out_frames / (OUT_RATE / 1000);
}

long mr_audio_latency_ms(void)
{
    long played, queued;

    if (!g_started)
        return 0;

    played = mr_wave_played_frames();
    if (played < 0)
        return 0;                       /* the device will not say */

    queued = g_mixed_frames - played;
    if (queued < 0)
        queued = 0;
    return queued / (OUT_RATE / 1000);
}

long mr_audio_output_position_ms(mr_channel ch)
{
    long played, pos;

    if (ch < 0 || ch >= MR_CH_COUNT || !g_ch[ch].active || !g_started)
        return 0;

    /* What the card has actually played, which trails what the mixer has
     * produced by whatever is still queued. The sink counts monotonically
     * for the whole session, so there is nothing to rebase. */
    played = mr_wave_played_frames();
    if (played < 0)
        return mr_audio_position_ms(ch);   /* no better answer available */

    pos = played - g_ch[ch].start_mixed;
    if (pos < 0)
        pos = 0;
    return pos / (OUT_RATE / 1000);
}

long mr_audio_duration_ms(mr_channel ch)
{
    if (ch < 0 || ch >= MR_CH_COUNT || !g_ch[ch].active)
        return 0;
    return g_ch[ch].duration_ms;
}

void mr_audio_set_mute(mr_channel ch, int mute)
{
    if (ch >= 0 && ch < MR_CH_COUNT)
        g_mute[ch] = mute;
}

int mr_audio_muted(mr_channel ch)
{
    return (ch >= 0 && ch < MR_CH_COUNT) ? g_mute[ch] : 0;
}

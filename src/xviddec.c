/* xviddec.c - xvidcore and minimp3 behind xviddec.h. See that header.
 *
 * --- one demuxer, two consumers, and why there are two queues --------------
 *
 * avi.c hands out packets in stored order, one at a time, into a buffer it
 * reuses. The two decoders above it are pulled independently and at different
 * rates: movie.c calls the audio side first and keeps calling it until the
 * mixer's ring is full (audio.h's contract -- ask for the room, then fill
 * it), and only then calls the video side once or a few times.
 *
 * So a demand for audio walks straight past video packets, and a demand for
 * video walks past audio packets, and neither may throw away what it walked
 * past. Filling a 400ms ring is about ten MP3 frames, which in an
 * ffmpeg-interleaved file means roughly ten video chunks passed over on the
 * way. Hence: one pump that reads a packet and files it, and a queue on each
 * side that the pump feeds and the consumer drains. pl_mpeg does the same
 * thing internally for the same reason; here it is visible because the
 * container is ours.
 *
 * The video queue keeps packet boundaries, because an MPEG-4 VOP is only
 * decodable as a unit. The audio queue does not, because MP3 is self-framing
 * and minimp3 resynchronises from anywhere in a byte stream -- so that side is
 * a sliding window compacted as frames are consumed, exactly as audio.c's MP3
 * path does for a music track.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mr.h"
#include "xviddec.h"
#include "avi.h"
#include "yuv.h"

#include "xvid.h"
#include "minimp3.h"

/* xvidcore is a __cdecl library and the engine is not.
 *
 * Makefile builds the engine `-6r` -- Watcom's register calling convention,
 * which decorates a name with a *trailing* underscore. xvidcore's
 * Makefile.watcom builds with `-ecc`, forcing __cdecl, which decorates with a
 * *leading* one. So this translation unit called `xvid_global_` and the
 * library offered `_xvid_global`, and the symptom was two undefined
 * references at link time with the names differing only in where the
 * underscore sat -- which is easy to read as a missing library rather than as
 * a convention mismatch, especially if the library happens to be missing too.
 *
 * The `-ecc` on xvidcore's side is not an accident to be fixed by matching
 * it: that library's C calls into nasm, and those .asm files pass arguments on
 * the stack. Rebuilding xvidcore `-6r` would fix this link and break every
 * C-to-assembly call inside it, which is a far quieter failure. The library is
 * correctly __cdecl; the caller has to say so.
 *
 * Applied to the two entry points this file uses rather than by compiling the
 * whole file `-ecc`, which would make mr_xvid_* __cdecl as well and move the
 * same mismatch to viddec.c.
 *
 * Nothing is needed for the gcc harness build: there is one calling
 * convention on the host and no name decoration to disagree about. */
#if defined(__WATCOMC__)
#pragma aux (__cdecl) xvid_global;
#pragma aux (__cdecl) xvid_decore;
#endif

/* WAVEFORMATEX.wFormatTag for MPEG Layer III. The one audio format minimp3
 * decodes, and so the one this path accepts. */
#define WAVE_FORMAT_MP3  0x0055

/* How many video packets may be queued while the audio side runs ahead.
 *
 * Filling a 400ms mixer ring takes about ten MP3 frames, and an
 * ffmpeg-interleaved AVI puts roughly one video chunk between each, so ten is
 * the working figure and this is twice it. Overflow is not a crash: the pump
 * stops reading, which stalls the audio side early, and the ring is then
 * topped up on the next update after the video side has drained. That is a
 * worse-shaped catch-up than it could be, never a lost packet. */
#define VQ_PACKETS  24

/* Bytes of video packet held across those slots. A 640x320 key frame at the
 * quality a cutscene is encoded at is tens of kilobytes and a P frame is a
 * few; this is grown on demand from a quarter of it rather than allocated at
 * this size. */
#define VQ_BYTES_MAX  (1024L * 1024L)
#define VQ_BYTES_START  (96L * 1024L)

/* The MP3 input window. minimp3 needs a whole frame in hand to decode one,
 * and a 320kbps frame is 1044 bytes; this holds a comfortable handful, so the
 * window is topped up from whole AVI chunks without ever straddling awkwardly.
 * Same role as audio.c's IN_WINDOW. */
#define AQ_BYTES  (16L * 1024L)

struct mr_xvid {
    mr_avi      *avi;
    mr_avi_info  info;

    void        *dec;           /* xvidcore decore handle */
    int          ended;         /* the demuxer has run out of packets */

    /* --- video packet queue, boundaries kept --- */
    unsigned char *vbuf;
    long           vcap;
    long           vhead, vtail;        /* live bytes are [vhead, vtail) */
    long           vlen[VQ_PACKETS];    /* packet lengths, in order */
    int            vfirst, vn;          /* ring over vlen */

    /* --- MP3 input window, a sliding byte stream --- */
    unsigned char  abuf[AQ_BYTES];
    long           alen;

    mp3dec_t       mp3;
    int            has_mp3;

    /* Output bookkeeping. g_frames counts pictures produced, which with no
     * B-frames is also the display position, and so the timestamp. */
    long           frames;

    /* The planes of the last picture, for a chunk that says "same again".
     * They point into xvidcore's reference buffers and stay valid for exactly
     * as long as the contract in xviddec.h promises the caller's do. */
    mr_yuv_frame   last;
    int            have_last;
};

/* xvidcore's one-time global init, and the SIMD switch that has to precede
 * it. Process-wide because xvidcore's is: it latches detected cpu features
 * into function pointer tables, and a second call would redo that work for no
 * reason. A cutscene is opened and closed many times in a run. */
static int g_global_done;
static int g_simd = 1;

void mr_xvid_set_simd(int on)
{
    /* Deliberately does not reset g_global_done. Changing the setting after
     * the first movie has played would not take effect, and pretending
     * otherwise by re-initialising would leave the two halves of xvidcore's
     * tables disagreeing about which build they came from. The knob is read
     * once per movie by movie.c and is in practice set in mrpy.ini before the
     * process starts, so the only honest behaviour is first-one-wins. */
    g_simd = on;
}

/* What xvidcore decided its kernels are, as a short string for the log.
 *
 * This exists because "is the SIMD actually running" is otherwise
 * unanswerable on the machine, and the two things it distinguishes look
 * identical from a chair: a decoder that is slow because the picture is big,
 * and a decoder that is slow because it silently fell back to its C paths.
 * The DOS build genuinely is pure C (its library is assembled without the
 * .asm files at all), the Win32 build genuinely is not, and before this line
 * existed the only way to tell which you had was to compare frame times
 * against a guess.
 *
 * XVID_GBL_INFO reports what the one-time init detected, so it is the
 * library's own answer and not ours. Read after global_init, never before. */
static const char *simd_name(void)
{
    xvid_gbl_info_t info;

    memset(&info, 0, sizeof(info));
    info.version = XVID_VERSION;

    if (xvid_global(NULL, XVID_GBL_INFO, &info, NULL) < 0)
        return "?";

    /* Reported from the most capable flag down, and only the ones a Pentium
     * III can have -- anything above MMXEXT is in the library because xvid.c
     * takes the addresses unconditionally, not because this target reaches
     * it. "none" is the interesting answer: it means the C fallbacks, either
     * because simd=0 was asked for or because the build has no assembly. */
    if (info.cpu_flags & XVID_CPU_SSE2)   return "sse2";
    if (info.cpu_flags & XVID_CPU_SSE)    return "sse";
    if (info.cpu_flags & XVID_CPU_MMXEXT) return "mmxext";
    if (info.cpu_flags & XVID_CPU_MMX)    return "mmx";
    return "none";
}

const char *mr_xvid_simd_name(void)
{
    return g_global_done ? simd_name() : "?";
}

static int global_init(void)
{
    xvid_gbl_init_t gi;

    if (g_global_done)
        return 1;

    memset(&gi, 0, sizeof(gi));
    gi.version = XVID_VERSION;
    /* 0 means autodetect. XVID_CPU_FORCE with no feature bits means "this
     * machine has nothing", which is how the pure C paths are selected --
     * xvid.c reads it exactly that way, so `simd=0` here is the same switch
     * mr_yuv_set_simd(0) is for our own kernel. */
    gi.cpu_flags = g_simd ? 0 : XVID_CPU_FORCE;

    if (xvid_global(NULL, XVID_GBL_INIT, &gi, NULL) < 0)
        return 0;

    g_global_done = 1;
    return 1;
}

/* --- the video queue -----------------------------------------------------*/

static int vq_room(mr_xvid *x, long bytes)
{
    unsigned char *nb;
    long cap;

    if (x->vn >= VQ_PACKETS)
        return 0;

    /* Everything already consumed moves out of the way first. The usual case
     * is that the queue is empty and this is a reset to zero, not a move. */
    if (x->vtail + bytes > x->vcap && x->vhead > 0) {
        memmove(x->vbuf, x->vbuf + x->vhead, (size_t)(x->vtail - x->vhead));
        x->vtail -= x->vhead;
        x->vhead = 0;
    }

    if (x->vtail + bytes <= x->vcap)
        return 1;

    if (x->vtail + bytes > VQ_BYTES_MAX)
        return 0;

    cap = x->vcap ? x->vcap : VQ_BYTES_START;
    while (cap < x->vtail + bytes)
        cap *= 2;
    if (cap > VQ_BYTES_MAX)
        cap = VQ_BYTES_MAX;

    nb = (unsigned char *)realloc(x->vbuf, (size_t)cap);
    if (!nb)
        return 0;
    x->vbuf = nb;
    x->vcap = cap;
    return 1;
}

static int vq_push(mr_xvid *x, const unsigned char *d, long n)
{
    if (!vq_room(x, n > 0 ? n : 1))
        return 0;

    if (n > 0)
        memcpy(x->vbuf + x->vtail, d, (size_t)n);
    x->vtail += n;
    x->vlen[(x->vfirst + x->vn) % VQ_PACKETS] = n;
    x->vn++;
    return 1;
}

static int vq_pop(mr_xvid *x, const unsigned char **d, long *n)
{
    if (x->vn <= 0)
        return 0;

    *n = x->vlen[x->vfirst];
    *d = x->vbuf + x->vhead;
    x->vhead += *n;
    x->vfirst = (x->vfirst + 1) % VQ_PACKETS;
    x->vn--;

    if (x->vn == 0) {
        /* Empty: start the next packet at the front rather than letting the
         * offsets crawl up to the cap and force a compaction. */
        x->vhead = 0;
        x->vtail = 0;
    }
    return 1;
}

/* --- the pump ------------------------------------------------------------
 *
 * One packet from the container into whichever queue it belongs to. Returns 1
 * if something was filed, 0 if the stream ended or there was no room.
 *
 * A video packet that will not fit must not be dropped, so the pump refuses
 * *before* reading rather than after: the packet stays in the container to be
 * read again next time, because mr_avi_next has not been called yet. Once it
 * has, there is nowhere to put the bytes and the picture is gone.
 *
 * Which is what the first version of this did, and it cost three pictures out
 * of fifty-one on the fixture. The refusal was conditional on *both* queues
 * being full -- so with the video queue at its limit and the audio window
 * with room in it, the pump read on, found a video chunk, and vq_push
 * silently refused it. The symptom is not a short tail, either: a dropped P
 * frame corrupts every picture up to the next key frame, and the frame
 * counter is the timestamp, so everything after the loss also moves 40ms
 * earlier and the sound ends after the vision. The host shim's waveOut
 * consumes buffers the instant they are queued, which makes an audio demand
 * run to the end of the file, which is exactly the shape that provokes it;
 * claim 8 in avitest.c is that shape, and goes red if this reverts.
 *
 * The audio window needs no such care. It is filled only to a frame's worth
 * (mr_xvid_decode_audio tops up to 2048 bytes and stops), and a demand for
 * video that passes an audio chunk is the only thing that can grow it
 * further, at a few hundred bytes per picture against a 16KB window. If it
 * ever did fill, truncating the chunk is a glitch in the sound; refusing to
 * read would be a false end of stream and lose the rest of the film.
 */
static int pump(mr_xvid *x)
{
    const unsigned char *d;
    long n;
    int kind;

    if (x->ended)
        return 0;

    /* The video queue is the one that cannot afford a loss. Never true when
     * the video side asks -- it only pumps with the queue empty -- so this is
     * the audio side being told to make do with what it has and come back
     * after the next decode has drained a slot. */
    if (x->vn >= VQ_PACKETS)
        return 0;

    kind = mr_avi_next(x->avi, &d, &n);
    if (!kind) {
        x->ended = 1;
        return 0;
    }

    if (kind == MR_AVI_VIDEO) {
        if (vq_push(x, d, n))
            return 1;

        /* A slot was free, so this is the byte cap: VQ_BYTES_MAX of queued
         * packets, or a failed realloc. Nothing can be done about the packet
         * now, but it must not be silent -- a picture has been lost, and the
         * corruption it causes shows up nowhere near here. */
        mr_log("xvid: video queue full at %d packets, lost a %ld-byte packet",
               x->vn, n);
        return 0;
    }

    if (kind == MR_AVI_AUDIO && x->has_mp3) {
        if (n > AQ_BYTES - x->alen)
            n = AQ_BYTES - x->alen;     /* see the refusal above; should not bite */
        if (n > 0) {
            memcpy(x->abuf + x->alen, d, (size_t)n);
            x->alen += n;
        }
        return 1;
    }

    /* An audio packet with no MP3 decoder behind it -- a PCM or AC3 track,
     * which mr_xvid_open reported as "no audio". Discarded, and the caller is
     * told something happened so the pump loop makes progress. */
    return 1;
}

/* --- open and close ------------------------------------------------------*/

mr_xvid *mr_xvid_open(const mr_avi_io *io, mr_avi_info *out)
{
    mr_xvid *x;
    xvid_dec_create_t dc;

    if (!io)
        return NULL;

    if (!global_init())
        return NULL;

    x = (mr_xvid *)calloc(1, sizeof(*x));
    if (!x)
        return NULL;

    x->avi = mr_avi_open(io, &x->info);
    if (!x->avi) {
        free(x);
        return NULL;
    }

    memset(&dc, 0, sizeof(dc));
    dc.version = XVID_VERSION;
    dc.width   = x->info.width;
    dc.height  = x->info.height;
    /* The container's fourcc, handed over rather than left for the bitstream
     * to declare. xvidcore uses it to recognise the DivX and XVID variants
     * that need a quirk -- it is an [in:opt] field precisely so a caller that
     * read it from a container can say so, and we did. */
    dc.fourcc  = (int)x->info.fourcc;

    if (xvid_decore(NULL, XVID_DEC_CREATE, &dc, NULL) < 0) {
        mr_avi_close(x->avi);
        free(x);
        return NULL;
    }
    x->dec = dc.handle;

    if (x->info.has_audio && x->info.audio_format == WAVE_FORMAT_MP3) {
        mp3dec_init(&x->mp3);
        x->has_mp3 = 1;
    } else {
        /* Reported as silent: see xviddec.h. The picture is the point and a
         * silent movie has a working clock. */
        x->info.has_audio = 0;
    }

    if (out)
        *out = x->info;
    return x;
}

void mr_xvid_close(mr_xvid *x)
{
    if (!x)
        return;

    if (x->dec)
        xvid_decore(x->dec, XVID_DEC_DESTROY, NULL, NULL);
    mr_avi_close(x->avi);
    free(x->vbuf);
    free(x);
}

/* --- video --------------------------------------------------------------*/

/* One timestamp from one frame number. In long arithmetic against the
 * container's own rate, so that a 25fps file gives exactly 0, 40, 80 rather
 * than the accumulation of a rounded 40. */
static long pts_of(const mr_xvid *x, long frame)
{
    if (x->info.fps <= 0.0)
        return frame * 40L;
    return (long)((double)frame * 1000.0 / x->info.fps + 0.5);
}

/* Hand one packet to xvidcore and keep going until it produces a picture or
 * runs out of bytes.
 *
 * The loop exists for the VOL headers. The first chunk of the stream, and the
 * first chunk after each key frame in some muxings, carries the visual object
 * sequence and object layer headers ahead of the first VOP. xvidcore reports
 * those as XVID_TYPE_VOL, consumes only them, and returns -- so a caller that
 * treats one call as one frame loses a picture per key frame and, at the
 * start, never gets a first frame at all.
 */
static int decode_packet(mr_xvid *x, const unsigned char *d, long n,
                         mr_yuv_frame *out)
{
    long off = 0;
    int guard;

    for (guard = 0; guard < 8 && off < n; guard++) {
        xvid_dec_frame_t fr;
        xvid_dec_stats_t st;
        int used;

        memset(&fr, 0, sizeof(fr));
        memset(&st, 0, sizeof(st));
        fr.version = XVID_VERSION;
        st.version = XVID_VERSION;

        /* No post-processing. XVID_DEBLOCKY and friends are quality for cpu,
         * and this exercise is the other way round: the whole reason for a
         * second codec is that the target has no cpu to spare. XVID_LOWDELAY
         * says there are no B-frames to wait for, which is true of what the
         * encoding recipe in avitest.c produces and makes output order equal
         * stored order -- which is what makes the frame counter a timestamp. */
        fr.general = XVID_LOWDELAY;

        fr.bitstream = (void *)(d + off);
        fr.length    = (int)(n - off);

        fr.output.csp = XVID_CSP_INTERNAL;
        /* Non-NULL and >= width or nothing is written at all. See xviddec.h;
         * this is the one piece of xvidcore's interface that does not do what
         * it looks like. */
        fr.output.plane[0]  = (void *)x;
        fr.output.stride[0] = x->info.width;

        used = xvid_decore(x->dec, XVID_DEC_DECODE, &fr, &st);
        if (used < 0)
            return 0;               /* a corrupt packet; the caller moves on */

        if (st.type > 0) {
            if (!fr.output.plane[0])
                return 0;
            out->w = x->info.width;
            out->h = x->info.height;
            out->y  = (const mr_u8 *)fr.output.plane[0];
            out->cb = (const mr_u8 *)fr.output.plane[1];
            out->cr = (const mr_u8 *)fr.output.plane[2];
            out->y_pitch = fr.output.stride[0];
            out->c_pitch = fr.output.stride[1];
            return 1;
        }

        if (used <= 0)
            return 0;               /* no progress; do not spin on it */
        off += used;
    }

    return 0;
}

int mr_xvid_decode_video(mr_xvid *x, mr_yuv_frame *out, long *pts_ms)
{
    const unsigned char *d;
    long n;

    if (!x || !out)
        return 0;

    for (;;) {
        while (x->vn <= 0) {
            if (!pump(x))
                return 0;           /* end of stream, or wedged */
        }

        if (!vq_pop(x, &d, &n))
            return 0;

        /* "Same frame again."
         *
         * AVI says it with a zero-length chunk; a VfW encoder says it with a
         * single 0x7f byte. Both are a real frame as far as the clock is
         * concerned -- time passes and the picture does not change -- so this
         * counts a frame and returns the planes already in hand rather than
         * decoding. xvidcore handles the 0x7f case itself (decoder.c:1609),
         * but only when it has a reference, and it cannot handle a length of
         * zero at all. Doing it here makes the two spellings behave alike.
         *
         * Before the first picture there is nothing to repeat, so such a
         * chunk is skipped outright. */
        if (n < 2) {
            if (!x->have_last)
                continue;
            *out = x->last;
            if (pts_ms)
                *pts_ms = pts_of(x, x->frames);
            x->frames++;
            return 1;
        }

        if (decode_packet(x, d, n, out)) {
            x->last = *out;
            x->have_last = 1;
            if (pts_ms)
                *pts_ms = pts_of(x, x->frames);
            x->frames++;
            return 1;
        }

        /* A packet that produced nothing: a corrupt VOP, or headers with no
         * picture behind them. Not a frame, so nothing is counted and the
         * timestamps stay in step with the pictures that did arrive. Round
         * again for the next packet. */
    }
}

/* --- audio --------------------------------------------------------------*/

int mr_xvid_decode_audio(mr_xvid *x, short *pcm, int max_frames)
{
    if (!x || !pcm || !x->has_mp3 || max_frames < MR_XVID_AUDIO_FRAME)
        return 0;

    for (;;) {
        mp3dec_frame_info_t info;
        short tmp[MINIMP3_MAX_SAMPLES_PER_FRAME];
        int samples;

        /* Top the window up before each attempt. A whole MP3 frame has to be
         * in hand or minimp3 reports it needs more, and the AVI chunking does
         * not align with frame boundaries in general. */
        while (x->alen < 2048 && !x->ended) {
            if (!pump(x))
                break;
        }

        if (x->alen <= 0)
            return 0;

        memset(&info, 0, sizeof(info));
        samples = mp3dec_decode_frame(&x->mp3, x->abuf, (int)x->alen,
                                      tmp, &info);

        if (info.frame_bytes > 0) {
            memmove(x->abuf, x->abuf + info.frame_bytes,
                    (size_t)(x->alen - info.frame_bytes));
            x->alen -= info.frame_bytes;
        } else if (x->ended) {
            /* Nothing recognisable left and nothing more coming: trailing
             * bytes after the last frame. */
            x->alen = 0;
            return 0;
        } else {
            /* Needs more input, and the pump above could not supply it this
             * time round. Not an error -- the caller asks again next update
             * -- and returning rather than looping is what keeps a demand for
             * audio from draining the whole file. */
            return 0;
        }

        if (samples <= 0)
            continue;               /* a skipped frame, or an ID3 tag */

        /* `samples` is per channel, as minimp3 documents it. */
        if (samples > max_frames)
            samples = max_frames;

        if (info.channels >= 2) {
            memcpy(pcm, tmp, (size_t)samples * 2 * sizeof(short));
        } else {
            int i;

            /* Mono duplicated to stereo here, so the mixer never sees a
             * channel count -- the same division of labour plmdec.h sets out.
             * Walked backwards so it would be safe in place; it is not in
             * place, but writing it the other way is a bug waiting for
             * somebody to remove the temporary. */
            for (i = samples - 1; i >= 0; i--) {
                pcm[i * 2]     = tmp[i];
                pcm[i * 2 + 1] = tmp[i];
            }
        }

        return samples;
    }
}

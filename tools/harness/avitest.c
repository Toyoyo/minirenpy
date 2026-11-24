/* avi.c and xviddec.c: an AVI out of the archive, demuxed and decoded.
 *
 * The second cutscene codec. movietest covers the MPEG-1 path end to end
 * through movie.c; this covers the two files underneath the new one -- the
 * container walk and the xvidcore/minimp3 glue -- because that is where the
 * novel code is. movie.c itself is unchanged by the addition: it asks
 * viddec.h for a frame and a timestamp and does not know which decoder
 * produced them, so re-testing its clock here would re-test movietest.
 *
 * --- what this can and cannot establish ----------------------------------
 *
 * It runs the real decoder. xvidcore builds pure C -- see mkxvidhost.sh --
 * so every claim below is executed here and not reasoned about.
 *
 * What it cannot establish is the *reason the codec was changed*, which is
 * decode cost on a Pentium III with MMX. xvidcore's MMX kernels are nasm and
 * win32-COFF and nothing on this host assembles or runs them; the library
 * this links is the C fallback. So the timing line printed at the end is a
 * host figure for the C path, useful as a ratio against pl_mpeg measured the
 * same way and worthless as a prediction of the target. CLAUDE.md: "say which
 * claims depend on it". The target figure comes from the log line movie.c
 * writes at the end of a cutscene, on the machine.
 *
 * Nor does it establish anything about A/V sync, for the reason movietest
 * gives at length: the host waveOut shim consumes buffers the instant they
 * are queued, so the movie clock is fake here. No mixer is linked at all.
 *
 * --- the fixture ---------------------------------------------------------
 *
 * fixgame/audio/fixmovie.avi: 64x32, 25fps, two seconds, MPEG-4 Simple
 * Profile with a 440Hz MP3 soundtrack. 44KB. Committed rather than generated,
 * like fixmovie.mpg beside it, because generating it needs ffmpeg and the
 * suite must run without it. Regenerate with:
 *
 *   ffmpeg -f lavfi -i "testsrc=size=64x32:rate=25:duration=2,hue=H=6.283*t" \
 *          -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=2" \
 *          -c:v libxvid -q:v 4 -bf 0 -g 12 -vtag XVID \
 *          -c:a libmp3lame -b:a 64k -ar 44100 -ac 2 \
 *          tools/harness/fixgame/audio/fixmovie.avi
 *
 * --- which of those flags are load-bearing ---------------------------------
 *
 * Measured by driving avi.c and xviddec.c over deliberately varied encodes,
 * not inferred:
 *
 *   -c:v libxvid    required. It is the codec.
 *   -c:a libmp3lame required, and more narrowly than it looks: xviddec.c
 *                   accepts WAVEFORMATEX tag 0x0055 and nothing else, so PCM
 *                   or AC3 in the AVI opens as a **silent movie** rather than
 *                   failing. And mp3impl.c defines MINIMP3_ONLY_MP3, so
 *                   Layer III only -- `-c:a mp2` in an AVI will not play even
 *                   though the MPEG-1 path decodes MP2 happily.
 *   -bf 0           **required, and it fails silently without it.** See below.
 *
 *   -vtag XVID      cosmetic. Nothing validates it: avi.c reads biCompression
 *                   and xviddec.c passes it to XVID_DEC_CREATE as the
 *                   [in:opt] hint it is, so xvidcore can apply per-variant
 *                   quirks. A DX50-tagged file decodes identically; checked.
 *   -ar 44100       taste. The rate is read from the stream header and handed
 *                   to mr_audio_push_open, which resamples as it does for a
 *                   48kHz music track. A 22050Hz file plays; checked. 44100
 *                   is merely free, being the mixer's own rate.
 *   -ac 2           taste. Mono is duplicated to stereo in
 *                   mr_xvid_decode_audio, so the mixer never sees a channel
 *                   count. A mono file plays; checked.
 *   -b:a 128k       taste. Size against quality.
 *   -q:v 5          taste. Size against quality, and it does move decode cost
 *                   a little -- at matched bitrate xvidcore measured 0.680ms
 *                   a frame on the host against 0.463ms at -q:v 5.
 *   -g 125          taste. It is the key-frame interval, and playback never
 *                   seeks: a cutscene runs from the first frame to the last
 *                   and a loop reopens the file (movie.h on mr_movie_replay).
 *                   So this only trades file size against how much damage a
 *                   corrupt packet does. The fixture uses 12 and the game's
 *                   encode uses 125; both are fine.
 *
 * --- why -bf 0 is the one that matters -------------------------------------
 *
 * Two reasons, and the second is the dangerous one.
 *
 * Cost: MPEG-4 ASP with B-frames, quarter-pel motion and global motion
 * compensation is a *more* expensive format to decode than the MPEG-1 it
 * replaces. Swapping codecs to make decoding cheaper only works if the stream
 * stays inside what makes it cheaper. `-bf 2` is enough to make ffmpeg label
 * the stream "Advanced Simple Profile".
 *
 * Correctness: `-bf 0` is also what makes output order equal stored order,
 * which is what lets xviddec.c take the frame *number* as the timestamp
 * instead of reordering, and what makes the XVID_LOWDELAY it passes true
 * rather than merely asserted. With B-frames that assertion is a lie and
 * xvidcore hands frames back in coded order.
 *
 * **And nothing complains.** A `-bf 2` encode of this fixture decodes to the
 * right number of pictures, with monotonic timestamps, with no error from any
 * call -- and when the luma of each frame is compared against ffmpeg's own
 * decode of the same file, only **5 of 51 frames are in the right position**.
 * The documented `-bf 0` encode matches ffmpeg on 50 of 51. So the symptom of
 * getting this wrong is not a failure, it is a cutscene whose motion is
 * subtly scrambled, which is exactly the kind of thing that gets blamed on
 * the display path. A fixture regenerated without `-bf 0` would still pass
 * every claim below while testing a configuration the engine does not ship.
 *
 * (The one frame the correct encode does not match is position 1, where our
 * output equals ffmpeg's frame 0: both decoders produce 51 frames with
 * exactly one repeat, and they place that repeat one position apart. That is
 * the zero-length chunk described below, and which placement a VfW player
 * would have chosen is not something this harness establishes.)
 *
 * `hue` for the same reason movietest gives: a fixture whose frames are
 * identical cannot tell a working decoder from a stuck first frame, which is
 * claim 5.
 *
 * One property of this file was not asked for and is worth keeping. ffmpeg
 * emits a **zero-length video chunk** for its second frame -- the picture did
 * not change enough to be worth coding -- which is AVI's way of saying "same
 * frame again". That is a real path in xviddec.c (xvidcore cannot be handed a
 * length of zero, so the repeat is done above it), and it is covered here
 * only because the muxer happened to produce one. Claim 5 counts those chunks
 * in one pass and the duplicate pictures in another, and requires the two
 * figures to agree; so if a regenerated fixture has no such chunk, the claim
 * weakens to "no picture ever repeats" rather than silently passing on a
 * path nobody ran.
 *
 * Eight claims:
 *
 *   1. The container parses out of the archive, and reports the size, rate,
 *      duration and audio format the file actually has.
 *   2. Something that is not an AVI is refused without dying. This is the
 *      path that sends interp.c to the poster-frame stand-in.
 *   3. Every packet in `movi` comes back tagged as video or audio, and the
 *      video count is the frame count in the stream header. A demuxer that
 *      mistook the stream numbering would hand the packets to the wrong
 *      decoder, and the symptom through a window is not "the demuxer assumed
 *      an ordering".
 *   4. Decoding yields a picture per video packet, with non-NULL planes and
 *      strides at least as wide as the picture. The strides are the subject:
 *      XVID_CSP_INTERNAL is a zero-copy output and the guard that enables it
 *      is easy to get wrong in the direction of three null planes and no
 *      error -- see xviddec.h.
 *   5. Successive pictures differ, except exactly where the container said
 *      "same frame again". A demuxer that worked and a decoder that
 *      reconstructed nothing produce the right *number* of frames.
 *   6. Timestamps are monotonic and match the container's frame rate.
 *   7. Audio and video can be pulled in either order and neither starves the
 *      other. This is the one claim about code that exists only because the
 *      container is shared: movie.c fills the mixer's ring *before* decoding
 *      any picture, so a demand for audio walks past ten video packets, and
 *      the queues in xviddec.c are what stop those being lost. Driven here in
 *      the order movie.c actually uses, which no other claim does.
 *   8. The same, with the audio side allowed to run to the end of the file
 *      rather than taking one frame per picture. This is the case where a
 *      full video queue has to make the pump *refuse to read* instead of
 *      reading and discarding, and three pictures of fifty-one were being
 *      lost here. Claim 7 cannot see it: taking one MP3 frame per picture
 *      never fills the queue.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mr.h"
#include "avi.h"
#include "xviddec.h"
#include "yuv.h"
#include "vfs.h"
#include "plat.h"

void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); exit(1); }
void mr_log(const char *f, ...) { (void)f; }

static int fails;

static void check(const char *label, int ok)
{
    printf("  %-62s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

#define MOVIE   "audio/fixmovie.avi"
/* Not an AVI, and not nothing either: a real file of the wrong kind is the
 * case that actually happens -- a game whose cutscene is still an .mpg while
 * the ini says otherwise. */
#define NOTAVI  "audio/fixmovie.mpg"

/* --- the io, which is the VFS and nothing else --------------------------
 *
 * Through the archive rather than a loose file, deliberately: a movie is an
 * asset and `MR_PAK selects the game and nothing else does` (CLAUDE.md). The
 * seek callback is also doing real work here in a way it does not for
 * movietest's MPEG path -- avi.c skips over chunk payloads it does not want
 * rather than reading them, so a seek that silently did nothing would show up
 * as a header walk that read the picture data as a chunk header.
 */
static mr_file *g_f;

static long io_read(void *user, void *buf, long bytes)
{
    (void)user;
    return g_f ? mr_vfs_read(g_f, buf, bytes) : 0;
}

static void io_seek(void *user, long off)
{
    (void)user;
    if (g_f)
        mr_vfs_seek(g_f, off, SEEK_SET);
}

static long io_tell(void *user)
{
    (void)user;
    return g_f ? mr_vfs_tell(g_f) : 0;
}

static int open_io(const char *path, mr_avi_io *io)
{
    g_f = mr_vfs_open(path);
    if (!g_f)
        return 0;

    io->read = io_read;
    io->seek = io_seek;
    io->tell = io_tell;
    io->size = mr_vfs_size(g_f);
    io->user = NULL;
    return 1;
}

static void close_io(void)
{
    if (g_f) {
        mr_vfs_close(g_f);
        g_f = NULL;
    }
}

/* A cheap digest of a picture's luma, for "did this frame differ from the
 * last". Over the whole plane rather than one pixel: presenthost.c reports
 * one pixel and movietest had to rotate the fixture's hue to make that
 * sufficient, which is a lesson worth not relearning -- here the planes are
 * in hand, so there is no reason to look at less than all of them. */
static unsigned long luma_sum(const mr_yuv_frame *f)
{
    unsigned long s = 0;
    int row, col;

    for (row = 0; row < f->h; row++) {
        const mr_u8 *p = f->y + (long)row * f->y_pitch;

        for (col = 0; col < f->w; col++)
            s = s * 31u + p[col];
    }
    return s;
}

int main(int argc, char **argv)
{
    const char *pak, *dir;
    mr_avi_io io;
    mr_avi_info info;
    mr_avi *a;
    mr_xvid *x;
    /* Counted in the packet pass and used in the decode pass: how many video
     * chunks are AVI's "same frame again". See the fixture note in the
     * header; claim 5 is the two figures agreeing. */
    long repeats = 0;

    if (argc < 3) {
        fprintf(stderr, "usage: avitest <pak> <game-dir>\n");
        return 2;
    }
    pak = argv[1];
    dir = argv[2];

    setbuf(stdout, NULL);

    if (!mr_vfs_init(pak, dir)) {
        printf("SKIP: no pak at '%s'\n", pak);
        return 77;
    }

    /* --- 1. the container parses --------------------------------------- */
    printf("\nthe container\n");

    if (!open_io(MOVIE, &io)) {
        printf("SKIP: no %s in the archive\n", MOVIE);
        return 77;
    }

    memset(&info, 0, sizeof(info));
    a = mr_avi_open(&io, &info);
    check("an AVI in the archive opens", a != NULL);
    if (!a) {
        printf("\nFAILURES (nothing else can be tested)\n");
        return 1;
    }

    printf("   (%dx%d, %.2ffps, %ldms, %ld frames, fourcc %c%c%c%c)\n",
           info.width, info.height, info.fps, info.duration_ms,
           info.total_frames,
           (char)(info.fourcc & 0xFF), (char)((info.fourcc >> 8) & 0xFF),
           (char)((info.fourcc >> 16) & 0xFF), (char)((info.fourcc >> 24) & 0xFF));
    printf("   (audio: format 0x%04X, %dHz, %d channels)\n",
           info.audio_format, info.audio_rate, info.audio_channels);

    check("it reports the picture size in the stream header",
          info.width == 64 && info.height == 32);
    check("and the frame rate", info.fps > 24.9 && info.fps < 25.1);
    /* Two seconds, from dwLength and the rate. Not read from a timestamp --
     * AVI has none -- which is why this is exact rather than approximate. */
    check("and the duration, from the stream header's own rate and length",
          info.duration_ms >= 1950 && info.duration_ms <= 2050);
    /* Against the duration and the rate rather than against a number written
     * down here: all three come out of the same stream header, and a figure
     * copied from the fixture would stop being a claim about avi.c the moment
     * the fixture was regenerated at another length. Two seconds at 25fps is
     * 51 frames, not 50 -- the first is at t=0 -- which is the sort of
     * off-by-one a hardcoded expectation hides. */
    check("the frame count agrees with the duration and the rate",
          info.total_frames > 0 &&
          info.total_frames ==
              (long)(info.duration_ms * info.fps / 1000.0 + 0.5));
    /* The fourcc is handed to xvidcore at create time, so a container walk
     * that lost it would change which quirks the decoder applies. */
    check("and the video fourcc", info.fourcc == 0x44495658L);  /* 'XVID' */
    check("and that the audio is MP3, stereo, 44.1kHz",
          info.has_audio && info.audio_format == 0x0055 &&
          info.audio_rate == 44100 && info.audio_channels == 2);

    /* --- 3. every packet is tagged, and the counts are right ----------- */
    printf("\nthe packets\n");
    {
        const unsigned char *d;
        long n;
        int kind;
        long vn = 0, an = 0, abytes = 0, vbytes = 0;
        int untagged = 0;

        while ((kind = mr_avi_next(a, &d, &n)) != 0) {
            if (kind == MR_AVI_VIDEO) {
                vn++;
                vbytes += n;
                if (n < 2)
                    repeats++;
            }
            else if (kind == MR_AVI_AUDIO) { an++; abytes += n; }
            else                            untagged++;
        }

        printf("   (%ld video packets / %ld bytes, %ld audio / %ld bytes)\n",
               vn, vbytes, an, abytes);
        printf("   (%ld of the video packets say \"same frame again\")\n",
               repeats);

        check("mr_avi_next returns only the two kinds it documents",
              untagged == 0);
        /* Against the stream header's own count, which claim 1 pinned at 50.
         * The two numbers come from different parts of the file -- `strh`
         * and the `movi` walk -- so agreeing is a real claim about the walk
         * and not a tautology. */
        check("the video packet count is the stream header's frame count",
              vn == info.total_frames);
        check("the audio stream was found too, and is not empty",
              an > 0 && abytes > 0);
        /* Two seconds of 64kbps MP3 is about 16KB. A walk that mistook the
         * stream numbering would put this in the video column instead, and
         * the count above would still look plausible. */
        check("and is about the size two seconds of 64kbps MP3 should be",
              abytes > 12000 && abytes < 22000);
    }

    mr_avi_close(a);
    close_io();

    /* --- 2. the wrong kind of file is refused -------------------------- */
    printf("\nrefusing what will not play\n");
    if (open_io(NOTAVI, &io)) {
        memset(&info, 0, sizeof(info));
        a = mr_avi_open(&io, &info);
        check("an MPEG program stream is not mistaken for an AVI", a == NULL);
        if (a)
            mr_avi_close(a);
        close_io();
    } else {
        check("the not-an-AVI fixture is present", 0);
    }

    /* --- 4,5,6. decoding ---------------------------------------------- */
    printf("\ndecoding\n");

    if (!open_io(MOVIE, &io)) {
        check("reopening the movie", 0);
        printf("\nFAILURES\n");
        return 1;
    }

    memset(&info, 0, sizeof(info));
    x = mr_xvid_open(&io, &info);
    check("the decoder opens the same file", x != NULL);
    if (!x) {
        printf("\nFAILURES (nothing else can be tested)\n");
        return 1;
    }

    /* Not a claim -- a statement of which kernels ran, so the timing line
     * below cannot be misread. The host library is built ARCH_IS_GENERIC with
     * no assembly, so "none" is the correct and expected answer here; a build
     * that reported mmx would mean the suite was measuring something other
     * than what it says it is. On the target this same line is in log.txt and
     * is the only way to tell "slow because the picture is big" from "slow
     * because the SIMD silently is not there". */
    printf("   (xvidcore kernels in use: %s)\n", mr_xvid_simd_name());

    {
        mr_yuv_frame f;
        long pts = -1, prev_pts = -1;
        long pictures = 0;
        int  bad_planes = 0, bad_pts = 0, same_as_last = 0;
        unsigned long prev_sum = 0;
        int  first = 1;
        unsigned int t0;
        long dec_ms;

        t0 = mr_plat_ticks();
        while (mr_xvid_decode_video(x, &f, &pts)) {
            unsigned long sum;

            pictures++;

            if (!f.y || !f.cb || !f.cr ||
                f.y_pitch < f.w || f.c_pitch < f.w / 2 ||
                f.w != info.width || f.h != info.height)
                bad_planes++;

            if (pts <= prev_pts && !first)
                bad_pts++;
            prev_pts = pts;

            if (!f.y)
                continue;

            sum = luma_sum(&f);
            if (!first && sum == prev_sum)
                same_as_last++;
            prev_sum = sum;
            first = 0;
        }
        dec_ms = (long)(unsigned int)(mr_plat_ticks() - t0);

        printf("   (%ld pictures, last pts %ldms)\n", pictures, prev_pts);

        check("a picture comes out for every video packet",
              pictures == info.total_frames);
        /* The CSP_INTERNAL trap: a decode that "succeeds" with three null
         * planes. Separated from the count above because the count passes
         * while this fails. */
        check("every picture has real planes and strides at least its width",
              pictures > 0 && bad_planes == 0);
        printf("   (%d pictures identical to the one before)\n", same_as_last);
        /* Not "no picture ever repeats": the container asked for `repeats` of
         * them, and reproducing those is correct -- it is how a muxer says a
         * frame did not change, and ffmpeg's own decode of this fixture
         * produces the same duplicate in the same place. Requiring equality
         * rather than `<=` makes this a claim in both directions: a decoder
         * stuck on a reference frame fails it, and a repeat chunk silently
         * dropped fails it too. */
        check("pictures repeat exactly where the container says they do",
              pictures > 0 && (long)same_as_last == repeats);
        check("timestamps are monotonic", bad_pts == 0);
        /* The last picture is due one frame period before the end: the first
         * is at 0, so n frames span n-1 periods. Checked against the
         * container's own rate rather than written down, which is also what
         * makes it a claim about pts_of's arithmetic -- an implementation
         * that accumulated a rounded 40ms would drift off this. */
        check("and the last one lands exactly one frame short of the duration",
              info.fps > 0.0 &&
              info.duration_ms - prev_pts ==
                  (long)(1000.0 / info.fps + 0.5));

        /* Not a claim. xvidcore here is the C fallback -- see the header --
         * so this is a ratio against pl_mpeg measured the same way, and says
         * nothing about the Pentium III. */
        printf("   (decode of %ld pictures took %ldms on this host, C paths,\n"
               "    no MMX: a ratio against pl_mpeg, not a target figure)\n",
               pictures, dec_ms);
    }

    mr_xvid_close(x);
    close_io();

    /* --- 7. both streams, pulled the way movie.c pulls them ------------ */
    printf("\naudio and video out of one container\n");

    if (!open_io(MOVIE, &io)) {
        check("reopening the movie a third time", 0);
        printf("\nFAILURES\n");
        return 1;
    }

    memset(&info, 0, sizeof(info));
    x = mr_xvid_open(&io, &info);
    if (!x) {
        check("the decoder opens the file again", 0);
        printf("\nFAILURES\n");
        return 1;
    }

    {
        static short pcm[MR_XVID_AUDIO_FRAME * 2];
        mr_yuv_frame f;
        long pts;
        long pictures = 0, aframes = 0, loud = 0;
        int  n;

        /* movie.c's order: audio to exhaustion first, then one picture. That
         * is what puts ten video packets in the queue before the first
         * decode_video call, and it is the only thing that exercises it. */
        for (;;) {
            int got_any = 0;

            while ((n = mr_xvid_decode_audio(x, pcm, MR_XVID_AUDIO_FRAME)) > 0) {
                int i;

                aframes += n;
                got_any = 1;
                for (i = 0; i < n * 2; i++)
                    if (pcm[i] > 256 || pcm[i] < -256) { loud++; break; }
                /* One MP3 frame per pass, as the mixer would take it: the
                 * contract is "ask for the room, then fill it", and draining
                 * the whole file in one go would not be movie.c's shape. */
                break;
            }

            if (mr_xvid_decode_video(x, &f, &pts)) {
                pictures++;
                got_any = 1;
            }

            if (!got_any)
                break;
        }

        printf("   (%ld pictures and %ld audio frames, interleaved)\n",
               pictures, aframes);

        check("interleaving the two pulls loses no picture",
              pictures == info.total_frames);
        /* Two seconds at 44.1kHz is 88200 frames. MP3 is 1152 frames a frame
         * and the encoder pads to a whole one, so a frame either way. */
        check("and no audio: about two seconds of it arrives",
              aframes > 86000 && aframes < 90500);
        check("and the audio is a tone rather than silence", loud > 0);
    }

    mr_xvid_close(x);
    close_io();

    /* --- 8. an audio demand that will not stop ------------------------- */
    printf("\nthe same, with the audio side running flat out\n");

    if (!open_io(MOVIE, &io)) {
        check("reopening the movie a fourth time", 0);
        printf("\nFAILURES\n");
        return 1;
    }

    memset(&info, 0, sizeof(info));
    x = mr_xvid_open(&io, &info);
    if (!x) {
        check("the decoder opens the file a fourth time", 0);
        printf("\nFAILURES\n");
        return 1;
    }

    {
        static short pcm[MR_XVID_AUDIO_FRAME * 2];
        mr_yuv_frame f;
        long pts;
        long pictures = 0, aframes = 0;
        int  n;

        /* Claim 7 takes one MP3 frame per picture, which is the mixer's
         * shape on the target. This takes *everything the decoder will give*
         * before each picture, which is the mixer's shape on this host: the
         * shim's waveOut consumes a buffer the instant it is queued, so
         * mr_audio_push_space never shrinks and movie.c's feed_audio loop
         * runs until the decoder refuses. The demand therefore walks to the
         * end of the file, filling the video queue to its limit on the way.
         *
         * That is the only configuration that reaches the pump's refusal, and
         * the count below is the whole point of it: three pictures of
         * fifty-one went missing here before the refusal was fixed, and no
         * other claim in this file -- or in movietest -- could see it. It is
         * not an artificial shape either; it is literally what the harness
         * build of movie.c does. */
        for (;;) {
            int got_any = 0;

            while ((n = mr_xvid_decode_audio(x, pcm, MR_XVID_AUDIO_FRAME)) > 0) {
                aframes += n;
                got_any = 1;
            }

            if (mr_xvid_decode_video(x, &f, &pts)) {
                pictures++;
                got_any = 1;
            }

            if (!got_any)
                break;
        }

        printf("   (%ld pictures and %ld audio frames, audio first)\n",
               pictures, aframes);

        check("an audio demand that empties the file loses no picture",
              pictures == info.total_frames);
        check("and the soundtrack still arrives whole",
              aframes > 86000 && aframes < 90500);
    }

    mr_xvid_close(x);
    close_io();

    printf("\n%s\n", fails ? "FAILURES" : "all ok");
    return fails ? 1 : 0;
}

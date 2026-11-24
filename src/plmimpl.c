/* plmimpl.c - the one translation unit that instantiates pl_mpeg.
 *
 * pl_mpeg (libs/pl_mpeg.h, MIT, Dominic Szablewski) is an MPEG-1 video
 * decoder, an MP2 audio decoder and an MPEG program-stream demuxer in one
 * file. It is the whole of the codec; movie.c is the engine's side of it and
 * yuv.c is the only part of the output path that is ours.
 *
 * Isolated for the same two reasons mp3impl.c is. pl_mpeg is C99 --
 * declarations after statements, declarations in `for` -- so this file is
 * compiled with -za99 while everything else stays C89, and the decoder's
 * static tables (the VLC trees, the dequant matrices, the MP2 filterbank
 * window) are emitted exactly once.
 *
 * **The vendor file is unmodified, and should stay that way.** It needed no
 * patching, which is worth recording because it looks as though it would:
 * out of the box it will not compile, and the fault is one missing include
 * rather than anything in the code. Its public section uses `size_t` and
 * `FILE` before anything has defined them, so a bare `#include "pl_mpeg.h"`
 * fails with thirteen "unknown type name size_t" and a cascade of a hundred
 * more. Supplying the two headers first is the whole fix; the result compiles
 * with -Wall and no warnings at all. Do not be tempted to edit the header to
 * add them -- a vendored file that differs from upstream by one line is a file
 * nobody can re-download.
 *
 * PLM_NO_STDIO removes the fopen/fread constructors. The engine never reads
 * the host filesystem for a movie: the file comes out of the VFS, which means
 * out of mrpy.pak, through the load callback movie.c installs. Leaving them in
 * would link fopen and a FILE* path that nothing calls, and on DOS it would
 * also offer a second way to open a file that bypasses the archive -- the
 * `MR_PAK selects the game and nothing else does` rule in CLAUDE.md.
 *
 * Note what is *not* defined here: there is no SIMD switch to turn on. Unlike
 * minimp3, pl_mpeg has no SSE path to disable, and its IDCT and motion
 * compensation are integer C. The MMX in this port is in yuv.c, on the colour
 * conversion, which is ours; see the note there on why that is the loop worth
 * hand-writing and the decoder's inner loops are not.
 */

/* pl_mpeg's public declarations use size_t and, with stdio left in, FILE,
 * before it has included anything that defines them. Order matters here. */
#include <stddef.h>
#include <stdlib.h>

#define PLM_NO_STDIO
#define PL_MPEG_IMPLEMENTATION

#include "pl_mpeg.h"

#include "mr.h"
#include "yuv.h"
#include "plmdec.h"

/* --- the engine's side of the line ---------------------------------------
 *
 * plmdec.h explains why this wrapper exists rather than movie.c including the
 * header directly: the engine is C89 and this file is the only one compiled
 * with -za99, so pl_mpeg's types and its `//` comments stop here.
 *
 * It is a thin layer and should stay thin. Nothing here decides anything --
 * no timing, no frame dropping, no mixing. Those are movie.c's, in C89, where
 * they can be read and argued about; this is translation and nothing else.
 */

struct mr_plm {
    plm_t        *plm;
    plm_buffer_t *buf;
    mr_plm_io     io;
};

/* pl_mpeg's three buffer callbacks, forwarded to the io struct. The
 * plm_buffer_t carries our mr_plm as its user pointer. */
static void plm_cb_load(plm_buffer_t *self, void *user)
{
    mr_plm *p = (mr_plm *)user;
    unsigned char chunk[16 * 1024];
    long got;

    if (!p || !p->io.read)
        return;

    got = p->io.read(p->io.user, chunk, (long)sizeof(chunk));
    if (got <= 0) {
        /* Telling the buffer the stream has ended is what lets the decoder
         * finish the picture it is part way through, instead of waiting for
         * bytes that are not coming and leaving playback stalled one frame
         * from the end. */
        plm_buffer_signal_end(self);
        return;
    }

    plm_buffer_write(self, chunk, (size_t)got);
}

static void plm_cb_seek(plm_buffer_t *self, size_t offset, void *user)
{
    mr_plm *p = (mr_plm *)user;

    (void)self;
    if (p && p->io.seek)
        p->io.seek(p->io.user, (long)offset);
}

static size_t plm_cb_tell(plm_buffer_t *self, void *user)
{
    mr_plm *p = (mr_plm *)user;

    (void)self;
    return (p && p->io.tell) ? (size_t)p->io.tell(p->io.user) : 0;
}

mr_plm *mr_plm_open(const mr_plm_io *io, mr_plm_info *info)
{
    mr_plm *p;
    int w, h;

    if (!io || !io->read)
        return NULL;

    p = (mr_plm *)calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->io = *io;

    /* A callbacks buffer rather than a capacity buffer, and the difference is
     * the duration: plm_demux_get_duration seeks to near the end of the file
     * and reads forward for the last video timestamp, so a forward-only
     * buffer cannot answer. The size is handed over because that search
     * measures backwards from it. */
    p->buf = plm_buffer_create_with_callbacks(plm_cb_load, plm_cb_seek,
                                              plm_cb_tell,
                                              (size_t)io->size, p);
    if (!p->buf) {
        free(p);
        return NULL;
    }

    /* 1: plm_destroy frees the buffer too, which mr_plm_close relies on. */
    p->plm = plm_create_with_buffer(p->buf, 1);
    if (!p->plm) {
        plm_buffer_destroy(p->buf);
        free(p);
        return NULL;
    }

    w = plm_get_width(p->plm);
    h = plm_get_height(p->plm);
    if (w <= 0 || h <= 0) {
        /* No video track, or a sequence header the decoder would not take. */
        mr_plm_close(p);
        return NULL;
    }

    if (info) {
        info->width = w;
        info->height = h;
        info->fps = plm_get_framerate(p->plm);
        info->duration_ms = (long)(plm_get_duration(p->plm) * 1000.0);
        info->has_audio = plm_get_num_audio_streams(p->plm) > 0;
        info->sample_rate = plm_get_samplerate(p->plm);
    }

    /* Audio is decoded only if it is wanted, and pl_mpeg's own A/V sync is
     * never used: plm_decode() is the interface that is handed an elapsed time
     * and decides what to decode, and here the clock is the sound card and
     * movie.c does the deciding. The plm_decode_video/plm_decode_audio pair
     * below pulls from the demuxer with no time of its own. */
    plm_set_audio_enabled(p->plm,
                          info ? info->has_audio
                               : (plm_get_num_audio_streams(p->plm) > 0));

    return p;
}

void mr_plm_close(mr_plm *p)
{
    if (!p)
        return;
    if (p->plm)
        plm_destroy(p->plm);        /* frees p->buf with it */
    else if (p->buf)
        plm_buffer_destroy(p->buf);
    free(p);
}

int mr_plm_decode_video(mr_plm *p, mr_yuv_frame *out, long *pts_ms)
{
    plm_frame_t *f;

    if (!p || !p->plm || !out)
        return 0;

    f = plm_decode_video(p->plm);
    if (!f)
        return 0;

    out->w = (int)f->width;
    out->h = (int)f->height;
    out->y = f->y.data;
    out->cb = f->cb.data;
    out->cr = f->cr.data;

    /* The planes are wider than the picture when the size is not a multiple
     * of 16: a macroblock is 16x16 and the decoder reconstructs whole ones,
     * so `width` and `y.width` are different numbers and the pitch is the
     * second. Getting this wrong is a picture that shears progressively down
     * the screen, and only for videos whose width is not a multiple of 16 --
     * which ending.mpg's 640 is, so it would not have shown up here. */
    out->y_pitch = (int)f->y.width;
    out->c_pitch = (int)f->cb.width;

    if (pts_ms)
        *pts_ms = (long)(f->time * 1000.0);

    return 1;
}

int mr_plm_decode_audio(mr_plm *p, short *pcm, int max_frames)
{
    plm_samples_t *s;
    int i, n;

    if (!p || !p->plm || !pcm || max_frames <= 0)
        return 0;

    s = plm_decode_audio(p->plm);
    if (!s)
        return 0;

    n = (int)s->count;
    if (n > max_frames)
        n = max_frames;
    if (n <= 0)
        return 0;

    /* Floats in -1..1 to interleaved 16-bit, which is what the mixer's ring
     * holds. pl_mpeg always produces two channels -- it duplicates a mono
     * stream itself -- so the mixer never sees a channel count.
     *
     * 32767 and not 32768, and the clamp is not decorative: MP2 is a lossy
     * transform codec and does produce samples a little outside the nominal
     * range, and at 32768 a sample at exactly +1.0 wraps to full-scale
     * negative -- an audible click, once per loud passage. */
    for (i = 0; i < n * 2; i++) {
        float v = s->interleaved[i];

        if (v > 1.0f)
            v = 1.0f;
        else if (v < -1.0f)
            v = -1.0f;

        pcm[i] = (short)(v * 32767.0f);
    }

    return n;
}

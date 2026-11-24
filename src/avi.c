/* avi.c - the AVI container, read forwards. See avi.h. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mr.h"
#include "avi.h"

/* The biggest packet that will be accepted.
 *
 * A chunk size comes straight out of the file, so it is attacker-shaped data
 * in the only sense that matters here: a truncated or mangled .avi in a pak
 * must fail to play rather than ask for a gigabyte on a machine with sixteen
 * megabytes. The figure is generous against what a cutscene actually
 * produces -- a 640x320 key frame at the quality the encoder is asked for is
 * tens of kilobytes, and the audio chunks are under two -- and the buffer is
 * grown to fit rather than allocated at this size. */
#define AVI_MAX_PACKET  (1024L * 1024L)

/* Where the buffer starts, before any packet has been seen. One read of this
 * covers every audio chunk and most video ones, so the usual case allocates
 * once at open and never again. */
#define AVI_BUF_START   (64L * 1024L)

struct mr_avi {
    mr_avi_io io;

    long movi_end;          /* file offset one past the last byte of `movi` */
    long pos;               /* where the next chunk header begins */

    /* Stream numbers, as they appear in a chunk id, not positions. -1 for
     * absent; see avi.h on why these are recorded rather than assumed. */
    int  vstream;
    int  astream;

    unsigned char *buf;
    long           bufcap;
};

/* --- little-endian fields, from a byte buffer -----------------------------
 *
 * Done by hand rather than by casting to a short or a long. RIFF is
 * little-endian whatever the host is, and the engine has one place where a
 * structure was read by overlaying it and it broke on the first big-endian
 * thought anybody had about it. These also sidestep alignment: a chunk
 * payload begins at an offset the file chose. */
static unsigned long rd32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static unsigned int rd16(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

/* A signed 32-bit field. biHeight is one: a negative value means a top-down
 * DIB, which for our purposes is only a sign on a number we take the
 * magnitude of. Built from the unsigned read so that a long wider than 32
 * bits -- which is what the gcc harness build has, and the Watcom build does
 * not -- sign-extends the same way in both. */
static long rd32s(const unsigned char *p)
{
    unsigned long v = rd32(p);

    if (v & 0x80000000UL)
        return -(long)(0xFFFFFFFFUL - v + 1UL);
    return (long)v;
}

static int read_exact(mr_avi *a, void *buf, long bytes)
{
    return a->io.read(a->io.user, buf, bytes) == bytes;
}

static int grow(mr_avi *a, long want)
{
    unsigned char *nb;
    long cap;

    if (want <= a->bufcap)
        return 1;
    if (want > AVI_MAX_PACKET)
        return 0;

    cap = a->bufcap ? a->bufcap : AVI_BUF_START;
    while (cap < want)
        cap *= 2;
    if (cap > AVI_MAX_PACKET)
        cap = AVI_MAX_PACKET;

    nb = (unsigned char *)realloc(a->buf, (size_t)cap);
    if (!nb)
        return 0;
    a->buf = nb;
    a->bufcap = cap;
    return 1;
}

/* Two ASCII digits of a chunk id as a stream number. -1 if they are not
 * digits, which is what `JUNK` and `idx1` inside `movi` look like. */
static int stream_of(const unsigned char *id)
{
    if (id[0] < '0' || id[0] > '9' || id[1] < '0' || id[1] > '9')
        return -1;
    return (id[0] - '0') * 10 + (id[1] - '0');
}

/* --- the header tree -----------------------------------------------------
 *
 * Walked rather than assumed. The layout every encoder actually produces is
 * `avih` then one `strl` per stream, but the format does not require that
 * order and ffmpeg has changed what it puts in `hdrl` between versions --
 * there is an `odml` list in there now that was not there before. Skipping
 * what is not recognised is the only reading of this that does not break on
 * the next muxer.
 */
static int parse_strl(mr_avi *a, long start, long end, int stream,
                      mr_avi_info *info)
{
    unsigned char hdr[8];
    unsigned char body[64];
    long pos = start;
    int  is_video = 0, is_audio = 0;
    long scale = 0, rate = 0, length = 0;

    while (pos + 8 <= end) {
        long sz;

        a->io.seek(a->io.user, pos);
        if (!read_exact(a, hdr, 8))
            return 0;
        sz = (long)rd32(hdr + 4);
        if (sz < 0 || pos + 8 + sz > end)
            return 0;

        if (!memcmp(hdr, "strh", 4) && sz >= 40) {
            if (!read_exact(a, body, 40))
                return 0;
            is_video = !memcmp(body, "vids", 4);
            is_audio = !memcmp(body, "auds", 4);
            scale  = (long)rd32(body + 20);
            rate   = (long)rd32(body + 24);
            length = (long)rd32(body + 32);

            /* The first of each kind wins. A file with two video streams is
             * not something a cutscene is, and taking the first is a
             * definite behaviour where taking the last would depend on the
             * muxer. */
            if (is_video && a->vstream < 0)
                a->vstream = stream;
            else if (is_audio && a->astream < 0)
                a->astream = stream;
            else
                is_video = is_audio = 0;

        } else if (!memcmp(hdr, "strf", 4) && is_video && sz >= 20) {
            /* BITMAPINFOHEADER. biHeight is signed and may be negative for a
             * top-down DIB; the picture is the same size either way. */
            if (!read_exact(a, body, 20))
                return 0;
            info->width  = (int)rd32s(body + 4);
            info->height = (int)rd32s(body + 8);
            if (info->height < 0)
                info->height = -info->height;
            info->fourcc = (long)rd32(body + 16);

            if (rate > 0 && scale > 0) {
                info->fps = (double)rate / (double)scale;
                info->total_frames = length;
                /* In this order, and in long arithmetic: length * scale can
                 * be large -- a three-minute movie at 1/25 scale is only
                 * 4500, but a stream with scale in microseconds is not -- and
                 * the double is already in hand for the division. */
                info->duration_ms =
                    (long)((double)length * (double)scale * 1000.0 /
                           (double)rate + 0.5);
            }

        } else if (!memcmp(hdr, "strf", 4) && is_audio && sz >= 16) {
            /* WAVEFORMATEX. */
            if (!read_exact(a, body, 16))
                return 0;
            info->has_audio      = 1;
            info->audio_format   = (int)rd16(body + 0);
            info->audio_channels = (int)rd16(body + 2);
            info->audio_rate     = (int)rd32(body + 4);
        }

        pos += 8 + sz + (sz & 1);
    }

    return 1;
}

static int parse_hdrl(mr_avi *a, long start, long end, mr_avi_info *info)
{
    unsigned char hdr[12];
    long pos = start;
    int  stream = 0;

    while (pos + 8 <= end) {
        long sz;

        a->io.seek(a->io.user, pos);
        if (!read_exact(a, hdr, 8))
            return 0;
        sz = (long)rd32(hdr + 4);
        if (sz < 0 || pos + 8 + sz > end)
            return 0;

        if (!memcmp(hdr, "LIST", 4) && sz >= 4) {
            if (!read_exact(a, hdr + 8, 4))
                return 0;
            if (!memcmp(hdr + 8, "strl", 4)) {
                if (!parse_strl(a, pos + 12, pos + 8 + sz, stream, info))
                    return 0;
                /* Counted per `strl`, whether or not it was a kind we use:
                 * the number in a chunk id is the position of the list, so a
                 * stream we ignore still consumes one. */
                stream++;
            }
        }
        /* `avih` is read for nothing: every field this file needs -- the
         * frame rate, the length, the picture size -- is in the *stream*
         * header, which is the authority when the two disagree, and they do
         * disagree in files produced by more than one tool. dwTotalFrames in
         * `avih` counts frames in the first `rec` list for an interleaved
         * file, which is not the frame count. */

        pos += 8 + sz + (sz & 1);
    }

    return 1;
}

mr_avi *mr_avi_open(const mr_avi_io *io, mr_avi_info *out)
{
    mr_avi *a;
    mr_avi_info info;
    unsigned char hdr[12];
    long pos;

    if (!io || !io->read || !io->seek || io->size < 12)
        return NULL;

    a = (mr_avi *)calloc(1, sizeof(*a));
    if (!a)
        return NULL;
    a->io = *io;
    a->vstream = -1;
    a->astream = -1;

    memset(&info, 0, sizeof(info));

    a->io.seek(a->io.user, 0);
    if (!read_exact(a, hdr, 12) ||
        memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "AVI ", 4)) {
        free(a);
        return NULL;
    }

    /* Top level: find `hdrl` and `movi`. Both are needed and `hdrl` always
     * precedes `movi`, so one forward walk does it. */
    for (pos = 12; pos + 8 <= io->size; ) {
        long sz;

        a->io.seek(a->io.user, pos);
        if (!read_exact(a, hdr, 8))
            break;
        sz = (long)rd32(hdr + 4);
        if (sz < 0)
            break;

        if (!memcmp(hdr, "LIST", 4) && sz >= 4) {
            long end = pos + 8 + sz;

            if (!read_exact(a, hdr + 8, 4))
                break;

            /* Clamped against the file, not trusted. A RIFF size that runs
             * past the end of the archive member is what a truncated pak
             * looks like, and the walk below must stop at real data. */
            if (end > io->size)
                end = io->size;

            if (!memcmp(hdr + 8, "hdrl", 4)) {
                if (!parse_hdrl(a, pos + 12, end, &info)) {
                    free(a);
                    return NULL;
                }
            } else if (!memcmp(hdr + 8, "movi", 4)) {
                a->movi_end = end;
                a->pos = pos + 12;
                break;
            }
        }

        pos += 8 + sz + (sz & 1);
    }

    if (!a->movi_end || a->vstream < 0 ||
        info.width <= 0 || info.height <= 0) {
        free(a);
        return NULL;
    }

    if (!grow(a, AVI_BUF_START)) {
        free(a);
        return NULL;
    }

    /* An audio stream whose `strf` was missing or too short is no audio
     * stream: movie.c would otherwise open a pushed channel at a rate of 0
     * and feed it nothing. */
    if (!info.has_audio || info.audio_rate <= 0) {
        info.has_audio = 0;
        a->astream = -1;
    }

    if (out)
        *out = info;
    return a;
}

void mr_avi_close(mr_avi *a)
{
    if (!a)
        return;
    free(a->buf);
    free(a);
}

int mr_avi_next(mr_avi *a, const unsigned char **data, long *len)
{
    unsigned char hdr[12];

    if (!a || !data || !len)
        return 0;

    while (a->pos + 8 <= a->movi_end) {
        long sz;
        int  stream;

        a->io.seek(a->io.user, a->pos);
        if (!read_exact(a, hdr, 8))
            return 0;
        sz = (long)rd32(hdr + 4);
        if (sz < 0 || a->pos + 8 + sz > a->movi_end)
            return 0;

        /* An interleaved file groups a video chunk and the audio that goes
         * with it inside `LIST rec `. Descending into it rather than skipping
         * it is the whole difference between playing such a file and seeing
         * nothing at all, and ffmpeg writes them when asked to interleave. */
        if (!memcmp(hdr, "LIST", 4) && sz >= 4) {
            a->pos += 12;
            continue;
        }

        stream = stream_of(hdr);

        if (stream >= 0 && stream == a->vstream &&
            hdr[2] == 'd' && (hdr[3] == 'c' || hdr[3] == 'b')) {
            if (!grow(a, sz > 0 ? sz : 1))
                return 0;
            if (sz > 0 && !read_exact(a, a->buf, sz))
                return 0;
            a->pos += 8 + sz + (sz & 1);
            *data = a->buf;
            *len = sz;
            return MR_AVI_VIDEO;
        }

        if (stream >= 0 && stream == a->astream &&
            hdr[2] == 'w' && hdr[3] == 'b') {
            if (!grow(a, sz > 0 ? sz : 1))
                return 0;
            if (sz > 0 && !read_exact(a, a->buf, sz))
                return 0;
            a->pos += 8 + sz + (sz & 1);
            *data = a->buf;
            *len = sz;
            return MR_AVI_AUDIO;
        }

        /* Somebody else's stream, or `idx1` reached early, or a `JUNK` pad.
         * Skipped without reading the payload. */
        a->pos += 8 + sz + (sz & 1);
    }

    return 0;
}

/* viddec.c - pick a codec from the file's own bytes. See viddec.h. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mr.h"
#include "viddec.h"
#include "plmdec.h"
#include "xviddec.h"
#include "avi.h"
#include "yuv.h"

/* Which backend is behind the handle. An enum rather than a function-pointer
 * table: there are two, the four entry points are each three lines, and a
 * vtable would put an indirect call on the per-frame path to save nothing. */
typedef enum {
    VID_PLM = 0,
    VID_XVID
} vid_kind;

struct mr_vid {
    vid_kind kind;
    mr_plm  *plm;
    mr_xvid *xvid;
};

void mr_vid_set_simd(int on)
{
    /* pl_mpeg has no SIMD of its own to switch -- our colour conversion is
     * the MMX in that path and movie.c sets it directly through
     * mr_yuv_set_simd. So this only has somewhere to go for xvidcore. */
    mr_xvid_set_simd(on);
}

/* Is this a RIFF/AVI file?
 *
 * Twelve bytes, read and then put back. Nothing else is needed to tell the
 * two containers apart: an MPEG program stream begins with a pack start code
 * 00 00 01 BA, so there is no file that answers yes to both.
 *
 * The seek back to 0 is what makes this safe to do before handing the reader
 * to a backend -- both of them start by reading from wherever the stream is,
 * and a sniff that consumed twelve bytes would leave pl_mpeg looking at the
 * fourth byte of a pack header and concluding the file was not a movie. */
static int looks_like_avi(const mr_vid_io *io)
{
    unsigned char hdr[12];
    int yes;

    if (io->size < 12)
        return 0;

    io->seek(io->user, 0);
    yes = io->read(io->user, hdr, 12) == 12 &&
          !memcmp(hdr, "RIFF", 4) &&
          !memcmp(hdr + 8, "AVI ", 4);
    io->seek(io->user, 0);
    return yes;
}

mr_vid *mr_vid_open(const mr_vid_io *io, mr_vid_info *out)
{
    mr_vid *v;
    mr_vid_info info;

    if (!io || !io->read || !io->seek)
        return NULL;

    v = (mr_vid *)calloc(1, sizeof(*v));
    if (!v)
        return NULL;

    memset(&info, 0, sizeof(info));

    if (looks_like_avi(io)) {
        mr_avi_io aio;
        mr_avi_info ainfo;

        aio.read = io->read;
        aio.seek = io->seek;
        aio.tell = io->tell;
        aio.size = io->size;
        aio.user = io->user;

        memset(&ainfo, 0, sizeof(ainfo));
        v->xvid = mr_xvid_open(&aio, &ainfo);
        if (v->xvid) {
            v->kind = VID_XVID;
            info.width       = ainfo.width;
            info.height      = ainfo.height;
            info.fps         = ainfo.fps;
            info.duration_ms = ainfo.duration_ms;
            info.has_audio   = ainfo.has_audio;
            info.sample_rate = ainfo.audio_rate;

            /* The codec *and* which kernels it chose. One static buffer
             * rather than a const literal, because the second half is only
             * known after the library's one-time init has run. It is written
             * once per open and read once per open, by the log line
             * immediately after. */
            {
                static char name[32];
                const char *simd = mr_xvid_simd_name();

                strcpy(name, "xvid/mp3 simd=");
                strncpy(name + 14, simd, sizeof(name) - 15);
                name[sizeof(name) - 1] = '\0';
                info.codec = name;
            }

            if (out)
                *out = info;
            return v;
        }

        /* An AVI that would not decode -- a fourcc xvidcore does not know, or
         * a header walk that found no video stream. Falling through to
         * pl_mpeg rather than failing here is deliberate and costs one
         * rewind: the MPEG backend will refuse it too, and then there is one
         * failure path and one log line instead of two. */
        io->seek(io->user, 0);
    }

    {
        mr_plm_io pio;
        mr_plm_info pinfo;

        pio.read = io->read;
        pio.seek = io->seek;
        pio.tell = io->tell;
        pio.size = io->size;
        pio.user = io->user;

        memset(&pinfo, 0, sizeof(pinfo));
        v->plm = mr_plm_open(&pio, &pinfo);
        if (!v->plm) {
            free(v);
            return NULL;
        }

        v->kind = VID_PLM;
        info.width       = pinfo.width;
        info.height      = pinfo.height;
        info.fps         = pinfo.fps;
        info.duration_ms = pinfo.duration_ms;
        info.has_audio   = pinfo.has_audio;
        info.sample_rate = pinfo.sample_rate;
        info.codec       = "mpeg1/mp2";
    }

    if (out)
        *out = info;
    return v;
}

void mr_vid_close(mr_vid *v)
{
    if (!v)
        return;

    if (v->plm)
        mr_plm_close(v->plm);
    if (v->xvid)
        mr_xvid_close(v->xvid);
    free(v);
}

int mr_vid_decode_video(mr_vid *v, mr_yuv_frame *out, long *pts_ms)
{
    if (!v)
        return 0;
    if (v->kind == VID_XVID)
        return mr_xvid_decode_video(v->xvid, out, pts_ms);
    return mr_plm_decode_video(v->plm, out, pts_ms);
}

int mr_vid_decode_audio(mr_vid *v, short *pcm, int max_frames)
{
    if (!v)
        return 0;
    if (v->kind == VID_XVID)
        return mr_xvid_decode_audio(v->xvid, pcm, max_frames);
    return mr_plm_decode_audio(v->plm, pcm, max_frames);
}

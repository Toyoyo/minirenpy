/* presenthost.h - what a harness may ask the host's video sink.
 *
 * Separate from present.h on purpose. These four functions exist only on this
 * machine; putting them in present.h would let a file in src/ come to depend
 * on something the target has no implementation of, and the failure would be
 * a link error on a DOS box rather than here.
 *
 * See presenthost.c. The sink is linked into any harness that links movie.c,
 * which is every harness that links the engine.
 */

#ifndef MR_PRESENTHOST_H
#define MR_PRESENTHOST_H

#include "mr.h"

/* Frames handed to mr_present_video since the last reset. */
long mr_hostpresent_video_frames(void);

/* The size of the most recent one. Either pointer may be NULL. */
void mr_hostpresent_video_last(int *w, int *h);

/* The top-left pixel of the most recent frame, converted. Returns 0 if no
 * frame with planes has arrived, in which case *px is untouched.
 *
 * For telling a decoded picture from a blank one. A decoder that demuxed
 * correctly but reconstructed nothing produces frames of a uniform colour --
 * usually mid-grey, since an all-zero YUV plane set is not black -- and a
 * frame count alone cannot see that. */
int  mr_hostpresent_video_pixel(mr_u32 *px);

void mr_hostpresent_video_reset(void);

#endif /* MR_PRESENTHOST_H */

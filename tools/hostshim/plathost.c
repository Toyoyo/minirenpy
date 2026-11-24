/* plathost.c - plat.h for the harnesses.
 *
 * The host build has no window, no message queue and nothing to present to,
 * but image.c times its PNG decodes with mr_plat_ticks under MR_PROF and
 * that is enough to need a definition. Rather than let each harness invent
 * one, there is one here and run.sh links it into every harness.
 *
 * Every entry point is present so that a harness which starts driving
 * input.c does not have to come back and add them. They do nothing; a test
 * that cares what one of them was asked to do should assert on it, which
 * means replacing this file rather than extending it.
 *
 * Not in src/: this is the platform layer for a target that is not real.
 */

#include <stdio.h>
#include <time.h>

#include "mr.h"
#include "plat.h"

/* clock() rather than gettimeofday or clock_gettime: C89, available
 * everywhere, and the harnesses only ever use it for a profiling total that
 * nothing asserts on. */
unsigned int mr_plat_ticks(void)
{
    return (unsigned int)(clock() / (CLOCKS_PER_SEC / 1000));
}

void mr_plat_yield(void)
{
}

/* Nothing to maintain: there is no pointer and no screen. The image
 * harnesses call it a few thousand times a run and it has to stay free. */
void mr_plat_service(void)
{
}

int mr_plat_pump(void)
{
    return 1;
}

void mr_plat_set_fullscreen(int on)
{
    (void)on;
}

void mr_plat_capture_mouse(int on)
{
    (void)on;
}

void mr_plat_restore_size(void)
{
}

void mr_plat_set_title(const char *text)
{
    (void)text;
}

/* mr_fatal already logged the message and is about to exit(1); each harness
 * supplies its own mr_fatal, so this is only reached if one of them calls
 * into the real main.c, which none do. */
/* No watchpoint here. See plat.h: the bug it exists for has appeared on
 * one DOS machine only, and a second implementation would be guesswork
 * with nothing to test it against. */
void mr_plat_watch_write(const void *addr)
{
    (void)addr;
}

void mr_plat_fatal(const char *msg)
{
    fprintf(stderr, "FATAL: %s\n", msg ? msg : "");
}

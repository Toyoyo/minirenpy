/* pthread.h - the three lines of it that xvidcore's DOS build needs.
 *
 * Only on the include path for `wmake -f Makefile.watcom dos`; neither the
 * Win32 build nor the engine ever sees this file.
 *
 * --- why it exists --------------------------------------------------------
 *
 * xvidcore's portab.h picks a threading flavour from the OS it is being built
 * for: a WIN32 branch over CreateThread, an AmigaOS one, a BeOS one, and
 * `#else # include <pthread.h>`. A Watcom `-bt=dos` build matches none of
 * them and falls into the else, and there is no pthread.h for a DOS target --
 * so xvid.c stopped at "E1055: Unable to open 'pthread.h'" before anything
 * else could be judged.
 *
 * The honest reading of that is not "DOS needs a thread library". It is that
 * **a decode-only xvidcore never starts a thread.** Every pthread_create and
 * pthread_join in the compiled set is inside `#ifdef HAVE_PTHREAD`, which
 * nothing in Makefile.watcom defines; sched_yield is only in
 * estimation_bvop.c and estimation_pvop.c, which are encoder files and are
 * not compiled. What remains is the *type*: motion_smp.h:34 and
 * postprocessing.h:54 each declare a `pthread_t handle` field, and a struct
 * needs a complete type for a field whether or not anything assigns to it.
 *
 * So this supplies the type and nothing else. If a future xvidcore, or an
 * encoder build, ever reaches a real pthread call, it will fail to compile
 * here rather than silently run single-threaded -- which is the right way
 * round, and is why the functions are deliberately *not* stubbed out.
 *
 * --- why a shim rather than a patch ---------------------------------------
 *
 * The same reasoning plmimpl.c sets out for pl_mpeg: vendor source that needs
 * no modification is vendor source that can be replaced by dropping in a new
 * release. Patching portab.h would make the next xvidcore update a merge.
 *
 * And it is outside libs/xvidcore-1.3.7/ for the reason the top-level
 * Makefile gives about libs/Makefile.libpng, which used to live inside
 * libpng's tree: a file kept inside a library tree is a file a library update
 * takes away.
 */

#ifndef MR_XVIDSHIM_PTHREAD_H
#define MR_XVIDSHIM_PTHREAD_H

/* An int, because it is only ever a struct field that nothing reads. Not a
 * pointer: a handle that is accidentally dereferenced should be a type error,
 * and on this build there is nothing it could point at. */
typedef int pthread_t;

#endif /* MR_XVIDSHIM_PTHREAD_H */

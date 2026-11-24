/* The two bounded builders that replaced two unbounded sprintfs.
 *
 * The bugs were in vfs.c:
 *
 *   sprintf(out, "%s%s", prefix, fd.cFileName);
 *   out[outsz - 1] = '\0';
 *
 * which truncates *after* writing -- cFileName is up to 260 bytes and
 * image.c hands it a 128-byte buffer -- and
 *
 *   sprintf(path, "%s/%s", g_loose, name);
 *
 * into 320 bytes, where the loose directory can be 191 and `name` comes from
 * the script.
 *
 * Neither is reachable from a Linux harness: the first is inside
 * `#ifdef _WIN32` and the second needs loose mode, so the suite ran green
 * past both for the life of the project. That is the whole reason mr_vfs_cat
 * and mr_vfs_join are public -- an overflow that no test can reach is an
 * overflow that stays. This drives them directly.
 *
 * What is being asserted is not "it truncates" but the three properties a
 * caller actually relies on: it never writes past outsz, it always NUL
 * terminates, and it says whether it fit. A function that always returned 0
 * and wrote nothing would satisfy the first two, so the fitting cases are
 * checked as hard as the overflowing ones.
 */
#include <stdio.h>
#include <string.h>

#include "mr.h"
#include "vfs.h"

void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); }
void mr_log(const char *f, ...) { (void)f; }

static int fails;

static void check(const char *label, int ok)
{
    printf("%-54s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

/* A canary either side of the target, so a write past the end is caught here
 * rather than by whatever happens to sit next to it in a real build. */
typedef struct {
    char guard_lo[8];
    char buf[32];
    char guard_hi[8];
} fenced;

static void fence(fenced *f)
{
    memset(f, 0xAA, sizeof(*f));
    memset(f->buf, 0x55, sizeof(f->buf));
}

static int intact(const fenced *f)
{
    int i;
    for (i = 0; i < 8; i++)
        if ((unsigned char)f->guard_lo[i] != 0xAA ||
            (unsigned char)f->guard_hi[i] != 0xAA)
            return 0;
    return 1;
}

int main(void)
{
    fenced f;
    char   big[MR_FSPATH_MAX];
    char   longname[400];
    int    r;

    setbuf(stdout, NULL);

    memset(longname, 'x', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';

    printf("-- it fits --\n");

    fence(&f);
    r = mr_vfs_cat(f.buf, (int)sizeof(f.buf), "images/", "bg.png");
    check("cat joins verbatim", r == 1 && !strcmp(f.buf, "images/bg.png"));
    check("  and leaves the fences alone", intact(&f));

    /* Exactly full: 31 characters plus the terminator in 32 bytes. This is
     * the boundary the old code got wrong in the other direction, so it is
     * worth pinning that it is *not* reported as truncated. */
    fence(&f);
    r = mr_vfs_cat(f.buf, (int)sizeof(f.buf), "0123456789012345", "678901234567890");
    check("a string that exactly fills the buffer fits",
          r == 1 && strlen(f.buf) == 31);
    check("  and leaves the fences alone", intact(&f));

    printf("\n-- it does not fit --\n");

    fence(&f);
    r = mr_vfs_cat(f.buf, (int)sizeof(f.buf), "images/", longname);
    check("cat reports truncation", r == 0);
    check("  stays inside the buffer", intact(&f));
    check("  NUL terminates", f.buf[sizeof(f.buf) - 1] == '\0' &&
                             strlen(f.buf) == sizeof(f.buf) - 1);
    check("  and keeps the part that fit", !strncmp(f.buf, "images/", 7));

    /* One past the end is the case an off-by-one gets wrong. */
    fence(&f);
    r = mr_vfs_cat(f.buf, (int)sizeof(f.buf), "0123456789012345", "6789012345678901");
    check("one character too many is truncation", r == 0);
    check("  stays inside the buffer", intact(&f));
    check("  NUL terminates", strlen(f.buf) == sizeof(f.buf) - 1);

    printf("\n-- join, and the separator it must not invent --\n");

    r = mr_vfs_join(big, (int)sizeof(big), "/home/game", "images/bg.png");
    check("join inserts a separator", r == 1 &&
          !strcmp(big, "/home/game/images/bg.png"));

    r = mr_vfs_join(big, (int)sizeof(big), "/home/game/", "images/bg.png");
    check("and does not double one that is there", r == 1 &&
          !strcmp(big, "/home/game/images/bg.png"));

    r = mr_vfs_join(big, (int)sizeof(big), "c:\\game\\", "images/bg.png");
    check("a backslash counts as a separator too", r == 1 &&
          !strcmp(big, "c:\\game\\images/bg.png"));

    r = mr_vfs_join(big, (int)sizeof(big), "", "images/bg.png");
    check("an empty dir adds no leading slash", r == 1 &&
          !strcmp(big, "images/bg.png"));

    /* mr_vfs_enum matches a prefix, not a directory: the archive branch
     * returns "images/bg_day.png" for the prefix "images/bg_", so the loose
     * branch must not turn it into "images/bg_/day.png". That is why enum
     * uses cat and open uses join, and getting it the other way round would
     * have been invisible until someone enumerated a non-directory prefix. */
    r = mr_vfs_cat(big, (int)sizeof(big), "images/bg_", "day.png");
    check("cat leaves a non-directory prefix alone", r == 1 &&
          !strcmp(big, "images/bg_day.png"));

    printf("\n-- the degenerate arguments --\n");

    fence(&f);
    r = mr_vfs_cat(f.buf, (int)sizeof(f.buf), NULL, NULL);
    check("two NULLs give an empty string", r == 1 && f.buf[0] == '\0');
    check("  and leave the fences alone", intact(&f));

    r = mr_vfs_cat(big, 1, "anything", "");
    check("a one-byte buffer holds only the terminator",
          r == 0 && big[0] == '\0');

    r = mr_vfs_cat(big, 0, "anything", "");
    check("a zero-byte buffer is refused, not written", r == 0);

    /* The real combination from vfs.c: the longest legal loose directory plus
     * the longest legal VFS name has to fit MR_FSPATH_MAX, or the engine
     * cannot open its own files. 191 + 1 + 255 = 447. */
    {
        char dir[192];
        char name[MR_PATH_MAX];
        memset(dir, 'd', sizeof(dir) - 1);  dir[sizeof(dir) - 1] = '\0';
        memset(name, 'n', sizeof(name) - 1); name[sizeof(name) - 1] = '\0';
        r = mr_vfs_join(big, (int)sizeof(big), dir, name);
        check("the worst legal loose path still fits MR_FSPATH_MAX",
              r == 1 && strlen(big) == 191 + 1 + 255);
    }

    printf("\n%s\n", fails ? "FAILURES" : "all ok");
    return fails ? 1 : 0;
}

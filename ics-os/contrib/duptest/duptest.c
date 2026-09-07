/*
 * duptest: guest self-test for the ICS-OS dup(2) syscall (0xC5).
 *
 * The kernel dup() was added to unblock the FEAT_TINY vim link, but `vim
 * --version` exits before vim's own dup(2), so test-vim only proves dup()
 * *links*. This self-test proves dup() actually *works* at runtime:
 *
 *   1. tty path: dup(1) must return a NEW descriptor (>= 3) that is closable.
 *      This is exactly the path vim takes at startup; closing a dup'd tty
 *      descriptor must release the slot (not return EBADF).
 *   2. file path: dup a live VFS file descriptor, write a marker THROUGH the
 *      duplicate, then read it back through the original. A duplicate that does
 *      not share the same open description -- or whose slot is unusable --
 *      fails the read-back, so the test fails for the original cause rather
 *      than a final success message.
 *
 * Prints DUPT_PASS on success, DUPT_FAIL + reason on failure.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static int fail(const char *msg)
{
    printf("duptest: FAIL %s errno=%d\n", msg, errno);
    printf("DUPT_FAIL\n");
    return 1;
}

#define DATA "DUPVFS_DATA"
#define DLEN (sizeof(DATA) - 1)

int main(void)
{
    const char *path = "/ramdisk/duptest.dat";
    int tfd, fd, fd2, n;
    char buf[16];

    printf("duptest: begin\n");

    /* 1. tty path (the one vim relies on): dup(1) yields a new, closable fd. */
    tfd = dup(1);
    if (tfd < 0)
        return fail("dup(1) tty");
    if (tfd <= 2)
        return fail("dup(1) tty did not allocate a new fd");
    if (close(tfd) != 0)
        return fail("close(dup'd tty)");
    printf("duptest: dup(1) tty -> fd %d, close OK\n", tfd);

    /* 2. file path: open, dup, write THROUGH the dup, read back. */
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return fail("open(ramdisk)");

    fd2 = dup(fd);
    if (fd2 < 0)
        return fail("dup(file)");
    if (fd2 <= 2 || fd2 == fd)
        return fail("dup(file) did not allocate a distinct new fd");
    printf("duptest: dup(file fd %d) -> fd %d OK\n", fd, fd2);

    /* Write the marker THROUGH the duplicate (not the original). */
    n = (int)write(fd2, DATA, DLEN);
    if (n != (int)DLEN)
        return fail("write() via duped file fd");

    /* Commit the dup'd-fd write so the bytes are visible on the shared file. */
    if (fsync(fd2) != 0)
        return fail("fsync() duped file fd");

    /* The duplicate shares the open description with the original: release it
       and confirm the bytes are visible reading back through fd. */
    if (close(fd2) != 0)
        return fail("close(dup'd file fd)");
    if (lseek(fd, 0, SEEK_SET) != 0)
        return fail("lseek");
    if (read(fd, buf, DLEN) != (int)DLEN)
        return fail("read() back via original fd");
    if (memcmp(buf, DATA, DLEN) != 0)
        return fail("read-back mismatch (dup did not share the file)");
    if (close(fd) != 0)
        return fail("close(original file fd)");
    printf("duptest: dup'd-fd write read back OK (%d bytes)\n", n);

    printf("duptest: all checks passed\n");
    printf("DUPT_PASS\n");
    return 0;
}

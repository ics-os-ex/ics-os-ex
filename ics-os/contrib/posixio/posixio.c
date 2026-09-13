/*
 * POSIX fd + preadv/pwritev/fsync + io_uring subset smoke test.
 * Writes to the in-kernel FAT ramdisk (CD is read-only).
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/io_uring.h>
#include <errno.h>

static int fail(const char *msg)
{
   printf("posixio: FAIL %s errno=%d\n", msg, errno);
   printf("POSIXIO_FAIL\n");
   return 1;
}

int main(void)
{
   const char *path = "/ramdisk/piotest.dat";
   const char *msg = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
   char a[8], b[8], c[32];
   struct iovec iov[2];
   struct stat st;
   int fd, n;
   struct io_uring ring;
   struct io_uring_sqe *sqe;
   struct io_uring_cqe *cqe;

   printf("posixio: open/write/preadv/pwritev/fsync\n");
   fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
   if (fd < 0)
      return fail("open");
   n = (int)write(fd, msg, 26);
   if (n != 26)
      return fail("write");
   if (fsync(fd) != 0)
      return fail("fsync");
   if (lseek(fd, 0, SEEK_SET) != 0)
      return fail("lseek");
   if (read(fd, c, 4) != 4 || memcmp(c, "ABCD", 4) != 0)
      return fail("read");

   iov[0].iov_base = a;
   iov[0].iov_len = 4;
   iov[1].iov_base = b;
   iov[1].iov_len = 4;
   n = (int)preadv(fd, iov, 2, 4);
   if (n != 8 || memcmp(a, "EFGH", 4) != 0 || memcmp(b, "IJKL", 4) != 0)
      return fail("preadv");
   /* file offset must be unchanged (after the 4-byte read). */
   if (lseek(fd, 0, SEEK_CUR) != 4)
      return fail("offset");

   iov[0].iov_base = (void *)"xxxx";
   iov[0].iov_len = 4;
   if (pwritev(fd, iov, 1, 10) != 4)
      return fail("pwritev");
   memset(c, 0, sizeof(c));
   if (pread(fd, c, 4, 10) != 4 || memcmp(c, "xxxx", 4) != 0)
      return fail("pread after pwritev");
   if (fstat(fd, &st) != 0 || st.st_size < 26)
      return fail("fstat");
   close(fd);

   printf("posixio: io_uring READ/WRITE/FSYNC\n");
   if (io_uring_queue_init(8, &ring, 0) != 0)
      return fail("uring setup");
   fd = open(path, O_RDWR);
   if (fd < 0)
      return fail("reopen");
   sqe = io_uring_get_sqe(&ring);
   if (!sqe)
      return fail("get_sqe");
   sqe->opcode = IORING_OP_READ;
   sqe->fd = fd;
   sqe->off = 0;
   sqe->addr = (uint64_t)(unsigned long)c;
   sqe->len = 4;
   sqe->user_data = 1;
   memset(c, 0, sizeof(c));
   if (io_uring_submit(&ring) != 1)
      return fail("uring submit");
   if (io_uring_wait_cqe(&ring, &cqe) != 0 || cqe->res != 4 ||
       memcmp(c, "ABCD", 4) != 0)
      return fail("uring read");
   io_uring_cqe_seen(&ring, cqe);

   sqe = io_uring_get_sqe(&ring);
   sqe->opcode = IORING_OP_FSYNC;
   sqe->fd = fd;
   sqe->user_data = 2;
   if (io_uring_submit(&ring) != 1)
      return fail("uring fsync submit");
   if (io_uring_wait_cqe(&ring, &cqe) != 0 || cqe->res != 0)
      return fail("uring fsync");
   io_uring_cqe_seen(&ring, cqe);

   sqe = io_uring_get_sqe(&ring);
   sqe->opcode = IORING_OP_NOP;
   sqe->user_data = 3;
   if (io_uring_submit_and_wait(&ring, 1) != 1)
      return fail("uring nop submit_and_wait");
   if (io_uring_wait_cqe(&ring, &cqe) != 0 || cqe->res != 0)
      return fail("uring nop");
   io_uring_cqe_seen(&ring, cqe);

   io_uring_queue_exit(&ring);
   close(fd);

   fd = open("/dev/vblk", O_RDWR);
   if (fd >= 0) {
      static char pa[512] __attribute__((aligned(16)));
      static char pb[512] __attribute__((aligned(16)));
      static char pc[512] __attribute__((aligned(16)));
      int i;
      printf("posixio: io_uring /dev/vblk DMA\n");
      if (io_uring_queue_init(8, &ring, 0) != 0)
         return fail("vblk uring setup");
      for (i = 0; i < 512; i++)
         pa[i] = (char)(0x5A ^ i);
      sqe = io_uring_get_sqe(&ring);
      sqe->opcode = IORING_OP_WRITE;
      sqe->fd = fd;
      sqe->off = 4096;
      sqe->addr = (uint64_t)(unsigned long)pa;
      sqe->len = 512;
      sqe->user_data = 10;
      if (io_uring_submit(&ring) != 1)
         return fail("vblk uring write submit");
      /* Closing with a DMA request in flight must drain the callback before
         freeing its ring. Verify the write after close as concrete evidence. */
      io_uring_queue_exit(&ring);
      memset(pb, 0, sizeof(pb));
      if (pread(fd, pb, 512, 4096) != 512 || memcmp(pa, pb, 512) != 0)
         return fail("vblk uring close drain");
      printf("URING_CLOSE_DRAIN_PASS\n");

      if (io_uring_queue_init(8, &ring, 0) != 0)
         return fail("vblk uring reopen");

      memset(pb, 0, sizeof(pb));
      memset(pc, 0, sizeof(pc));
      sqe = io_uring_get_sqe(&ring);
      sqe->opcode = IORING_OP_READ;
      sqe->fd = fd;
      sqe->off = 4096;
      sqe->addr = (uint64_t)(unsigned long)pb;
      sqe->len = 512;
      sqe->user_data = 11;
      sqe = io_uring_get_sqe(&ring);
      sqe->opcode = IORING_OP_READ;
      sqe->fd = fd;
      sqe->off = 4096;
      sqe->addr = (uint64_t)(unsigned long)pc;
      sqe->len = 512;
      sqe->user_data = 12;
      if (io_uring_submit_and_wait(&ring, 2) != 2)
         return fail("vblk uring pipelined read submit");
      if (io_uring_wait_cqe(&ring, &cqe) != 0 || cqe->res != 512)
         return fail("vblk uring read0");
      io_uring_cqe_seen(&ring, cqe);
      if (io_uring_wait_cqe(&ring, &cqe) != 0 || cqe->res != 512)
         return fail("vblk uring read1");
      io_uring_cqe_seen(&ring, cqe);
      if (memcmp(pa, pb, 512) != 0 || memcmp(pa, pc, 512) != 0)
         return fail("vblk uring mismatch");
      io_uring_queue_exit(&ring);
      close(fd);
      printf("URING_VBLK_PASS\n");
   }

   /* The target starts this executable again under a new PID. The second
      run proves this rejected create did not strand the global VFS lock. */
   fd = open("/ramdisk/bad name", O_RDWR | O_CREAT, 0666);
   if (fd >= 0) {
      close(fd);
      return fail("invalid filename accepted");
   }
   printf("VFS_CREATE_UNLOCK_PASS\n");

   /* Regression: make echoes long `ar rcs` lines through printf(). A 1024-byte
      stack vsprintf used to smash the return address (#GP on ret). */
   {
      char longbuf[2048];
      int i;
      for (i = 0; i < (int)sizeof(longbuf) - 1; i++)
         longbuf[i] = (char)('A' + (i % 26));
      longbuf[sizeof(longbuf) - 1] = 0;
      printf("%s\n", longbuf);
      printf("POSIXIO_PRINTF_LONG_OK\n");
   }

   printf("POSIXIO_PASS\n");
   printf("URING_PASS\n");
   return 0;
}

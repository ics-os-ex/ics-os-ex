/*
 * Concurrent FAT writes on virtio /work. Four children each write a unique
 * 512 KiB pattern while two siblings keep reading a seed file. The parallel
 * gcc self-host failed when unlocked FAT readers loadfat()'d into a cache
 * buffer a writer was still walking (truncated .s: `movq` became `q`).
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>

#define NWRITE   4
#define NREAD    2
#define FILE_SZ  (512 * 1024)
#define CHUNK    4096
#define READ_LOOPS 8

static int fail(const char *msg)
{
   printf("fatwr: FAIL %s errno=%d\n", msg, errno);
   printf("FATWR_FAIL\n");
   return 1;
}

static unsigned char pat(int id, unsigned long i)
{
   return (unsigned char)((id * 31u + (unsigned)i) & 0xFFu);
}

static int write_file(const char *path, int id)
{
   char buf[CHUNK];
   int fd, n;
   unsigned long off;

   fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
   if (fd < 0)
      return -1;
   for (off = 0; off < FILE_SZ; off += CHUNK) {
      unsigned long i;
      for (i = 0; i < CHUNK; i++)
         buf[i] = (char)pat(id, off + i);
      n = (int)write(fd, buf, CHUNK);
      if (n != CHUNK) {
         close(fd);
         return -1;
      }
   }
   if (close(fd) != 0)
      return -1;
   return 0;
}

static int verify_file(const char *path, int id)
{
   char buf[CHUNK];
   int fd, n;
   unsigned long off, i;

   fd = open(path, O_RDONLY);
   if (fd < 0) {
      printf("fatwr: open %s failed\n", path);
      return -1;
   }
   for (off = 0; off < FILE_SZ; off += CHUNK) {
      n = (int)read(fd, buf, CHUNK);
      if (n != CHUNK) {
         printf("fatwr: %s short read off=%lu n=%d\n", path, off, n);
         close(fd);
         return -1;
      }
      for (i = 0; i < CHUNK; i++) {
         if ((unsigned char)buf[i] != pat(id, off + i)) {
            printf("fatwr: %s mismatch off=%lu got=%u want=%u\n",
                   path, off + i, (unsigned)(unsigned char)buf[i],
                   (unsigned)pat(id, off + i));
            close(fd);
            return -1;
         }
      }
   }
   close(fd);
   return 0;
}

static int read_loop(const char *path)
{
   char buf[CHUNK];
   int loop, fd, n;
   unsigned long got;

   for (loop = 0; loop < READ_LOOPS; loop++) {
      fd = open(path, O_RDONLY);
      if (fd < 0)
         return -1;
      got = 0;
      while (got < FILE_SZ) {
         n = (int)read(fd, buf, CHUNK);
         if (n <= 0) {
            close(fd);
            return -1;
         }
         got += (unsigned long)n;
      }
      close(fd);
   }
   return 0;
}

int main(void)
{
   pid_t wpids[NWRITE], rpids[NREAD];
   char path[64];
   int i, st;

   printf("fatwr: %d writers + %d readers, %d bytes each on /work\n",
          NWRITE, NREAD, FILE_SZ);

   if (write_file("/work/seed.dat", 99) != 0)
      return fail("seed write");

   for (i = 0; i < NWRITE; i++) {
      pid_t p = fork();
      if (p < 0)
         return fail("fork write");
      if (p == 0) {
         sprintf(path, "/work/fw%d.dat", i);
         _exit(write_file(path, i) == 0 ? 0 : 1);
      }
      wpids[i] = p;
   }
   for (i = 0; i < NREAD; i++) {
      pid_t p = fork();
      if (p < 0)
         return fail("fork read");
      if (p == 0)
         _exit(read_loop("/work/seed.dat") == 0 ? 0 : 1);
      rpids[i] = p;
   }

   for (i = 0; i < NWRITE; i++) {
      if (waitpid(wpids[i], &st, 0) != wpids[i] || st != 0)
         return fail("writer status");
   }
   for (i = 0; i < NREAD; i++) {
      if (waitpid(rpids[i], &st, 0) != rpids[i] || st != 0)
         return fail("reader status");
   }
   if (verify_file("/work/seed.dat", 99) != 0)
      return fail("seed verify");
   for (i = 0; i < NWRITE; i++) {
      sprintf(path, "/work/fw%d.dat", i);
      if (verify_file(path, i) != 0)
         return fail("writer verify");
   }
   printf("FATWR_PASS\n");
   return 0;
}

/*
  memcorrupt.c - minimal in-OS user-memory corruption detector.

  Background: the strict GCC self-host cert (test-selfhost-cert) fails
  non-deterministically in the in-OS `as`: a valid .s on disk (the guest
  re-reads identical bytes, and host GNU `as` assembles it cleanly) is
  reported by the guest `as` as having errors at lines that are valid on
  disk, and the failing .s differs run to run. A single-threaded `as` with
  a different failing input each run points at kernel corruption of the `as`
  user address space, not a read-path/DMA bug.

  Three earlier shapes did NOT reproduce it: bulk 64 KiB IO; varying-size slab
  churn (freed each iter) with bulk IO; and small sequential reads with churn.
  The remaining difference from the `as` is its RESIDENT working set: it holds
  ~250k live slab blocks (symbol/frag tables, varying sizes, tens of MiB) plus
  a large output buffer at the same time, while walking the .s with small reads.
  This revision reproduces that resident shape:

    - one large contiguous buffer (the `as`'s output/section buffer)
    - N_BLOCKS live varying-size blocks (the `as`'s symbol/frag tables), kept
      for the whole run and re-verified
    - a seeded input file on /work walked with small sequential reads
      (the `as` per-line read shape)

  Both the large buffer and the live blocks are re-verified periodically, so
  the first mismatch is reported with its region, offset, and bytes.
*/
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

#define BIG_MB        8
#define BIG_SIZE      ((size_t)BIG_MB * 1024 * 1024)
#define N_BLOCKS      250000
#define SEED_FILE     "/work/MEMCORR.seed"
#define SEED_SIZE     ((size_t)4 * 1024 * 1024)
#define READ_CHUNK    128
#define N_CHUNKS      (SEED_SIZE / READ_CHUNK)   /* 32768 small reads */
#define VERIFY_EVERY  4096                        /* re-verify both regions every N chunks */

static unsigned char big_expect(size_t i)
{
   return (unsigned char)(((i * 7u + 13u) ^ (i >> 3)) & 0xFFu);
}

static unsigned char block_expect(unsigned blk, size_t off)
{
   return (unsigned char)(((blk * 31u + off * 5u + 101u) ^ (off >> 2)) & 0xFFu);
}

static unsigned char seed_expect(size_t i)
{
   return (unsigned char)(((i * 131u + 17u) ^ (i >> 4)) & 0xFFu);
}

static int big_check(const unsigned char *b, long pass)
{
   size_t i;
   for (i = 0; i < BIG_SIZE; i++) {
      if (b[i] != big_expect(i)) {
         printf("MEMCORR_CORRUPT big pass=%ld offset=%lu got=0x%02x want=0x%02x\n",
                pass, (unsigned long)i, b[i], big_expect(i));
         return 0;
      }
   }
   return 1;
}

static int blocks_check(unsigned char **blks, const size_t *sizes, long pass)
{
   unsigned blk;
   size_t off;
   for (blk = 0; blk < N_BLOCKS; blk++) {
      const unsigned char *p = blks[blk];
      for (off = 0; off < sizes[blk]; off++) {
         if (p[off] != block_expect(blk, off)) {
            printf("MEMCORR_CORRUPT block pass=%d blk=%u off=%lu got=0x%02x want=0x%02x\n",
                   pass, blk, (unsigned long)off, p[off], block_expect(blk, off));
            return 0;
         }
      }
   }
   return 1;
}

int main()
{
   unsigned char *big, *seed, *rd;
   unsigned char **blks;
   size_t *sizes;
   size_t off, w, i;
   int fd, pass = 0;
   long chunk;
   unsigned blk;
   size_t total_blocks = 0;

   printf("MEMCORR_BEGIN big_mb=%d nblocks=%d seed=%lu nchunks=%lu\n",
          BIG_MB, N_BLOCKS, (unsigned long)SEED_SIZE, (unsigned long)N_CHUNKS);

   big  = (unsigned char *)malloc(BIG_SIZE);
   if (!big) { printf("MEMCORR_FAIL malloc big\n"); return 1; }
   seed = (unsigned char *)malloc(SEED_SIZE);
   if (!seed) { printf("MEMCORR_FAIL malloc seed\n"); return 1; }
   rd   = (unsigned char *)malloc(READ_CHUNK);
   if (!rd) { printf("MEMCORR_FAIL malloc rd\n"); return 1; }
   blks = (unsigned char **)malloc((size_t)N_BLOCKS * sizeof(unsigned char *));
   if (!blks) { printf("MEMCORR_FAIL malloc blks\n"); return 1; }
   sizes = (size_t *)malloc((size_t)N_BLOCKS * sizeof(size_t));
   if (!sizes) { printf("MEMCORR_FAIL malloc sizes\n"); return 1; }

   for (i = 0; i < BIG_SIZE; i++) big[i] = big_expect(i);
   for (i = 0; i < SEED_SIZE; i++) seed[i] = seed_expect(i);

   /* Allocate N_BLOCKS live varying-size blocks (the `as` symbol/frag tables)
      and fill each with a deterministic pattern. */
   for (blk = 0; blk < N_BLOCKS; blk++) {
      size_t sz = 16 + (size_t)(((unsigned)blk * 2654435761u) % 480); /* 16..495 */
      sizes[blk] = sz;
      total_blocks += sz;
      blks[blk] = (unsigned char *)malloc(sz);
      if (!blks[blk]) { printf("MEMCORR_FAIL malloc blk=%u sz=%lu\n", blk, (unsigned long)sz); return 1; }
      for (i = 0; i < sz; i++) blks[blk][i] = block_expect(blk, i);
   }
   printf("MEMCORR_BLOCKS_OK total=%lu bytes\n", (unsigned long)total_blocks);

   if (!big_check(big, 0) || !blocks_check(blks, sizes, 0)) {
      printf("MEMCORR_FAIL init check\n");
      return 1;
   }
   printf("MEMCORR_PATTERN_OK\n");

   /* Seed the input file on /work (the .s equivalent). */
   fd = open(SEED_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
   if (fd < 0) { printf("MEMCORR_FAIL open seed write\n"); return 1; }
   off = 0;
   while (off < SEED_SIZE) {
      size_t c = (SEED_SIZE - off > (size_t)(READ_CHUNK * 16)) ? (size_t)(READ_CHUNK * 16) : (SEED_SIZE - off);
      w = 0;
      while (w < c) { int k = (int)write(fd, seed + off + w, c - w); if (k <= 0) break; w += k; }
      if (w == 0) { printf("MEMCORR_FAIL seed write short\n"); return 1; }
      off += w;
   }
   close(fd);
   printf("MEMCORR_SEED_OK\n");

   /* Walk the seed file with small sequential reads (the .s line shape),
      re-verifying the large buffer and the live blocks periodically. */
   fd = open(SEED_FILE, O_RDONLY);
   if (fd < 0) { printf("MEMCORR_FAIL open read\n"); return 1; }
   for (chunk = 0; chunk < (long)N_CHUNKS; chunk++) {
      size_t base = (size_t)chunk * READ_CHUNK;
      int k;

      k = (int)read(fd, rd, READ_CHUNK);
      if (k <= 0) { printf("MEMCORR_FAIL read short chunk=%ld k=%d\n", chunk, k); return 1; }

      /* the read content must match the seed pattern (valid read path) */
      for (i = 0; i < (size_t)k; i++) {
         size_t idx = base + i;
         if (rd[i] != seed_expect(idx)) {
            printf("MEMCORR_SEEDBAD chunk=%ld idx=%lu got=0x%02x want=0x%02x\n",
                   chunk, (unsigned long)idx, rd[i], seed_expect(idx));
            return 1;
         }
      }

      if ((chunk & (VERIFY_EVERY - 1)) == (VERIFY_EVERY - 1)) {
         pass++;
         if (!big_check(big, pass)) { printf("MEMCORR_FAIL chunk=%ld\n", chunk); return 1; }
         if (!blocks_check(blks, sizes, pass)) { printf("MEMCORR_FAIL chunk=%ld\n", chunk); return 1; }
      }
   }
   close(fd);

   if (!big_check(big, pass) || !blocks_check(blks, sizes, pass)) {
      printf("MEMCORR_FAIL final\n");
      return 1;
   }

   printf("MEMCORR_PASS chunks=%d nblocks=%d\n", N_CHUNKS, N_BLOCKS);
   return 0;
}

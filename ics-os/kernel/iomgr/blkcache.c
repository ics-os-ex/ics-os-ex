/*
 * 4KiB page cache + read-merge (I/O P2).
 *
 * Pages are indexed by (deviceid, byte_offset >> 12) so 512-byte ATA/virtio
 * and 2048-byte CD LBAs share one cache. A miss reads aligned 4KiB runs
 * (blk-mq merge). Writes are write-back; disk_mgr / fclose flush dirty pages.
 *
 * Lock order: per-device blk_mq lock, then pc_busy. Never hold pc_busy
 * across a device transfer.
 */
#include "../dextypes.h"
#include "../devmgr/dex32_devmgr.h"
#include "../process/sync.h"
#include "blkcache.h"

extern void *malloc(unsigned int);
extern void free(void *);
extern void *memcpy(void *, const void *, unsigned int);
extern void *memset(void *, int, unsigned int);
extern void printf(const char *fmt, ...);
extern void blk_mq_lock(int deviceid);
extern void blk_mq_unlock(int deviceid);

#define PC_SHIFT    12
#define PC_SIZE     (1u << PC_SHIFT)
#define PC_NPAGES   512
#define PC_UPTODATE 1
#define PC_DIRTY    2
#define PC_MAX_IO   (32u * PC_SIZE)

typedef struct {
   int   deviceid;
   u64   index;
   DWORD age;
   DWORD generation;
   DWORD device_generation;
   BYTE  flags;
   BYTE  data[PC_SIZE];
} pc_page;

static pc_page *pc_tab;
static DWORD pc_bsz[MAXDEVICES];
static DWORD pc_bsz_generation[MAXDEVICES];
static DWORD pc_clock;
static DWORD pc_hits, pc_misses, pc_fills, pc_merged;
static DWORD pc_dirty;
static sync_sharedvar pc_busy;

void blkcache_init(void)
{
   int i;
   memset(&pc_busy, 0, sizeof(pc_busy));
   memset(pc_bsz, 0, sizeof(pc_bsz));
   memset(pc_bsz_generation, 0, sizeof(pc_bsz_generation));
   pc_tab = (pc_page *)malloc(sizeof(pc_page) * PC_NPAGES);
   if (!pc_tab) {
      printf("pagecache: alloc failed\n");
      return;
   }
   for (i = 0; i < PC_NPAGES; i++) {
      pc_tab[i].flags = 0;
      pc_tab[i].deviceid = -1;
      pc_tab[i].index = ~(u64)0;
      pc_tab[i].generation = 0;
      pc_tab[i].device_generation = 0;
   }
   pc_clock = 1;
   pc_hits = pc_misses = pc_fills = pc_merged = 0;
   pc_dirty = 0;
   printf("pagecache: %u x %u KiB\n", PC_NPAGES, PC_SIZE / 1024);
}

static int pc_valid_bsize(DWORD s)
{
   return s == 512 || s == 1024 || s == 2048 || s == 4096;
}

static DWORD pc_bsize(int deviceid)
{
   devmgr_block_desc *b;
   DWORD s,generation=0;
   if (deviceid < 0 || deviceid >= MAXDEVICES)
      return 512;
   if (pc_bsz[deviceid] &&
       pc_bsz_generation[deviceid]==devmgr_get_generation(deviceid))
      return pc_bsz[deviceid];
   b = (devmgr_block_desc *)devmgr_getdevice_ref(deviceid);
   s = 512;
   if (b != (devmgr_block_desc *)-1) {
      generation=devmgr_get_generation(deviceid);
   }
   if (b != (devmgr_block_desc *)-1 && b->get_block_size) {
      int got = b->get_block_size();
      if (pc_valid_bsize((DWORD)got))
         s = (DWORD)got;
   }
   devmgr_putdevice((devmgr_generic *)b);
   if (generation && pc_valid_bsize(s)) {
      pc_bsz[deviceid] = s;
      pc_bsz_generation[deviceid]=generation;
   }
   return s;
}

static void pc_remember_bsize(int deviceid)
{
   devmgr_block_desc *b;
   DWORD s,generation;
   if (deviceid < 0 || deviceid >= MAXDEVICES)
      return;
   b = (devmgr_block_desc *)devmgr_getdevice_ref(deviceid);
   if (b == (devmgr_block_desc *)-1 || !b->get_block_size) {
      devmgr_putdevice((devmgr_generic *)b);
      return;
   }
   generation=devmgr_get_generation(deviceid);
   s = (DWORD)b->get_block_size();
   devmgr_putdevice((devmgr_generic *)b);
   if (generation && pc_valid_bsize(s)) {
      pc_bsz[deviceid] = s;
      pc_bsz_generation[deviceid]=generation;
   }
}

static int pc_slot(int deviceid, u64 index)
{
   DWORD h = (DWORD)index * 2654435761u + (DWORD)(index >> 32) * 2246822519u
             + (DWORD)deviceid * 40503u;
   return (int)(h % PC_NPAGES);
}

static pc_page *pc_lookup(int deviceid, u64 index)
{
   int s = pc_slot(deviceid, index);
   int probe, i;
   DWORD device_generation=devmgr_get_generation(deviceid);

   /* Fast path: linear probe of the hash neighborhood. */
   for (probe = 0; probe < 8; probe++) {
      int idx = (s + probe) % PC_NPAGES;
      if ((pc_tab[idx].flags & PC_UPTODATE) &&
          pc_tab[idx].deviceid == deviceid &&
          blkcache_device_key_is_current(pc_tab[idx].device_generation,
                                         device_generation) &&
          pc_tab[idx].index == index)
         return &pc_tab[idx];
   }
   /*
    * pc_claim may place a line anywhere in the table when the 8-slot
    * neighborhood is full of dirty pages (full scan for a clean victim).
    * A lookup that only probes 8 slots then misses, refills zeros from
    * disk, and drops concurrent 16 KiB cluster writes (fatwr offset 81920).
    */
   for (i = 0; i < PC_NPAGES; i++) {
      if ((pc_tab[i].flags & PC_UPTODATE) &&
          pc_tab[i].deviceid == deviceid &&
          blkcache_device_key_is_current(pc_tab[i].device_generation,
                                         device_generation) &&
          pc_tab[i].index == index)
         return &pc_tab[i];
   }
   return 0;
}

/* Replace only empty or clean pages. Never steal a dirty line. */
static pc_page *pc_claim(int deviceid, u64 index)
{
   int s = pc_slot(deviceid, index);
   int probe, idx = -1;
   DWORD oldest = ~(DWORD)0;
   int i;
   pc_page *p;

   p = pc_lookup(deviceid, index);
   if (p)
      return p;

   for (probe = 0; probe < 8; probe++) {
      int j = (s + probe) % PC_NPAGES;
      if (!(pc_tab[j].flags & PC_UPTODATE)) {
         idx = j;
         break;
      }
      if (!(pc_tab[j].flags & PC_DIRTY) && pc_tab[j].age < oldest) {
         oldest = pc_tab[j].age;
         idx = j;
      }
   }
   if (idx < 0) {
      for (i = 0; i < PC_NPAGES; i++) {
         if (!(pc_tab[i].flags & PC_UPTODATE)) {
            idx = i;
            break;
         }
         if (!(pc_tab[i].flags & PC_DIRTY) && pc_tab[i].age < oldest) {
            oldest = pc_tab[i].age;
            idx = i;
         }
      }
   }
   if (idx < 0)
      return 0;
   p = &pc_tab[idx];
   if (p->flags & PC_DIRTY)
      return 0;
   p->deviceid = deviceid;
   p->index = index;
   p->generation = 0;
   p->device_generation = devmgr_get_generation(deviceid);
   p->flags = 0;
   return p;
}

static int pc_copy_out(int deviceid, u64 byte_off, u64 nbytes, char *dst)
{
   u64 done = 0;
   while (done < nbytes) {
      u64 index = (byte_off + done) >> PC_SHIFT;
      DWORD poff = (DWORD)((byte_off + done) & (PC_SIZE - 1));
      DWORD n = PC_SIZE - poff;
      pc_page *p;
      if ((u64)n > nbytes - done)
         n = (DWORD)(nbytes - done);
      p = pc_lookup(deviceid, index);
       if (!p)
          return 0;
       memcpy(dst + (DWORD)done, p->data + poff, n);
       p->age = ++pc_clock;
       done += n;
   }
   return 1;
}

int blkcache_get(int deviceid, u64 sector, DWORD numblocks, void *buf)
{
   DWORD bsz;
   u64 bytes;
   int ok;
   if (!pc_tab || numblocks == 0 || !buf)
      return 0;
   bsz = pc_bsize(deviceid);
   bytes = (u64)numblocks * bsz;
   sync_entercrit(&pc_busy);
   ok = pc_copy_out(deviceid, sector * bsz, bytes, (char *)buf);
   if (ok)
      pc_hits += numblocks;
   sync_leavecrit(&pc_busy);
   return ok;
}

int blkcache_put(int deviceid, u64 sector, DWORD numblocks, void *buf)
{
   DWORD bsz;
   u64 byte_off, total, done = 0;
   char *src;
   if (!pc_tab || numblocks == 0 || !buf)
      return 0;
   bsz = pc_bsize(deviceid);
   byte_off = sector * bsz;
   total = (u64)numblocks * bsz;
   src = (char *)buf;
   sync_entercrit(&pc_busy);
   while (done < total) {
      u64 index = (byte_off + done) >> PC_SHIFT;
      DWORD poff = (DWORD)((byte_off + done) & (PC_SIZE - 1));
      DWORD n = PC_SIZE - poff;
      pc_page *p = pc_lookup(deviceid, index);
      if ((u64)n > total - done)
         n = (DWORD)(total - done);
      if (!p) {
         p = pc_claim(deviceid, index);
         if (!p) {
            sync_leavecrit(&pc_busy);
            return 0;
         }
         if (poff != 0 || n != PC_SIZE)
            memset(p->data, 0, PC_SIZE);
         p->flags = PC_UPTODATE;
         pc_fills++;
      }
      memcpy(p->data + poff, src + (DWORD)done, n);
      p->age = ++pc_clock;
      done += n;
   }
   sync_leavecrit(&pc_busy);
   return 1;
}

static int pc_dev_rw(int deviceid, int write, u64 lba, DWORD nsect, void *buf,
                     DWORD *device_generation)
{
   devmgr_block_desc *b = (devmgr_block_desc *)devmgr_getdevice_ref(deviceid);
   int retval=0;
   int last_context;
   DWORD generation;
   if (b == (devmgr_block_desc *)-1 || b->hdr.type != DEVMGR_BLOCK) {
      if (write)
         printf("pcache: write device lookup failed dev=%d ptr=%p type=%d\n",
                deviceid,b,b == (devmgr_block_desc *)-1 ? -1 : b->hdr.type);
      devmgr_putdevice((devmgr_generic *)b);
      return 0;
   }
   generation=devmgr_get_generation(deviceid);
   if (!generation || (device_generation && *device_generation &&
                       *device_generation!=generation)) {
      if (write)
         printf("pcache: write generation rejected dev=%d expected=%u current=%u\n",
             deviceid,device_generation ? (unsigned)*device_generation : 0,
             (unsigned)generation);
      devmgr_putdevice((devmgr_generic *)b);
      return 0;
   }
   if (device_generation)
      *device_generation=generation;
   last_context=devmgr_getcontext();
   devmgr_setcontext(b->hdr.id);
   if (write) {
      if (b->write_block)
         retval=b->write_block(lba,(char *)buf,nsect) > 0;
      else
         printf("pcache: device has no write callback dev=%d\n",deviceid);
   } else if (b->read_block) {
      retval=b->read_block(lba,(char *)buf,nsect) > 0;
   }
   devmgr_setcontext(last_context);
   devmgr_putdevice((devmgr_generic *)b);
   return retval;
}

static int pc_fill_range(int deviceid, u64 pg0, u64 pg1)
{
   DWORD bsz = pc_bsize(deviceid);
   u64 pg = pg0;
   DWORD sect_per = PC_SIZE / bsz;

   while (pg <= pg1) {
      u64 run0, run1, i;
      DWORD np, nsect;
      DWORD device_generation=0;
      char *bounce;

      sync_entercrit(&pc_busy);
      while (pg <= pg1 && pc_lookup(deviceid, pg))
         pg++;
      if (pg > pg1) {
         sync_leavecrit(&pc_busy);
         break;
      }
      run0 = pg;
      while (pg <= pg1 && !pc_lookup(deviceid, pg) &&
             (pg - run0 + 1) * PC_SIZE <= PC_MAX_IO)
         pg++;
      run1 = pg - 1;
      sync_leavecrit(&pc_busy);

      np = (DWORD)(run1 - run0 + 1);
      bounce = (char *)malloc(np * PC_SIZE);
      if (!bounce)
         return 0;
      nsect = np * sect_per;
      if (!pc_dev_rw(deviceid, 0, run0 * sect_per, nsect, bounce,
               &device_generation)) {
         free(bounce);
         return 0;
      }

      sync_entercrit(&pc_busy);
      pc_merged += nsect;
      for (i = 0; i < np; i++) {
         pc_page *p = pc_lookup(deviceid, run0 + i);
         if (p)
            continue;
         p = pc_claim(deviceid, run0 + i);
         if (!p)
            continue;
         p->device_generation=device_generation;
         memcpy(p->data, bounce + (DWORD)i * PC_SIZE, PC_SIZE);
         p->flags = PC_UPTODATE;
         p->age = ++pc_clock;
         pc_fills++;
      }
      sync_leavecrit(&pc_busy);
      free(bounce);
   }
   return 1;
}

int blkcache_read(int deviceid, u64 sector, DWORD numblocks, void *buf)
{
   DWORD bsz;
   u64 start, end, pg0, pg1;
   int ok;

   if (numblocks == 0 || !buf)
      return 0;
   if (!pc_tab)
      return pc_dev_rw(deviceid, 0, sector, numblocks, buf, 0);

   pc_remember_bsize(deviceid);
   bsz = pc_bsize(deviceid);
   start = sector * bsz;
   end = start + (u64)numblocks * bsz;
   pg0 = start >> PC_SHIFT;
   pg1 = (end - 1) >> PC_SHIFT;

  sync_entercrit(&pc_busy);
    ok = pc_copy_out(deviceid, start, end - start, (char *)buf);
    if (ok) {
       pc_hits += numblocks;
       sync_leavecrit(&pc_busy);
       return 1;
    }
    pc_misses += numblocks;
    sync_leavecrit(&pc_busy);

    if (!pc_fill_range(deviceid, pg0, pg1))
      return pc_dev_rw(deviceid, 0, sector, numblocks, buf, 0);

   sync_entercrit(&pc_busy);
   ok = pc_copy_out(deviceid, start, end - start, (char *)buf);
   sync_leavecrit(&pc_busy);
   if (ok)
      return 1;
   return pc_dev_rw(deviceid, 0, sector, numblocks, buf, 0);
}

int blkcache_write(int deviceid, u64 sector, DWORD numblocks, void *buf)
{
   DWORD bsz;
   u64 start, end, pg0, pg1, done = 0, total;
   char *src;
   int fail = 0;

   if (numblocks == 0 || !buf)
      return 0;
   if (!pc_tab)
      return pc_dev_rw(deviceid, 1, sector, numblocks, buf, 0);

   pc_remember_bsize(deviceid);
   bsz = pc_bsize(deviceid);
   start = sector * bsz;
   total = (u64)numblocks * bsz;
   end = start + total;
   pg0 = start >> PC_SHIFT;
   pg1 = (end - 1) >> PC_SHIFT;
   src = (char *)buf;

   if ((start & (PC_SIZE - 1)) != 0 || (end & (PC_SIZE - 1)) != 0) {
      if (!pc_fill_range(deviceid, pg0, pg1))
         return pc_dev_rw(deviceid, 1, sector, numblocks, buf, 0);
   }

   sync_entercrit(&pc_busy);
   while (done < total) {
      u64 index = (start + done) >> PC_SHIFT;
      DWORD poff = (DWORD)((start + done) & (PC_SIZE - 1));
      DWORD n = PC_SIZE - poff;
      pc_page *p;
      if ((u64)n > total - done)
         n = (DWORD)(total - done);
      p = pc_lookup(deviceid, index);
      if (!p) {
         p = pc_claim(deviceid, index);
         if (!p) {
            fail = 1;
            break;
         }
         if (poff != 0 || n != PC_SIZE)
            memset(p->data, 0, PC_SIZE);
         p->flags = PC_UPTODATE;
      }
      memcpy(p->data + poff, src + (DWORD)done, n);
      p->generation++;
      if (!(p->flags & PC_DIRTY))
         pc_dirty++;
      p->flags = PC_UPTODATE | PC_DIRTY;
      p->age = ++pc_clock;
      done += n;
   }
   sync_leavecrit(&pc_busy);

   if (fail)
      return pc_dev_rw(deviceid, 1, sector, numblocks, buf, 0);
   return 1;
}

int blkcache_has_dirty(void)
{
   return pc_dirty != 0;
}

int blkcache_flush(void)
{
   int i;
   if (!pc_tab)
      return 1;
   for (i = 0; i < PC_NPAGES; i++) {
      pc_page *p;
      DWORD bsz, nsect;
      DWORD generation;
      DWORD device_generation;
      int dev;
      u64 lba, index;
      BYTE copy[PC_SIZE];

      sync_entercrit(&pc_busy);
      p = &pc_tab[i];
      if (!(p->flags & (PC_DIRTY | PC_UPTODATE)) ||
          (p->flags & (PC_DIRTY | PC_UPTODATE)) != (PC_DIRTY | PC_UPTODATE)) {
         sync_leavecrit(&pc_busy);
         continue;
      }
      dev = p->deviceid;
      index = p->index;
      generation = p->generation;
      device_generation = p->device_generation;
      memcpy(copy, p->data, PC_SIZE);
      sync_leavecrit(&pc_busy);

      pc_remember_bsize(dev);
      if (!blkcache_device_key_is_current(device_generation,
                                          devmgr_get_generation(dev))) {
         sync_entercrit(&pc_busy);
         if (pc_tab[i].deviceid==dev && pc_tab[i].index==index &&
             pc_tab[i].device_generation==device_generation) {
            if (pc_tab[i].flags & PC_DIRTY && pc_dirty)
               pc_dirty--;
            pc_tab[i].flags=0;
            pc_tab[i].deviceid=-1;
         }
         sync_leavecrit(&pc_busy);
         return 0;
      }
      bsz = pc_bsize(dev);
      lba = index * (PC_SIZE / bsz);
      nsect = PC_SIZE / bsz;

      blk_mq_lock(dev);
      if (!pc_dev_rw(dev, 1, lba, nsect, copy, &device_generation)) {
         printf("pcache: flush failed dev=%d lba=%llu nb=%u page_gen=%u dev_gen=%u current_gen=%u\n",
                dev, (unsigned long long)lba, (unsigned)nsect,
                (unsigned)generation, (unsigned)device_generation,
                (unsigned)devmgr_get_generation(dev));
         blk_mq_unlock(dev);
         return 0;
      }
      blk_mq_unlock(dev);

      sync_entercrit(&pc_busy);
            if (blkcache_writeback_is_current(
               pc_tab[i].deviceid == dev && pc_tab[i].index == index,
               pc_tab[i].generation, generation) &&
          (pc_tab[i].flags & PC_DIRTY)) {
         pc_tab[i].flags &= (BYTE)~PC_DIRTY;
         if (pc_dirty)
            pc_dirty--;
      }
      sync_leavecrit(&pc_busy);
   }
   return 1;
}

int blkcache_invalidate_device(int deviceid)
{
   int i;
   int dirty=0;
   if (!pc_tab)
      return 0;
   sync_entercrit(&pc_busy);
   for (i = 0; i < PC_NPAGES; i++)
      if (pc_tab[i].deviceid == deviceid) {
         if (pc_tab[i].flags & PC_DIRTY) {
            pc_dirty--;
            dirty++;
         }
         pc_tab[i].flags = 0;
         pc_tab[i].deviceid = -1;
      }
   sync_leavecrit(&pc_busy);
   return dirty;
}

void blkcache_stats(DWORD *hits, DWORD *misses, DWORD *fills, DWORD *slots)
{
   if (hits) *hits = pc_hits;
   if (misses) *misses = pc_misses;
   if (fills) *fills = pc_fills;
   if (slots) *slots = PC_NPAGES;
}

void blkcache_reset_stats(void)
{
   pc_hits = pc_misses = pc_fills = pc_merged = 0;
}

void blkcache_mq_stats(DWORD *merged)
{
   if (merged)
      *merged = pc_merged;
}

//**************************************************************************
// DEX32 Disk drive scheduler
// April 3, 2003 by Joseph Emmanuel Dayo
// This currently implements a very simple first-come-first-served IO queue
//**************************************************************************

#include "iosched.h"
#include "../devmgr/dex32_devmgr.h"
#include "../hardware/chips/serial.h"
#include "../process/process.h"
#include "../stdlib/time.h"

// The registers that the io scheduler uses
int IOmgr_pause = 0;
int IOrequest_time = 0;
int flush_time = 10; /* Time in seconds before writes are flushed to the disk.
                        Only for devices that have caches*/
int flush_counter = 0;
int forceflush = 0;
IOrequest *IOjob;
IOrequest *cur;
int io_flushid = 0;
int flushing = 0, flushok = 1;
static DWORD io_mgr_pid = 0;

#define IOREQ_POOL_SIZE 128
#define IO_MERGE_MAX_BLOCKS 256
#define IO_CACHE_BLOCKS 64
#define IO_READAHEAD_BLOCKS 16
#define IO_READAHEAD_TRIGGER 8

static IOrequest io_pool[IOREQ_POOL_SIZE];
static IOrequest *io_free_list = 0;

typedef struct _IOReadCache {
  int valid;
  int deviceid;
  DWORD start_block;
  DWORD blocks;
  BYTE *data;
} IOReadCache;

static BYTE io_cache_data[MAX_DEVICE][IO_CACHE_BLOCKS * 512];
static IOReadCache io_cache[MAX_DEVICE];

static DWORD io_read_reqs = 0;
static DWORD io_write_reqs = 0;
static DWORD io_read_blocks = 0;
static DWORD io_write_blocks = 0;
static DWORD io_read_ticks = 0;
static DWORD io_write_ticks = 0;
static DWORD io_last_report_ticks = 0;
static DWORD io_last_report_reqs = 0;
static DWORD io_last_read_blocks = 0;
static DWORD io_last_write_blocks = 0;
static DWORD io_last_read_ticks = 0;
static DWORD io_last_write_ticks = 0;

static void io_perf_maybe_report() {
  DWORD now = getprecisetime();
  DWORD total_reqs = io_read_reqs + io_write_reqs;
  if ((total_reqs - io_last_report_reqs) < 512 &&
      (io_last_report_ticks == 0 || (now - io_last_report_ticks) < 2000))
    return;

  if (io_last_report_ticks == 0) {
    io_last_report_ticks = now;
    io_last_report_reqs = total_reqs;
    io_last_read_blocks = io_read_blocks;
    io_last_write_blocks = io_write_blocks;
    io_last_read_ticks = io_read_ticks;
    io_last_write_ticks = io_write_ticks;
  }
  {
    DWORD d_read_blocks = io_read_blocks - io_last_read_blocks;
    DWORD d_write_blocks = io_write_blocks - io_last_write_blocks;
    DWORD d_read_ticks = io_read_ticks - io_last_read_ticks;
    DWORD d_write_ticks = io_write_ticks - io_last_write_ticks;
    DWORD read_kbps = 0, write_kbps = 0;

    if (d_read_ticks)
      read_kbps = (d_read_blocks * 512 * 100) / (d_read_ticks * 1024);
    if (d_write_ticks)
      write_kbps = (d_write_blocks * 512 * 100) / (d_write_ticks * 1024);

    serial_printf("IO perf: r_reqs=%u w_reqs=%u r_kb/s=%u w_kb/s=%u "
                  "r_blocks=%u w_blocks=%u\n",
                  io_read_reqs, io_write_reqs, read_kbps, write_kbps,
                  io_read_blocks, io_write_blocks);

    io_last_report_ticks = now;
    io_last_report_reqs = total_reqs;
    io_last_read_blocks = io_read_blocks;
    io_last_write_blocks = io_write_blocks;
    io_last_read_ticks = io_read_ticks;
    io_last_write_ticks = io_write_ticks;
  }
}

sync_sharedvar IOrequest_busy;

void iomgr_setpid(DWORD pid) { io_mgr_pid = pid; }

static IOrequest *io_alloc_request() {
  IOrequest *ptr = 0;
  if (io_free_list) {
    ptr = io_free_list;
    io_free_list = io_free_list->next;
    memset(ptr, 0, sizeof(IOrequest));
    return ptr;
  }
  return (IOrequest *)malloc(sizeof(IOrequest));
}

static int io_is_pool_ptr(IOrequest *ptr) {
  return (ptr >= io_pool && ptr < (io_pool + IOREQ_POOL_SIZE));
}

static void io_free_request(IOrequest *ptr) {
  if (!ptr)
    return;
  if (io_is_pool_ptr(ptr)) {
    ptr->next = io_free_list;
    io_free_list = ptr;
    return;
  }
  free(ptr);
}

static int io_cache_hit(int deviceid, DWORD block, DWORD numblocks, void *buf) {
  if (deviceid < 0 || deviceid >= MAX_DEVICE)
    return 0;
  if (!io_cache[deviceid].valid)
    return 0;
  if (io_cache[deviceid].deviceid != deviceid)
    return 0;
  if (block < io_cache[deviceid].start_block)
    return 0;
  if ((block + numblocks) >
      (io_cache[deviceid].start_block + io_cache[deviceid].blocks))
    return 0;

  memcpy(buf,
         io_cache[deviceid].data +
             (block - io_cache[deviceid].start_block) * 512,
         numblocks * 512);
  return 1;
}

static void io_cache_store_and_readahead(int deviceid, DWORD block,
                                         DWORD numblocks, void *buf,
                                         devmgr_block_desc *myblock) {
  DWORD store_blocks;
  DWORD ahead_blocks = 0;

  if (deviceid < 0 || deviceid >= MAX_DEVICE)
    return;

  store_blocks = numblocks;
  if (store_blocks > IO_CACHE_BLOCKS)
    store_blocks = IO_CACHE_BLOCKS;

  memcpy(io_cache_data[deviceid], buf, store_blocks * 512);
  io_cache[deviceid].valid = 1;
  io_cache[deviceid].deviceid = deviceid;
  io_cache[deviceid].start_block = block;
  io_cache[deviceid].blocks = store_blocks;
  io_cache[deviceid].data = io_cache_data[deviceid];

  if (store_blocks <= IO_READAHEAD_TRIGGER && myblock && myblock->read_block) {
    ahead_blocks = IO_READAHEAD_BLOCKS;
    if (store_blocks + ahead_blocks > IO_CACHE_BLOCKS)
      ahead_blocks = IO_CACHE_BLOCKS - store_blocks;
    if (ahead_blocks > 0) {
      if (myblock->read_block(block + store_blocks,
                              (char *)io_cache_data[deviceid] +
                                  store_blocks * 512,
                              ahead_blocks)) {
        io_cache[deviceid].blocks = store_blocks + ahead_blocks;
      }
    }
  }
}

static int io_try_merge_request(int deviceid, int type, DWORD block,
                                DWORD numblocks, void *buf) {
  IOrequest *ptr;
  DWORD block_size = 512;
  DWORD new_end = block + numblocks;

  for (ptr = IOjob; ptr != 0; ptr = ptr->next) {
    if (ptr->status != IO_PENDING)
      continue;
    if (ptr->deviceid != deviceid)
      continue;
    if (ptr->type != type)
      continue;

    if (ptr->num_of_blocks + numblocks > IO_MERGE_MAX_BLOCKS)
      continue;

    // append
    if (ptr->lowblock + ptr->num_of_blocks == block &&
        (char *)buf == (char *)ptr->buf + (ptr->num_of_blocks * block_size)) {
      ptr->num_of_blocks += numblocks;
      return (int)ptr->rID;
    }

    // prepend
    if (new_end == ptr->lowblock &&
        (char *)buf + (numblocks * block_size) == (char *)ptr->buf) {
      ptr->lowblock = block;
      ptr->buf = buf;
      ptr->num_of_blocks += numblocks;
      return (int)ptr->rID;
    }
  }
  return 0;
}

// initializes all the data structures needed in this module
DWORD iomgr_init() {
  devmgr_iomgr iomgr;
  int i;
  IOjob = 0;
  cur = 0;
  memset(&IOrequest_busy, 0, sizeof(sync_sharedvar));
  memset(io_cache, 0, sizeof(io_cache));
  io_free_list = 0;
  for (i = 0; i < IOREQ_POOL_SIZE; i++) {
    io_pool[i].next = io_free_list;
    io_free_list = &io_pool[i];
  }

  strcpy(iomgr.hdr.name, "default_iomgr");
  strcpy(iomgr.hdr.description, "DEX default I/O scheduler and manager");
  iomgr.hdr.type = DEVMGR_IOMGR;
  iomgr.hdr.size = sizeof(devmgr_iomgr);
  iomgr.init = iomgr_init;
  iomgr.complete = dex32_IOcomplete;
  iomgr.close = dex32_closeIO;
  iomgr.request = dex32_requestIO;
  devmgr_register((devmgr_generic *)&iomgr);

  serial_printf("IO perf profiling enabled\n");
};

DWORD iomgr_flushmgr() {
  int ret;

  ret = devmgr_flushblocks();
  if (ret == -1)
    printf("Error writing to device %d. Data might be lost.\n", ret);
};

// this defines the threaded function which performs the actual
// reads and writes to the block device
DWORD iomgr_diskmgr() {
  IOrequest *ptr;
  char temp[10], temp2[10];
  char *vidmem = (char *)0xB8000;
  DWORD lastjob = 0;
  devmgr_block_desc *myblock;
  flush_counter = time();


  do {

    while (IOmgr_pause)
      ;
    ptr = IOmgr_obtainjob(0, 0, lastjob);
    if (ptr == 0) {
      // Commit writes to the disk if a specified time interval has
      // been met.
      if ((flush_counter - time() > flush_time) ||
          (time() - flush_counter > flush_time) || forceflush) {
        forceflush = 0;

        if (shouldflush())
          iomgr_flushmgr();
        flush_counter = time();
      };
      io_perf_maybe_report();
      continue;
    };

    if (ptr != 0) {
      do {
        // read or write data to the disk
        //    sigwait=current_process->processid;
        if (ptr->type == IO_READ) {
          DWORD t0 = getprecisetime();

#ifdef DEBUG_READ
          printf("dex32_diskmgr: Reading block %d..\n", ptr->lowblock);
#endif
          myblock = (devmgr_block_desc *)devmgr_devlist[ptr->deviceid];

          devmgr_setcontext(ptr->deviceid);

          if (myblock->hdr.type != DEVMGR_BLOCK) {
            printf("IO ERROR: Device %d is not a block device!\n",
                   ptr->deviceid);
          } else {
#ifdef DEBUG_READ
            printf("reading %d blocks starting at %d to location 0x%s.\n",
                   ptr->num_of_blocks, ptr->lowblock, itoa(ptr->buf, temp, 16));
#endif
            if (myblock->read_block(ptr->lowblock, ptr->buf,
                                    ptr->num_of_blocks)) {
              lastjob = ptr->lowblock;
              ptr->status = IO_COMPLETE;
              io_cache_store_and_readahead(ptr->deviceid, ptr->lowblock,
                                           ptr->num_of_blocks, ptr->buf,
                                           myblock);
            } else {
#ifdef DEBUG_READ
              printf("io_mgr(): read error?\n");
#endif
              ptr->status = IO_ERROR;
            };

#ifdef DEBUG_READ
            printf("dex32_diskmgr: read block Done..\n");
#endif

            io_read_reqs++;
            io_read_blocks += ptr->num_of_blocks;
            io_read_ticks += (getprecisetime() - t0);
          };

        } else if (ptr->type == IO_WRITE) {
          DWORD t0 = getprecisetime();
#ifdef DEBUG_IOREADWRITE
          printf("dex32_diskmgr: Writing block %d to device %d\n",
                 ptr->lowblock, ptr->deviceid);
#endif

          // obtain the device descriptor
          myblock = (devmgr_block_desc *)devmgr_devlist[ptr->deviceid];

          devmgr_setcontext(ptr->deviceid);

          if (myblock->hdr.type != DEVMGR_BLOCK) {
            printf("IO ERROR: Device %d is not a block device!\n",
                   ptr->deviceid);
          } else if (myblock->write_block == 0) {
            printf("IO ERROR: Device %d does not support writes!\n",
                   ptr->deviceid);
            ptr->status = IO_ERROR;
          } else if (myblock->write_block(ptr->lowblock, ptr->buf,
                                          ptr->num_of_blocks)) {
            lastjob = ptr->lowblock;
            ptr->status = IO_COMPLETE;
            if (ptr->deviceid >= 0 && ptr->deviceid < MAX_DEVICE &&
                io_cache[ptr->deviceid].valid) {
              DWORD w_start = ptr->lowblock;
              DWORD w_end = ptr->lowblock + ptr->num_of_blocks;
              DWORD c_start = io_cache[ptr->deviceid].start_block;
              DWORD c_end = io_cache[ptr->deviceid].start_block +
                            io_cache[ptr->deviceid].blocks;
              if (!(w_end <= c_start || w_start >= c_end))
                io_cache[ptr->deviceid].valid = 0;
            }
          } else
            ptr->status = IO_ERROR;
#ifdef DEBUG_IOREADWRITE
          printf("dex32_diskmgr: write block Done..\n");
#endif

          io_write_reqs++;
          io_write_blocks += ptr->num_of_blocks;
          io_write_ticks += (getprecisetime() - t0);
          ;
        }
        ptr = IOmgr_obtainjob(0, 0, lastjob);

      }

      while (ptr != 0);
    };

  } while (1);

  ;
};

// dequeue a request
IOrequest *IOmgr_obtainjob(int deviceid, DWORD lblockhigh,
                           DWORD lblocklow /*for optimization*/) {
  IOrequest *ptr, *tmp, *handlecand;
  DWORD mindist = 0xFFFFFFFF;

  // Wait until the IO manager is ready
  sync_entercrit(&IOrequest_busy);

  if (IOjob == 0) {
    sync_leavecrit(&IOrequest_busy);
    return 0;
  };

  ptr = IOjob;
  handlecand = IOjob;

  while (ptr != 0) {
    DWORD dist;

    if (lblocklow > ptr->lowblock)
      dist = lblocklow - ptr->lowblock;
    else
      dist = ptr->lowblock - lblocklow;

    if (dist < mindist) {
      handlecand = ptr;
      mindist = dist;
    };

    ptr = ptr->next;
    ;
  };

  ptr = handlecand;

  // remove the JOB from the queue and return
  if (ptr == IOjob)
    IOjob = ptr->next;

  if (ptr->prev != 0)
    ptr->prev->next = ptr->next;

  if (ptr->next != 0)
    ptr->next->prev = ptr->prev;

  sync_leavecrit(&IOrequest_busy);
  return ptr;
  ;
};

int dex32_IOcomplete(DWORD handle) {
  int retval;
  IOrequest *ptr;

  // wait until the I/O manager is ready
  sync_entercrit(&IOrequest_busy);

  ptr = (IOrequest *)handle;

  if (ptr->status == IO_COMPLETE)
    retval = 1;
  else if (ptr->status == IO_ERROR)
    retval = -1;
  else if (ptr->status == IO_PENDING) {
    retval = 0;
    sync_leavecrit(&IOrequest_busy);
    taskswitch();
    return retval;
  }
  //     else //ptr->status was given an unknown value, this is impossible
  // unless a process overwrites the IOrequest data structure
  //  printf("iomgr() data structure protection error\n");
  sync_leavecrit(&IOrequest_busy);

  return retval;

  ;
};

int dex32_waitIO(DWORD handle) {
  int res;
  if (!handle)
    return -1;
  do {
    res = dex32_IOcomplete(handle);
  } while (res == 0);
  return res;
  ;
};

void dex32_closeIO(DWORD handle) {
  IOrequest *ptr;

  // wait until the I/O manager is ready
  sync_entercrit(&IOrequest_busy);

  ptr = (IOrequest *)handle;
  io_free_request(ptr);

  sync_leavecrit(&IOrequest_busy);
  ;
};

DWORD dex32_requestIO(int deviceid, int type, DWORD block, DWORD numblocks,
                      void *buf) {
  IOrequest *ptr;
  devmgr_block_desc *myblock = (devmgr_block_desc *)devmgr_getdevice(deviceid);
  DWORD flags;


  // wait until the I/O manager is ready
  sync_entercrit(&IOrequest_busy);
  storeflags(&flags);
  stopints();
#ifdef DEBUG_IOREADWRITE2
  printf("R(");
#endif
  if (type == IO_READ && io_cache_hit(deviceid, block, numblocks, buf)) {
    ptr = io_alloc_request();
    ptr->rID = (DWORD)ptr;
    ptr->type = type;
    ptr->lowblock = block;
    ptr->num_of_blocks = numblocks;
    ptr->status = IO_COMPLETE;
    ptr->buf = buf;

    restoreflags(flags);
    sync_leavecrit(&IOrequest_busy);
    return (DWORD)ptr->rID;
  }

  if (myblock->getcache != 0)
    if (type == IO_READ && myblock->getcache(buf, block, numblocks)) {
      ptr = io_alloc_request();
      ptr->rID = (DWORD)ptr;
      ptr->type = type;
      ptr->lowblock = block;
      ptr->num_of_blocks = numblocks;
      ptr->status = IO_COMPLETE;
      ptr->buf = buf;

#ifdef DEBUG_IOREADWRITE2
      printf(")r\n");
#endif
      restoreflags(flags);
      sync_leavecrit(&IOrequest_busy);
      return (DWORD)ptr->rID;
    };

  if (myblock->putcache != 0)
    if (type == IO_WRITE && myblock->putcache(buf, block, numblocks)) {
      ptr = io_alloc_request();
      ptr->rID = (DWORD)ptr;
      ptr->type = type;
      ptr->lowblock = block;
      ptr->status = IO_COMPLETE;
      ptr->buf = buf;
      ptr->num_of_blocks = numblocks;

#ifdef DEBUG_IOREADWRITE2
      printf(")r\n");
#endif
      restoreflags(flags);
      sync_leavecrit(&IOrequest_busy);
      return (DWORD)ptr->rID;
    };

  // try to merge with existing pending request
  {
    int merged = io_try_merge_request(deviceid, type, block, numblocks, buf);
    if (merged) {
      restoreflags(flags);
      sync_leavecrit(&IOrequest_busy);
      if (io_mgr_pid)
        sigpriority = io_mgr_pid;
      return (DWORD)merged;
    }
  }

  // queue the request
  ptr = io_alloc_request();
  if (IOjob == 0) {
    IOjob = ptr;
    ptr->next = 0;
    ptr->prev = 0;
  } else {
    ptr->next = IOjob;
    ptr->prev = 0;
    IOjob->prev = ptr;
    IOjob = ptr;
  };

  ptr->deviceid = deviceid;
  ptr->rID = (DWORD)ptr;
  ptr->type = type;
  ptr->lowblock = block;
  ptr->status = IO_PENDING;
  ptr->buf = buf;
  ptr->time = IOrequest_time++;
  ptr->num_of_blocks = numblocks;
#ifdef DEBUG_IOREADWRITE2
  printf(")r\n");
#endif
  restoreflags(flags);
  sync_leavecrit(&IOrequest_busy);

  if (io_mgr_pid)
    sigpriority = io_mgr_pid;

  return (DWORD)ptr->rID;
};

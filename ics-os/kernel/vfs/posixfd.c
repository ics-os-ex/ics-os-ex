/*
 * POSIX file descriptors and io_uring (I/O P3 + async virtio completions).
 *
 * DEX fopen/fread stay as compat. New syscalls allocate per-process fds
 * wrapping file_PCB. Cache hits and ramdisk SQEs complete inline.
 * /dev/vblk READ/WRITE/FSYNC SQEs are submitted to virtio-blk and the
 * MSI-X harvest posts CQEs. io_uring_enter honors min_complete and
 * IORING_ENTER_GETEVENTS. Ring VA is params.sq_off.user_addr (no mmap).
 */
#include "../dextypes.h"
#include "../process/process.h"
#include "../process/scheduler.h"
#include "../console/tty.h"
#include "../iomgr/blkcache.h"
#include "../iomgr/iosched.h"
#include "../hardware/virtio/virtio.h"
#include "../hardware/virtio/virtio_blk.h"
#include "../cpu/spinlock.h"
#include "../process/completion.h"
#include "../stdlib/time.h"
#include "../net/sock.h"
#include "posixfd.h"

extern void *malloc(unsigned int);
extern void free(void *);
extern void *memset(void *s, int c, unsigned int n);
extern void *memcpy(void *d, const void *s, unsigned int n);
extern int strcmp(const char *s, const char *t);
extern void putc(char c);
extern int vfs_setbuffer(file_PCB *handle, char *buffer, int bufsize, int mode);
extern unsigned int ticks;
extern char *userspace;
extern char *showpath(char *s);
extern int dex32_loader(char *name, char *image, char *loadaddress, int mode,
                        char *p, char *workdir, PCB386 *parent);
extern int elf64_stream_load(char *name, int mode, char *p, char *workdir,
                             PCB386 *parent);
extern void dex32_stopints(DWORD *flags);
extern void dex32_restoreints(DWORD flags);
extern unsigned int strlen(const char *s);
extern int vfs_listdir(vfs_node *current_dir, vfs_node *buffer, int size);
extern PCB386 *sched_gethead(void);

#define SPAWN_CMDLINE 4096
#define WNOHANG    1

#define EPERM   1
#define ENOENT  2
#define EIO     5
#define ENOEXEC 8
#define EBADF   9
#define ECHILD  10
#define EAGAIN  11
#define ENOMEM  12
#define EINVAL  22
#define EMFILE  24
#define ENOSYS  38

#define URING_MAX 64
#define URING_QUEUED  1
#define URING_WAIT_TICKS 500

static DWORD next_wait_deadline(void)
{
   DWORD deadline=ticks+1;
   /* wait_for_completion_until reserves zero for an unbounded wait. */
   return deadline ? deadline : 1;
}
#define VBLK_MAX_XFER 4096

typedef struct ics_uring ics_uring;

typedef struct {
   ics_uring *ring;
   u64 user_data;
} uring_blk_cb;

typedef struct {
   u64 off;
   spinlock_t state_lock;
   sync_sharedvar io_busy;
   DWORD fd_refs;
   DWORD active_refs;
   DWORD closing;
} fd_blk_t;

typedef struct __attribute__((packed)) {
   BYTE opcode;
   BYTE flags;
   WORD ioprio;
   int fd;
   u64 off;
   u64 addr;
   DWORD len;
   DWORD rw_flags;
   u64 user_data;
   WORD buf_index;
   WORD personality;
   DWORD splice_fd_in;
   u64 addr3;
   u64 pad2;
} io_uring_sqe;

typedef struct __attribute__((packed)) {
   u64 user_data;
   int res;
   DWORD flags;
} io_uring_cqe;

struct ics_uring {
   DWORD sq_head;
   DWORD sq_tail;
   DWORD sq_ring_mask;
   DWORD sq_ring_entries;
   DWORD sq_flags;
   DWORD sq_dropped;
   DWORD cq_head;
   DWORD cq_tail;
   DWORD cq_ring_mask;
   DWORD cq_ring_entries;
   DWORD cq_overflow;
   DWORD pad;
   DWORD sq_array[URING_MAX];
   io_uring_cqe cqes[URING_MAX];
   io_uring_sqe sqes[URING_MAX];
   completion_t cq_event;
   uring_blk_cb blkcb[URING_MAX];
   spinlock_t lock;
   DWORD inflight;
   DWORD closing;
   DWORD fd_refs;
   DWORD active_refs;
   DWORD close_waiting;
};

static inline DWORD uring_load_acquire(volatile DWORD *p)
{
   return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static inline void uring_store_release(volatile DWORD *p, DWORD v)
{
   __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

typedef struct __attribute__((packed)) {
   DWORD head, tail, ring_mask, ring_entries, flags, dropped, array, resv1;
   u64 user_addr;
} io_sqring_offsets;

typedef struct __attribute__((packed)) {
   DWORD head, tail, ring_mask, ring_entries, overflow, cqes, flags, resv1;
   u64 user_addr;
} io_cqring_offsets;

typedef struct __attribute__((packed)) {
   DWORD sq_entries;
   DWORD cq_entries;
   DWORD flags;
   DWORD sq_thread_cpu;
   DWORD sq_thread_idle;
   DWORD features;
   DWORD wq_fd;
   DWORD resv[3];
   io_sqring_offsets sq_off;
   io_cqring_offsets cq_off;
} io_uring_params;

static int fd_alloc_slot(void)
{
   int i,fd=-EMFILE;
   if (!current_process)
      return -EMFILE;
   spin_lock(&current_process->fd_lock);
   for (i = 3; i < FD_MAX; i++)
      if (current_process->fds[i].type == FD_NONE) {
         current_process->fds[i].type=FD_RESERVED;
         current_process->fds[i].ptr=0;
         fd=i;
         break;
      }
   if (fd < 0) {
      /* Defensive recovery: if the table is full of leaked FD_RESERVED slots
         (a race between fd_alloc_slot and a concurrent modifier), reset them
         and retry once. This is a workaround for a known race; the proper fix
         is to identify and eliminate the concurrent modifier. */
      int any=0;
      for (i = 3; i < FD_MAX; i++)
         if (current_process->fds[i].type == FD_RESERVED) {
            any=1;
            break;
         }
      if (any) {
         for (i = 3; i < FD_MAX; i++)
            if (current_process->fds[i].type == FD_RESERVED) {
               current_process->fds[i].type=FD_NONE;
               current_process->fds[i].ptr=0;
            }
         for (i = 3; i < FD_MAX; i++)
            if (current_process->fds[i].type == FD_NONE) {
               current_process->fds[i].type=FD_RESERVED;
               current_process->fds[i].ptr=0;
               fd=i;
               break;
            }
      }
   }
   spin_unlock(&current_process->fd_lock);
   return fd;
}

static void fd_install(int fd,int type,void *ptr)
{
   spin_lock(&current_process->fd_lock);
   if (fd>=0 && fd<FD_MAX && current_process->fds[fd].type==FD_RESERVED) {
      current_process->fds[fd].ptr=ptr;
      current_process->fds[fd].type=type;
   }
   spin_unlock(&current_process->fd_lock);
}

static void fd_release_slot(int fd)
{
   spin_lock(&current_process->fd_lock);
   if (fd>=0 && fd<FD_MAX && current_process->fds[fd].type==FD_RESERVED) {
      current_process->fds[fd].type=FD_NONE;
      current_process->fds[fd].ptr=0;
   }
   spin_unlock(&current_process->fd_lock);
}

int sock_fd_alloc(void)
{
   return fd_alloc_slot();
}

void sock_fd_install(int fd, void *sock)
{
   fd_install(fd, FD_SOCK, sock);
}

void sock_fd_release(int fd)
{
   fd_release_slot(fd);
}

struct sock *sock_from_fd(int fd)
{
   void *ptr = 0;
   if (!current_process || fd < 0 || fd >= FD_MAX)
      return 0;
   spin_lock(&current_process->fd_lock);
   if (current_process->fds[fd].type == FD_SOCK)
      ptr = current_process->fds[fd].ptr;
   spin_unlock(&current_process->fd_lock);
   return (struct sock *)ptr;
}

static file_PCB *fd_file_get(int fd)
{
   file_PCB *f=0,*candidate;
   if (!current_process || fd < 0 || fd >= FD_MAX)
      return 0;
   spin_lock(&current_process->fd_lock);
   if (current_process->fds[fd].type == FD_VFS) {
      candidate=(file_PCB *)current_process->fds[fd].ptr;
      if (candidate && vfs_file_get(candidate))
         f=candidate;
   }
   spin_unlock(&current_process->fd_lock);
   return f;
}

static tty_t *fd_istty(int fd)
{
   int type;
   if (!current_process || fd < 0 || fd >= FD_MAX)
      return 0;
   spin_lock(&current_process->fd_lock);
   type = current_process->fds[fd].type;
   if (type == FD_TTY) {
      tty_t *t = (tty_t *)current_process->fds[fd].ptr;
      spin_unlock(&current_process->fd_lock);
      return t;
   }
   /* An installed non-tty (socket/file) on 0-2 wins over the ctty fallback. */
   if (type != FD_NONE && type != FD_RESERVED) {
      spin_unlock(&current_process->fd_lock);
      return 0;
   }
   spin_unlock(&current_process->fd_lock);
   if (current_process->ctty && fd >= 0 && fd < 3)
      return current_process->ctty;
   return 0;
}

tty_t *posix_fd_tty(int fd)
{
   return fd_istty(fd);
}

/* select() readiness probe: no refcounts, no blocking. Regular files and
   block devices report ready once open (same as Linux); ttys report
   readable only when input is pending. */
int posix_fd_selectable(int fd, int for_write)
{
   if (!current_process || fd < 0 || fd >= FD_MAX)
      return 0;
   {
      tty_t *t = fd_istty(fd);
      if (t)
         return for_write ? 1 : tty_input_ready(t);
   }
   {
      int type;
      spin_lock(&current_process->fd_lock);
      type = current_process->fds[fd].type;
      spin_unlock(&current_process->fd_lock);
      if (type == FD_VFS || type == FD_BLK)
         return 1;
   }
   return 0;
}

static int path_is_vblk(const char *p)
{
   return p && (strcmp(p, "/dev/vblk") == 0 || strcmp(p, "vblk") == 0);
}

static fd_blk_t *fd_blk_get(int fd)
{
   fd_blk_t *b=0,*candidate;
   if (!current_process || fd < 0 || fd >= FD_MAX)
      return 0;
   spin_lock(&current_process->fd_lock);
   if (current_process->fds[fd].type == FD_BLK) {
      candidate=(fd_blk_t *)current_process->fds[fd].ptr;
      if (candidate) {
         spin_lock(&candidate->state_lock);
         if (!candidate->closing) {
            candidate->active_refs++;
            b=candidate;
         }
         spin_unlock(&candidate->state_lock);
      }
   }
   spin_unlock(&current_process->fd_lock);
   return b;
}

static void fd_blk_put(fd_blk_t *b)
{
   int release=0;
   if (!b)
      return;
   spin_lock(&b->state_lock);
   if (b->active_refs)
      b->active_refs--;
   if (b->closing && !b->fd_refs && !b->active_refs)
      release=1;
   spin_unlock(&b->state_lock);
   if (release)
      free(b);
}

static int fd_blk_inherit(fd_blk_t *b)
{
   int ok=0;
   if (!b)
      return 0;
   spin_lock(&b->state_lock);
   if (!b->closing && b->fd_refs) {
      b->fd_refs++;
      ok=1;
   }
   spin_unlock(&b->state_lock);
   return ok;
}

static void fd_blk_close(fd_blk_t *b)
{
   int release=0;
   if (!b)
      return;
   spin_lock(&b->state_lock);
   if (b->fd_refs)
      b->fd_refs--;
   if (!b->fd_refs) {
      b->closing=1;
      if (!b->active_refs)
         release=1;
   }
   spin_unlock(&b->state_lock);
   if (release)
      free(b);
}

static long vblk_posix_rw(int write, u64 off, void *buf, unsigned long len)
{
   u64 cap, end;
   int n;

   if (!virtio_blk_present())
      return -EIO;
   if (!buf || len == 0)
      return 0;
   if ((off | (u64)len) & (VIRTIO_BLK_SECTOR_SIZE - 1))
      return -EINVAL;
   cap = virtio_blk_sectors() * (u64)VIRTIO_BLK_SECTOR_SIZE;
   if (off >= cap)
      return 0;
   end = off + len;
   if (end > cap)
      len = (unsigned long)(cap - off);
   if (len > VBLK_MAX_XFER)
      len = VBLK_MAX_XFER;
   if (write && virtio_blk_readonly())
      return -EIO;
   n = virtio_blk_rw(write, off, buf, (u32)len);
   return n;
}

static int posix_to_vfs_mode(int flags)
{
   if (flags & ICSOS_O_APPEND)
      return FILE_APPEND;
   if ((flags & ICSOS_O_ACCMODE) == ICSOS_O_RDWR)
      return FILE_READWRITE;
   if (flags & ICSOS_O_WRONLY)
      return FILE_WRITE;
   return FILE_READ;
}

long sys_open(const char *path, int flags, int mode)
{
   int fd, vmode;
   file_PCB *fcb;
   (void)mode;
   if (!path || !current_process)
     return -EINVAL;
   if (path_is_vblk(path)) {
     fd_blk_t *b;
     if (!virtio_blk_present())
         return -ENOENT;
      if ((flags & ICSOS_O_ACCMODE) != ICSOS_O_RDONLY &&
          virtio_blk_readonly())
         return -EIO;
      fd = fd_alloc_slot();
      if (fd < 0)
         return fd;
      b = (fd_blk_t *)malloc(sizeof(fd_blk_t));
      if (!b) {
         fd_release_slot(fd);
         return -ENOMEM;
      }
      b->off = 0;
      spin_init(&b->state_lock);
      memset(&b->io_busy,0,sizeof(b->io_busy));
      b->fd_refs=1;
      b->active_refs=0;
      b->closing=0;
      fd_install(fd,FD_BLK,b);
      return fd;
   }
   fd = fd_alloc_slot();
   if (fd < 0) {
      return fd;
   }
   vmode = posix_to_vfs_mode(flags);
   fcb = openfilex((char *)path, vmode);
   if (!fcb) {
      fd_release_slot(fd);
      return -ENOENT;
   }
   if (flags & ICSOS_O_DIRECT)
      vfs_setbuffer(fcb, 0, 0, FILE_IONBF);
   vfs_file_mark_posix(fcb);
   fd_install(fd,FD_VFS,fcb);
   return fd;
}

static void uring_free(ics_uring *r)
{
   if (r)
      free(r);
}

static void uring_put(ics_uring *r)
{
   int release=0;
   int wake=0;
   if (!r)
      return;
   spin_lock(&r->lock);
   if (r->active_refs)
      r->active_refs--;
      if (r->closing && !r->close_waiting && !r->fd_refs &&
         !r->active_refs && !r->inflight)
      release=1;
      if (r->closing)
         wake=1;
   spin_unlock(&r->lock);
      if (wake)
         complete_all(&r->cq_event);
   if (release)
      uring_free(r);
}

static ics_uring *uring_fdget(int fd)
{
   ics_uring *r=0,*candidate;
   if (!current_process || fd<0 || fd>=FD_MAX)
      return 0;
   spin_lock(&current_process->fd_lock);
   if (current_process->fds[fd].type==FD_URING) {
      candidate=(ics_uring*)current_process->fds[fd].ptr;
      if (candidate) {
         spin_lock(&candidate->lock);
         if (!candidate->closing) {
            candidate->active_refs++;
            r=candidate;
         }
         spin_unlock(&candidate->lock);
      }
   }
   spin_unlock(&current_process->fd_lock);
   return r;
}

static int uring_close(ics_uring *r)
{
   unsigned start;
   int last=0;

   if (!r)
      return 0;
   spin_lock(&r->lock);
   if (r->fd_refs)
      r->fd_refs--;
   if (!r->fd_refs) {
      r->closing=1;
      r->close_waiting=1;
      last=1;
   }
   spin_unlock(&r->lock);

   if (!last)
      return 0;

   start = ticks;
   for (;;) {
      DWORD inflight;
      virtio_blk_harvest();
      spin_lock(&r->lock);
      inflight = r->inflight;
      if (!inflight && !r->active_refs) {
         spin_unlock(&r->lock);
         uring_free(r);
         return 0;
      }
      reinit_completion(&r->cq_event);
      spin_unlock(&r->lock);
      if (ticks - start > URING_WAIT_TICKS) {
         int release=0;
         /* A close timeout must not leave a device-owned request retaining
            this process's user address. Reset retires DMA first; harvesting
            then performs this ring's EIO callbacks in the owner CR3. */
         (void)virtio_blk_force_reset();
         virtio_blk_harvest();
         spin_lock(&r->lock);
         r->close_waiting=0;
         if (!r->active_refs && !r->inflight)
            release=1;
         spin_unlock(&r->lock);
         if (release)
            uring_free(r);
         return -EIO;
      }
      wait_for_completion_until(&r->cq_event,next_wait_deadline());
   }
}

long sys_close(int fd)
{
   int type;
   void *ptr;
   if (!current_process || fd < 0 || fd >= FD_MAX)
      return -EBADF;
   spin_lock(&current_process->fd_lock);
   type=current_process->fds[fd].type;
   ptr=current_process->fds[fd].ptr;
   /* Stdio slots stay as ctty fallback unless a real object is installed. */
   if (fd < 3 && (type == FD_NONE || type == FD_RESERVED)) {
      spin_unlock(&current_process->fd_lock);
      return 0;
   }
   if (type==FD_NONE || type==FD_RESERVED) {
      spin_unlock(&current_process->fd_lock);
      return -EBADF;
   }
   current_process->fds[fd].type=FD_NONE;
   current_process->fds[fd].ptr=0;
   spin_unlock(&current_process->fd_lock);
   if (type==FD_VFS)
      return vfs_file_fdclose((file_PCB *)ptr);
   if (type==FD_URING)
      return uring_close((ics_uring *)ptr);
   if (type==FD_BLK) {
       fd_blk_close((fd_blk_t *)ptr);
       return 0;
    }
   if (type==FD_SOCK) {
      sock_close(ptr);
      return 0;
   }
   /* FD_TTY: the tty is a shared, never-destroyed resource (see sys_dup);
      closing a dup'd tty descriptor only releases the fd slot. */
   return 0;
}

long sys_dup(int oldfd)
{
    int nf, type;
    void *ptr;
    tty_t *t;
    if (!current_process || oldfd < 0 || oldfd >= FD_MAX)
       return -EBADF;
    spin_lock(&current_process->fd_lock);
    type=current_process->fds[oldfd].type;
    ptr=current_process->fds[oldfd].ptr;
    spin_unlock(&current_process->fd_lock);
    /* fd<3 resolves to the controlling tty via fd_istty(); the tty is a
       shared, never-destroyed resource so no reference count is taken. */
    t = fd_istty(oldfd);
    if (t) {
       nf = fd_alloc_slot();
       if (nf < 0)
          return nf;
       fd_install(nf,FD_TTY,t);
       return nf;
    }
    if (type==FD_VFS && ptr) {
       file_PCB *f=(file_PCB *)ptr;
       nf = fd_alloc_slot();
       if (nf < 0)
          return nf;
       if (!vfs_file_inherit(f)) {
          fd_release_slot(nf);
          return -EBADF;
       }
       fd_install(nf,FD_VFS,f);
       return nf;
    }
    if (type==FD_BLK && ptr) {
       fd_blk_t *b=(fd_blk_t *)ptr;
       nf = fd_alloc_slot();
       if (nf < 0)
          return nf;
       if (!fd_blk_inherit(b)) {
          fd_release_slot(nf);
          return -EBADF;
       }
       fd_install(nf,FD_BLK,b);
       return nf;
    }
    if (type==FD_SOCK && ptr) {
       nf = fd_alloc_slot();
       if (nf < 0)
          return nf;
       if (!sock_retain(ptr)) {
          fd_release_slot(nf);
          return -EBADF;
       }
       fd_install(nf, FD_SOCK, ptr);
       return nf;
    }
    return -EBADF;
}

/* Install an existing object into an arbitrary slot (including 0-2). */
static void fd_force_install(int fd, int type, void *ptr)
{
   spin_lock(&current_process->fd_lock);
   if (fd >= 0 && fd < FD_MAX) {
      current_process->fds[fd].ptr = ptr;
      current_process->fds[fd].type = type;
   }
   spin_unlock(&current_process->fd_lock);
}

long sys_dup2(int oldfd, int newfd)
{
   int type;
   void *ptr;
   tty_t *t;

   if (!current_process || oldfd < 0 || oldfd >= FD_MAX ||
       newfd < 0 || newfd >= FD_MAX)
      return -EBADF;
   if (oldfd == newfd)
      return newfd;

   spin_lock(&current_process->fd_lock);
   type = current_process->fds[oldfd].type;
   ptr = current_process->fds[oldfd].ptr;
   spin_unlock(&current_process->fd_lock);

   t = fd_istty(oldfd);
   if (t) {
      (void)sys_close(newfd);
      fd_force_install(newfd, FD_TTY, t);
      return newfd;
   }
   if (type == FD_SOCK && ptr) {
      if (!sock_retain(ptr))
         return -EBADF;
      (void)sys_close(newfd);
      fd_force_install(newfd, FD_SOCK, ptr);
      return newfd;
   }
   if (type == FD_VFS && ptr) {
      if (!vfs_file_inherit((file_PCB *)ptr))
         return -EBADF;
      (void)sys_close(newfd);
      fd_force_install(newfd, FD_VFS, ptr);
      return newfd;
   }
   if (type == FD_BLK && ptr) {
      if (!fd_blk_inherit((fd_blk_t *)ptr))
         return -EBADF;
      (void)sys_close(newfd);
      fd_force_install(newfd, FD_BLK, ptr);
      return newfd;
   }
   return -EBADF;
}

long sys_read(int fd, void *buf, long n)
{
   file_PCB *f;
   tty_t *t;
   int got;
   if (!buf || n <= 0)
      return 0;
   t = fd_istty(fd);
   if (t)
      return tty_read(t, (char *)buf, (int)n);
   {
      struct sock *sk = sock_from_fd(fd);
      if (sk)
         return sock_read(sk, buf, n);
   }
   {
      fd_blk_t *b = fd_blk_get(fd);
      if (b) {
         long got;
         sync_entercrit(&b->io_busy);
         got = vblk_posix_rw(0, b->off, buf, (unsigned long)n);
         if (got > 0)
            b->off += (u64)got;
         sync_leavecrit(&b->io_busy);
         fd_blk_put(b);
         return got;
      }
   }
   f = fd_file_get(fd);
   if (!f)
      return -EBADF;
   sync_entercrit(&f->io_busy);
   got = fread((char *)buf, 1, (int)n, f);
   sync_leavecrit(&f->io_busy);
   vfs_file_put(f);
   return got;
}

long sys_write(int fd, const void *buf, long n)
{
   file_PCB *f;
   tty_t *t;
   if (!buf || n <= 0)
      return 0;
   t = fd_istty(fd);
   if (t)
      return tty_write(t, (const char *)buf, (int)n);
   {
      struct sock *sk = sock_from_fd(fd);
      if (sk)
         return sock_write(sk, buf, n);
   }
   {
      fd_blk_t *b = fd_blk_get(fd);
      if (b) {
         long got;
         sync_entercrit(&b->io_busy);
         got = vblk_posix_rw(1, b->off, (void *)buf, (unsigned long)n);
         if (got > 0)
            b->off += (u64)got;
         sync_leavecrit(&b->io_busy);
         fd_blk_put(b);
         return got;
      }
   }
   f = fd_file_get(fd);
   if (f) {
      long written;
      sync_entercrit(&f->io_busy);
      written=fwrite((char *)buf, 1, (int)n, f);
      sync_leavecrit(&f->io_busy);
      vfs_file_put(f);
      return written;
   }
   if (buf && n > 0) {
      int i;
      for (i = 0; i < (int)n; i++)
         putc(((const char *)buf)[i]);
      return n;
   }
   return -EBADF;
}

long sys_lseek(int fd, long off, int whence)
{
   file_PCB *f;
   fd_blk_t *b = fd_blk_get(fd);
   if (b) {
      u64 cap = virtio_blk_sectors() * (u64)VIRTIO_BLK_SECTOR_SIZE;
      s64 noff;
      long result;
      sync_entercrit(&b->io_busy);
      if (whence == SEEK_SET)
         noff = off;
      else if (whence == SEEK_CUR)
         noff = (s64)b->off + off;
      else if (whence == SEEK_END)
         noff = (s64)cap + off;
      else {
         sync_leavecrit(&b->io_busy);
         fd_blk_put(b);
         return -EINVAL;
      }
      if (noff < 0) {
         sync_leavecrit(&b->io_busy);
         fd_blk_put(b);
         return -EINVAL;
      }
      b->off = (u64)noff;
      result=(long)b->off;
      sync_leavecrit(&b->io_busy);
      fd_blk_put(b);
      return result;
   }
   f = fd_file_get(fd);
   if (!f)
      return -EBADF;
   sync_entercrit(&f->io_busy);
   fseek(f, off, whence);
   off=ftell(f);
   sync_leavecrit(&f->io_busy);
   vfs_file_put(f);
   return off;
}

static long do_iov_rw(file_PCB *f, const struct k_iovec *iov, int iovcnt,
                      int write)
{
   long total = 0;
   int i;
   if (!iov || iovcnt <= 0)
      return -EINVAL;
   for (i = 0; i < iovcnt; i++) {
      long n;
      if (!iov[i].iov_base && iov[i].iov_len)
         return -EINVAL;
      if (iov[i].iov_len == 0)
         continue;
      if (write)
         n = fwrite((char *)iov[i].iov_base, 1, (int)iov[i].iov_len, f);
      else
         n = fread((char *)iov[i].iov_base, 1, (int)iov[i].iov_len, f);
      if (n < 0)
         return total ? total : -EIO;
      total += n;
      if ((unsigned long)n < iov[i].iov_len)
         break;
   }
   return total;
}

long sys_preadv(int fd, const struct k_iovec *iov, int iovcnt, long offset)
{
   file_PCB *f;
   fd_blk_t *b = fd_blk_get(fd);
   long total = 0;
   int i;

   if (b) {
      u64 off = (u64)offset;
      sync_entercrit(&b->io_busy);
      if (!iov || iovcnt <= 0)
         goto blk_invalid;
      for (i = 0; i < iovcnt; i++) {
         long n;
         if (!iov[i].iov_base && iov[i].iov_len)
            goto blk_invalid;
         if (iov[i].iov_len == 0)
            continue;
         n = vblk_posix_rw(0, off, iov[i].iov_base, iov[i].iov_len);
         if (n < 0)
            {
               long result=total ? total : n;
               sync_leavecrit(&b->io_busy);
               fd_blk_put(b);
               return result;
            }
         total += n;
         off += (u64)n;
         if ((unsigned long)n < iov[i].iov_len)
            break;
      }
      sync_leavecrit(&b->io_busy);
      fd_blk_put(b);
      return total;
   blk_invalid:
      sync_leavecrit(&b->io_busy);
      fd_blk_put(b);
      return -EINVAL;
   }
      f = fd_file_get(fd);
   {
      long old, n;
      if (!f)
         return -EBADF;
      sync_entercrit(&f->io_busy);
      old = ftell(f);
      fseek(f, offset, SEEK_SET);
      n = do_iov_rw(f, iov, iovcnt, 0);
      fseek(f, old, SEEK_SET);
      sync_leavecrit(&f->io_busy);
      vfs_file_put(f);
      return n;
   }
}

long sys_pwritev(int fd, const struct k_iovec *iov, int iovcnt, long offset)
{
   file_PCB *f;
   fd_blk_t *b = fd_blk_get(fd);
   long total = 0;
   int i;

   if (b) {
      u64 off = (u64)offset;
      sync_entercrit(&b->io_busy);
      if (!iov || iovcnt <= 0)
         goto blk_invalid;
      for (i = 0; i < iovcnt; i++) {
         long n;
         if (!iov[i].iov_base && iov[i].iov_len)
            goto blk_invalid;
         if (iov[i].iov_len == 0)
            continue;
         n = vblk_posix_rw(1, off, iov[i].iov_base, iov[i].iov_len);
         if (n < 0)
            {
               long result=total ? total : n;
               sync_leavecrit(&b->io_busy);
               fd_blk_put(b);
               return result;
            }
         total += n;
         off += (u64)n;
         if ((unsigned long)n < iov[i].iov_len)
            break;
      }
      sync_leavecrit(&b->io_busy);
      fd_blk_put(b);
      return total;
   blk_invalid:
      sync_leavecrit(&b->io_busy);
      fd_blk_put(b);
      return -EINVAL;
   }
      f = fd_file_get(fd);
   {
      long old, n;
      if (!f)
         return -EBADF;
      sync_entercrit(&f->io_busy);
      old = ftell(f);
      fseek(f, offset, SEEK_SET);
      n = do_iov_rw(f, iov, iovcnt, 1);
      fseek(f, old, SEEK_SET);
      sync_leavecrit(&f->io_busy);
      vfs_file_put(f);
      return n;
   }
}

long sys_fsync(int fd)
{
   file_PCB *f;
   fd_blk_t *b=fd_blk_get(fd);
   if (b) {
      long result;
      sync_entercrit(&b->io_busy);
      result=virtio_blk_flush();
      sync_leavecrit(&b->io_busy);
      fd_blk_put(b);
      return result;
   }
   f = fd_file_get(fd);
   if (!f)
      return -EBADF;
   sync_entercrit(&f->io_busy);
   if (!fflush(f)) {
      sync_leavecrit(&f->io_busy);
      vfs_file_put(f);
      return -EIO;
   }
   sync_leavecrit(&f->io_busy);
   vfs_file_put(f);
   if (!iomgr_flushmgr())
      return -EIO;
   return 0;
}

long sys_fstat_fd(int fd, void *statbuf)
{
   file_PCB *f = 0;
   fd_blk_t *b;
   vfs_stat vs;
   struct {
      unsigned int st_dev, st_ino, st_mode, st_nlink, st_uid, st_gid, st_rdev;
      long st_size;
      long st_atime, st_mtime, st_ctime;
   } *st = statbuf;
   if (!st)
      return -EINVAL;
   if (fd_istty(fd)) {
      memset(st, 0, sizeof(*st));
      st->st_mode = 0020000 | 0666; /* S_IFCHR */
      return 0;
   }
   b=fd_blk_get(fd);
   if (b) {
      memset(st, 0, sizeof(*st));
      st->st_mode = 0060000 | 0666; /* S_IFBLK */
      st->st_size = (long)(virtio_blk_sectors() * (u64)VIRTIO_BLK_SECTOR_SIZE);
      fd_blk_put(b);
      return 0;
   }
   f=fd_file_get(fd);
   if (!f)
      return -EBADF;
   memset(&vs, 0, sizeof(vs));
   vs.size = (int)sizeof(vs);
   sync_entercrit(&f->io_busy);
   if (fstat(f, &vs) < 0) {
      sync_leavecrit(&f->io_busy);
      vfs_file_put(f);
      return -EIO;
   }
   memset(st, 0, sizeof(*st));
   /* Build tools compare (st_dev, st_ino) to remove duplicate include and
      search directories. Returning zero for every VFS object collapsed
      GCC's complete include path to its first entry. Filesystem ID plus the
      stable mounted-node identity gives each live VFS object a real key. */
   if (f->ptr) {
      st->st_dev = f->ptr->fsid;
      st->st_ino = (unsigned int)((uintptr)f->ptr >> 4);
   }
   st->st_size = vs.st_size;
   /* Report the true file type so S_ISDIR/S_ISREG work. ld's
      find_scripts_dir() checks S_ISDIR to locate its ldscripts/
      directory; a blanket S_IFREG made the check fail and ld could
      not find its default linker script. */
   st->st_mode = (f->ptr && (f->ptr->attb & FILE_DIRECTORY))
                    ? (0040000 | 0777) /* S_IFDIR */
                    : (0100000 | 0777); /* S_IFREG */
   st->st_atime = vs.st_atime;
   st->st_mtime = vs.st_mtime;
   st->st_ctime = vs.st_ctime;
   /* ICS-OS has no hard-link support; every file has exactly one link.
    * GNU binutils smart_rename() only uses rename(2) when the destination
    * has st_nlink == 1; a zero value forces its copy-fallback path. */
   st->st_nlink = 1;
   sync_leavecrit(&f->io_busy);
   vfs_file_put(f);
   return 0;
}

void *sys_fd_file(int fd)
{
   file_PCB *f=0,*candidate;
   if (!current_process || fd<0 || fd>=FD_MAX)
      return 0;
   spin_lock(&current_process->fd_lock);
   if (current_process->fds[fd].type==FD_VFS) {
      candidate=(file_PCB *)current_process->fds[fd].ptr;
      if (candidate)
         f=candidate;
   }
   spin_unlock(&current_process->fd_lock);
   return f;
}

long sys_io_uring_setup(unsigned entries, void *params)
{
   io_uring_params *p = (io_uring_params *)params;
   ics_uring *r;
   int fd, i;
   unsigned n;

   if (!p || !current_process)
      return -EINVAL;
   n = entries;
   if (n < 1)
      n = 1;
   if (n > URING_MAX)
      n = URING_MAX;
   /* round up to power of two */
   {
      unsigned p2 = 1;
      while (p2 < n)
         p2 <<= 1;
      n = p2;
   }

   fd = fd_alloc_slot();
   if (fd < 0)
      return fd;
   r = (ics_uring *)malloc(sizeof(ics_uring));
   if (!r) {
      fd_release_slot(fd);
      return -ENOMEM;
   }
   memset(r, 0, sizeof(ics_uring));
   spin_init(&r->lock);
   completion_init(&r->cq_event);
   r->fd_refs = 1;
   r->sq_ring_mask = n - 1;
   r->sq_ring_entries = n;
   r->cq_ring_mask = n - 1;
   r->cq_ring_entries = n;
   for (i = 0; i < (int)n; i++)
      r->sq_array[i] = (DWORD)i;

   memset(p, 0, sizeof(*p));
   p->sq_entries = n;
   p->cq_entries = n;
   p->sq_off.head = (DWORD)((uintptr)&r->sq_head - (uintptr)r);
   p->sq_off.tail = (DWORD)((uintptr)&r->sq_tail - (uintptr)r);
   p->sq_off.ring_mask = (DWORD)((uintptr)&r->sq_ring_mask - (uintptr)r);
   p->sq_off.ring_entries = (DWORD)((uintptr)&r->sq_ring_entries - (uintptr)r);
   p->sq_off.flags = (DWORD)((uintptr)&r->sq_flags - (uintptr)r);
   p->sq_off.dropped = (DWORD)((uintptr)&r->sq_dropped - (uintptr)r);
   p->sq_off.array = (DWORD)((uintptr)&r->sq_array[0] - (uintptr)r);
   p->sq_off.user_addr = (u64)(uintptr)r;
   p->cq_off.head = (DWORD)((uintptr)&r->cq_head - (uintptr)r);
   p->cq_off.tail = (DWORD)((uintptr)&r->cq_tail - (uintptr)r);
   p->cq_off.ring_mask = (DWORD)((uintptr)&r->cq_ring_mask - (uintptr)r);
   p->cq_off.ring_entries = (DWORD)((uintptr)&r->cq_ring_entries - (uintptr)r);
   p->cq_off.overflow = (DWORD)((uintptr)&r->cq_overflow - (uintptr)r);
   p->cq_off.cqes = (DWORD)((uintptr)&r->cqes[0] - (uintptr)r);
   p->cq_off.user_addr = (u64)(uintptr)r;
   p->resv[0] = (DWORD)((uintptr)&r->sqes[0] - (uintptr)r);

   fd_install(fd,FD_URING,r);
   return fd;
}

static void uring_vblk_done(void *arg, int res)
{
   uring_blk_cb *cb = (uring_blk_cb *)arg;
   ics_uring *r;
   DWORD cq_tail;
   io_uring_cqe *cqe;
   int release=0;

   if (!cb || !cb->ring)
      return;
   r = cb->ring;
   spin_lock(&r->lock);
   cq_tail = uring_load_acquire(&r->cq_tail);
   if (cq_tail - uring_load_acquire(&r->cq_head) < r->cq_ring_entries) {
      cqe = &r->cqes[cq_tail & r->cq_ring_mask];
      cqe->user_data = cb->user_data;
      cqe->res = res;
      cqe->flags = 0;
      uring_store_release(&r->cq_tail, cq_tail + 1);
   } else {
      r->cq_overflow++;
   }
   cb->ring = 0;
   if (r->inflight)
      r->inflight--;
   complete_all(&r->cq_event);
      if (r->closing && !r->close_waiting && !r->fd_refs &&
         !r->active_refs && !r->inflight)
      release=1;
   spin_unlock(&r->lock);
   if (release)
      uring_free(r);
}

/* Caller holds r->lock. */
static int uring_alloc_cb(ics_uring *r)
{
   int i;
   for (i = 0; i < URING_MAX; i++)
      if (!r->blkcb[i].ring)
         return i;
   return -1;
}

static int uring_queue_vblk(ics_uring *r, io_uring_sqe *sqe,
                            u32 type, void *buf, u32 bytes, u64 off)
{
   int i, n;
   u64 sector;

   if ((off | (u64)bytes) & (VIRTIO_BLK_SECTOR_SIZE - 1))
      return -EINVAL;
   if (!virtio_blk_present())
      return -EIO;
   spin_lock(&r->lock);
   if (r->closing) {
      spin_unlock(&r->lock);
      return -EBADF;
   }
   i = uring_alloc_cb(r);
   if (i < 0) {
      spin_unlock(&r->lock);
      return -ENOMEM;
   }
   r->blkcb[i].ring = r;
   r->blkcb[i].user_data = sqe->user_data;
   r->inflight++;
   spin_unlock(&r->lock);
   sector = off / VIRTIO_BLK_SECTOR_SIZE;
   n = virtio_blk_submit(type, sector, buf, bytes,
                         uring_vblk_done, &r->blkcb[i], 0);
   if (n < 0) {
      spin_lock(&r->lock);
      if (r->blkcb[i].ring) {
         r->blkcb[i].ring = 0;
         if (r->inflight)
            r->inflight--;
      }
      spin_unlock(&r->lock);
      return n;
   }
   return URING_QUEUED;
}

/* 0 = not a vblk SQE (caller runs it inline). URING_QUEUED or -errno. */
static int uring_try_vblk(ics_uring *r, io_uring_sqe *sqe)
{
   int fd;
   int result;
   fd_blk_t *b;
   void *buf;
   u32 bytes, type;
   u64 off;

   if (!r || !sqe)
      return 0;
   fd = sqe->fd;
   if (!current_process || fd < 0 || fd >= FD_MAX)
      return 0;
   b=fd_blk_get(fd);
   if (!b)
      return 0;
   sync_entercrit(&b->io_busy);

   if (sqe->opcode == IORING_OP_FSYNC) {
      int i, n;
      spin_lock(&r->lock);
      if (r->closing) {
         spin_unlock(&r->lock);
         sync_leavecrit(&b->io_busy);
         fd_blk_put(b);
         return -EBADF;
      }
      i = uring_alloc_cb(r);
      if (i < 0) {
         spin_unlock(&r->lock);
         sync_leavecrit(&b->io_busy);
         fd_blk_put(b);
         return -ENOMEM;
      }
      r->blkcb[i].ring = r;
      r->blkcb[i].user_data = sqe->user_data;
      r->inflight++;
      spin_unlock(&r->lock);
      n = virtio_blk_submit(VIRTIO_BLK_T_FLUSH, 0, 0, 0,
                            uring_vblk_done, &r->blkcb[i], 0);
      if (n < 0) {
         spin_lock(&r->lock);
         if (r->blkcb[i].ring) {
            r->blkcb[i].ring = 0;
            if (r->inflight)
               r->inflight--;
         }
         spin_unlock(&r->lock);
         sync_leavecrit(&b->io_busy);
         fd_blk_put(b);
         return n;
      }
      sync_leavecrit(&b->io_busy);
      fd_blk_put(b);
      return URING_QUEUED;
   }

   if (sqe->opcode == IORING_OP_READ || sqe->opcode == IORING_OP_WRITE) {
      buf = (void *)(uintptr)sqe->addr;
      bytes = sqe->len;
      off = sqe->off;
      if (off == ~(u64)0)
         off = b->off;
      type = (sqe->opcode == IORING_OP_WRITE) ?
             VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
      result=uring_queue_vblk(r, sqe, type, buf, bytes, off);
      if (sqe->off == ~(u64)0) {
         if (result==URING_QUEUED)
            b->off+=bytes;
      }
      sync_leavecrit(&b->io_busy);
      fd_blk_put(b);
      return result;
   }
   if (sqe->opcode == IORING_OP_READV || sqe->opcode == IORING_OP_WRITEV) {
      const struct k_iovec *iov = (const struct k_iovec *)(uintptr)sqe->addr;
      if (!iov || sqe->len != 1) {
         sync_leavecrit(&b->io_busy);
         fd_blk_put(b);
         return 0;
      }
      type = (sqe->opcode == IORING_OP_WRITEV) ?
             VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
      result=uring_queue_vblk(r, sqe, type, iov[0].iov_base,
                              (u32)iov[0].iov_len, sqe->off);
      sync_leavecrit(&b->io_busy);
      fd_blk_put(b);
      return result;
   }
   sync_leavecrit(&b->io_busy);
   fd_blk_put(b);
   return 0;
}

static int uring_inherit(ics_uring *r, int for_fork)
{
   int ok=0;
   if (!r)
      return 0;
   spin_lock(&r->lock);
   if (!r->closing && r->fd_refs && (!for_fork || !r->inflight)) {
      r->fd_refs++;
      ok=1;
   }
   spin_unlock(&r->lock);
   return ok;
}

static int posix_fd_clone_mode(struct _PCB386 *dst, struct _PCB386 *src,
                               int for_fork)
{
   int fd,type,result=0;
   int hi=-1,nf=0;
   void *ptr;
   if (!dst || !src)
      return -EINVAL;
   spin_lock(&src->fd_lock);
   for (fd=0;fd<FD_MAX;fd++) {
      type=src->fds[fd].type;
      ptr=src->fds[fd].ptr;
      if (type==FD_RESERVED)
         continue;
      if (type==FD_VFS && !vfs_file_inherit((file_PCB *)ptr)) {
         result=-EAGAIN;
         break;
      }
      if (type==FD_BLK && !fd_blk_inherit((fd_blk_t *)ptr)) {
         result=-EAGAIN;
         break;
      }
      if (type==FD_URING && !uring_inherit((ics_uring *)ptr,for_fork)) {
         result=-EAGAIN;
         break;
      }
      if (type==FD_SOCK && !sock_retain(ptr)) {
         result=-EAGAIN;
         break;
      }
      dst->fds[fd].type=type;
      dst->fds[fd].ptr=ptr;
      if (hi < fd) hi=fd;
      nf++;
   }
   spin_unlock(&src->fd_lock);
   if (result)
      posix_fd_close_all(dst);
   return result;
}

int posix_fd_fork_ready(struct _PCB386 *src)
{
   int fd,result=0;

   if (!src)
      return -EINVAL;
   spin_lock(&src->fd_lock);
   for (fd=0;fd<FD_MAX;fd++) {
      ics_uring *ring;
      if (src->fds[fd].type!=FD_URING)
         continue;
      ring=(ics_uring *)src->fds[fd].ptr;
      if (!ring) {
         result=-EAGAIN;
         break;
      }
      spin_lock(&ring->lock);
      if (ring->closing || ring->inflight)
         result=-EAGAIN;
      spin_unlock(&ring->lock);
      if (result)
         break;
   }
   spin_unlock(&src->fd_lock);
   return result;
}

int posix_fd_clone(struct _PCB386 *dst, struct _PCB386 *src)
{
   return posix_fd_clone_mode(dst,src,0);
}

int posix_fd_clone_fork(struct _PCB386 *dst, struct _PCB386 *src)
{
   return posix_fd_clone_mode(dst,src,1);
}

void posix_fd_close_all(struct _PCB386 *process)
{
   int fd,type;
   void *ptr;
   if (!process)
      return;
   /* Descriptor objects may be shared with another process, but pending DMA
      copyback is owned by this address space and must never outlive it. */
   virtio_blk_retire_owner((u64)(uintptr)process->pagedirloc);
   for (fd=0;fd<FD_MAX;fd++) {
      spin_lock(&process->fd_lock);
      type=process->fds[fd].type;
      ptr=process->fds[fd].ptr;
      process->fds[fd].type=FD_NONE;
      process->fds[fd].ptr=0;
      spin_unlock(&process->fd_lock);
      if (type==FD_VFS)
         vfs_file_fdclose((file_PCB *)ptr);
      else if (type==FD_BLK)
         fd_blk_close((fd_blk_t *)ptr);
      else if (type==FD_URING)
         uring_close((ics_uring *)ptr);
      else if (type==FD_SOCK)
         sock_close(ptr);
   }
}

static int uring_do_sqe(io_uring_sqe *sqe)
{
   struct k_iovec iov;
   long n;
   u64 off;

   if (!sqe)
      return -EINVAL;
   switch (sqe->opcode) {
   case IORING_OP_NOP:
      return 0;
   case IORING_OP_FSYNC:
      return sys_fsync(sqe->fd);
   case IORING_OP_CLOSE:
      return sys_close(sqe->fd);
   case IORING_OP_OPENAT:
      return sys_open((const char *)(uintptr)sqe->addr,
                      (int)sqe->rw_flags, (int)sqe->len);
   case IORING_OP_READ:
   case IORING_OP_WRITE:
      iov.iov_base = (void *)(uintptr)sqe->addr;
      iov.iov_len = sqe->len;
      off = sqe->off;
      if (off == ~(u64)0) {
         if (sqe->opcode == IORING_OP_READ)
            return (int)sys_read(sqe->fd, iov.iov_base, (long)iov.iov_len);
         return (int)sys_write(sqe->fd, iov.iov_base, (long)iov.iov_len);
      }
      if (sqe->opcode == IORING_OP_READ)
         n = sys_preadv(sqe->fd, &iov, 1, (long)off);
      else
         n = sys_pwritev(sqe->fd, &iov, 1, (long)off);
      return (int)n;
   case IORING_OP_READV:
      n = sys_preadv(sqe->fd, (const struct k_iovec *)(uintptr)sqe->addr,
                     (int)sqe->len, (long)sqe->off);
      return (int)n;
   case IORING_OP_WRITEV:
      n = sys_pwritev(sqe->fd, (const struct k_iovec *)(uintptr)sqe->addr,
                      (int)sqe->len, (long)sqe->off);
      return (int)n;
   default:
      return -ENOSYS;
   }
}

long sys_io_uring_enter(int fd, unsigned to_submit, unsigned min_complete,
                       unsigned flags)
{
   ics_uring *r;
   unsigned i, done = 0;

   r=uring_fdget(fd);
   if (!r)
      return -EBADF;

   for (i = 0; i < to_submit; i++) {
      DWORD head = uring_load_acquire(&r->sq_head);
      DWORD tail = uring_load_acquire(&r->sq_tail);
      DWORD mask = r->sq_ring_mask;
      DWORD idx;
      io_uring_sqe *sqe;
      int v;

      if (head == tail)
         break;
      idx = r->sq_array[head & mask];
      if (idx > mask)
         idx &= mask;
      sqe = &r->sqes[idx];
      v = uring_try_vblk(r, sqe);
      if (v != URING_QUEUED) {
         io_uring_cqe *cqe;
         int res = (v < 0) ? v :
                   ((sqe->opcode == IORING_OP_CLOSE && sqe->fd == fd) ?
                    -EBADF : uring_do_sqe(sqe));
         DWORD cq_tail;
         spin_lock(&r->lock);
         cq_tail = uring_load_acquire(&r->cq_tail);
         if (cq_tail - uring_load_acquire(&r->cq_head) < r->cq_ring_entries) {
            cqe = &r->cqes[cq_tail & r->cq_ring_mask];
            cqe->user_data = sqe->user_data;
            cqe->res = res;
            cqe->flags = 0;
            uring_store_release(&r->cq_tail, cq_tail + 1);
            complete_all(&r->cq_event);
         } else {
            r->cq_overflow++;
         }
         spin_unlock(&r->lock);
      }
      uring_store_release(&r->sq_head, head + 1);
      done++;
   }

   if (min_complete) {
      unsigned start = ticks;
      for (;;) {
         spin_lock(&r->lock);
         if ((uring_load_acquire(&r->cq_tail) -
              uring_load_acquire(&r->cq_head)) >= min_complete) {
            spin_unlock(&r->lock);
            break;
         }
         reinit_completion(&r->cq_event);
         spin_unlock(&r->lock);
         virtio_blk_harvest();
         if ((uring_load_acquire(&r->cq_tail) -
              uring_load_acquire(&r->cq_head)) >= min_complete)
            break;
         if (ticks - start > URING_WAIT_TICKS)
            break;
         if (!completion_done(&r->cq_event) &&
             !wait_for_completion_until(&r->cq_event,next_wait_deadline()))
            continue;
      }
   }
   (void)flags;
   uring_put(r);
   return (int)done;
}

static int spawn_load(const char *path, const char *params)
{
   char *buf;
   DWORD size;
   DWORD id;
   char temp[255];
   char name[256];
   char cmd[SPAWN_CMDLINE];
   int i;

   if (!path)
      return -EINVAL;
   for (i = 0; path[i] && i < 255; i++)
      name[i] = path[i];
   name[i] = 0;
   if (!name[0])
      return -EINVAL;

   cmd[0] = 0;
   if (params) {
      for (i = 0; params[i] && i < SPAWN_CMDLINE - 1; i++)
         cmd[i] = params[i];
      cmd[i] = 0;
   }
   if (!cmd[0]) {
      for (i = 0; name[i] && i < SPAWN_CMDLINE - 1; i++)
         cmd[i] = name[i];
      cmd[i] = 0;
   }

   /* ELF64 executables are streamed page-by-page from VFS into the child's
       private frames.  This avoids allocating an 18 MiB cc1.exe image in the
       kernel heap and removes the need to hold a spin crit while the loader
       copies the image.  Non-ELF images still use the old eager map+load path,
       serialized by elf_map_crit to bound kernel-heap pressure. */
    id = (DWORD)elf64_stream_load(name, 0, cmd,
                                  showpath(temp), current_process);
    if (id && (int)id != -1)
       return (int)id;

    sync_entercrit(&elf_map_crit);
    buf = (char *)vfs_mapfile(name, &size);
    if (!buf) {
       sync_leavecrit(&elf_map_crit);
       return -ENOENT;
    }

    id = dex32_loader(name, buf, userspace, 0, cmd, showpath(temp),
                      current_process);
    free(buf);
    sync_leavecrit(&elf_map_crit);
    if (!id || (int)id == -1)
       return -ENOEXEC;
    return (int)id;
}

long sys_spawn(const char *path, const char *params)
{
   return spawn_load(path, params);
}

static PCB386 *waitpid_live_child(PCB386 *parent)
{
   PCB386 *head, *p;
   if (!parent)
      return (PCB386 *)-1;
   head = sched_gethead();
   if (!head)
      return (PCB386 *)-1;
   p = head;
      do {
         if (p->owner == parent->processid && p != parent
          && !(p->status & PS_ATTB_THREAD)
          && !(p->status & PS_ATTB_UNLOADABLE))
         return p;
         p = p->next;
      } while (p && p->before && p != head);
   return (PCB386 *)-1;
}

long sys_waitpid(int pid, int *status, int options)
{
    PCB386 *me = current_process;
    int wpid = 0, wst = 0;

    if (!me)
       return -EINVAL;
    if (pid < -1 || pid == 0)
       return -EINVAL;

    for (;;) {
       int alive;

       if (waitq_reap(me, pid, &wpid, &wst)) {
          if (status)
             *status = wst;
          return wpid;
       }

       /* Is it still worth waiting?  This runs without waitq_lock:
          ps_findprocess() waits on processmgr_busy, which an exiting sibling
          can hold across closeallfiles() and a TLB-shootdown IPI, so it must
          never be called with interrupts masked. */
       if (pid > 0) {
          PCB386 *p = ps_findprocess((DWORD)pid);
          alive = (p != (PCB386 *)-1 && p->owner == me->processid
                   && !(p->status & PS_ATTB_THREAD));
       } else {
          alive = me->nlive > 0;
       }

       if (!alive) {
          /* The child may have exited between the scan above and this check.
             Both exit paths append the status before dequeueing the PCB, so a
             child that is already gone has a queued status; re-scan before
             reporting ECHILD.  Skipping this re-scan is what made gccdriver
             print "waitpid /work/apps/as.exe failed" under make -j4. */
          __sync_synchronize();
          if (waitq_reap(me, pid, &wpid, &wst)) {
             if (status)
                *status = wst;
             return wpid;
          }
          return -ECHILD;
       }

       if (options & WNOHANG) {
          taskswitch();
          return 0;
       }
       sched_block_process(me, ticks + 20);
       taskswitch();
       sched_wake_process(me);
    }
}

long sys_getdents(const char *path, char *ubuf, int ubuflen)
{
   vfs_node *dir;
   vfs_node *list;
   int n, i, off;

   if (!path || !ubuf || ubuflen < 2)
      return -EINVAL;
   if (path[0] == '.' && path[1] == 0 && current_process)
      dir = current_process->workdir;
   else
      dir = vfs_searchname(path);
   if (!dir)
      return -ENOENT;
   n = vfs_listdir(dir, 0, 0);
   if (n < 0)
      return -EIO;
   if (n == 0) {
      ubuf[0] = 0;
      return 1;
   }
   list = (vfs_node *)malloc((unsigned)n * sizeof(vfs_node));
   if (!list)
      return -ENOMEM;
   n = vfs_listdir(dir, list, n * (int)sizeof(vfs_node));
   off = 0;
   for (i = 0; i < n; i++) {
      const char *nm = list[i].name;
      int l;
      if (!nm)
         continue;
      l = (int)strlen(nm);
      if (off + l + 2 > ubuflen)
         break;
      memcpy(ubuf + off, nm, (unsigned)l + 1);
      off += l + 1;
   }
   ubuf[off++] = 0;
   free(list);
   return off;
}

long sys_execve(const char *path, const char *params)
{
   PCB386 *me = current_process;
   PCB386 *child;
   int id, oldpid;
   DWORD flags;

   /* Keep IRQs off until the child owns our pid so waitpid(old)
      cannot miss the steal window. createprocess restores the flags
      it sampled here (already off). */
   dex32_stopints(&flags);
   id = spawn_load(path, params);
   if (id < 0) {
      dex32_restoreints(flags);
      return id;
   }
   child = ps_findprocess((DWORD)id);
   if (child == (PCB386 *)-1) {
      dex32_restoreints(flags);
      return -EIO;
   }
   oldpid = (int)me->processid;
   child->processid = (DWORD)oldpid;
   child->owner = me->owner;
   me->processid = (DWORD)id;
   ps_dequeue(me);
   dex32_restoreints(flags);
   ps_switchto(child);
   for (;;)
      ;
   return 0;
}

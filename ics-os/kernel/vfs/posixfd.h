#ifndef ICSOS_POSIXFD_H
#define ICSOS_POSIXFD_H

#include "../types.h"

#define ICSOS_O_RDONLY   0x0000
#define ICSOS_O_WRONLY   0x0001
#define ICSOS_O_RDWR     0x0002
#define ICSOS_O_ACCMODE  0x0003
#define ICSOS_O_CREAT    0x0100
#define ICSOS_O_TRUNC    0x0800
#define ICSOS_O_APPEND   0x1000
#define ICSOS_O_DIRECT   0x4000

#define ICSOS_AT_FDCWD   (-100)

#define IORING_OP_NOP     0
#define IORING_OP_READV   1
#define IORING_OP_WRITEV  2
#define IORING_OP_FSYNC   3
#define IORING_OP_OPENAT  18
#define IORING_OP_CLOSE   19
#define IORING_OP_READ    22
#define IORING_OP_WRITE   23

#define IORING_ENTER_GETEVENTS 1

struct k_iovec {
   void *iov_base;
   unsigned long iov_len;
};

struct _PCB386;
int posix_fd_clone(struct _PCB386 *dst, struct _PCB386 *src);
int posix_fd_fork_ready(struct _PCB386 *src);
int posix_fd_clone_fork(struct _PCB386 *dst, struct _PCB386 *src);
void posix_fd_close_all(struct _PCB386 *process);

/* POSIX syscall handlers return a 64-bit (long) value so negative errno
 * codes are sign-extended into rax per the SysV AMD64 ABI. The int 0x30
 * dispatcher (dex32API.c api_syscall) invokes these through an api_arg_t
 * (64-bit) function pointer; an int-returning handler would zero-extend
 * its 32-bit result, turning e.g. -ENOENT into a huge positive value. */
long sys_open(const char *path, int flags, int mode);
long sys_close(int fd);
long sys_dup(int oldfd);
long sys_dup2(int oldfd, int newfd);
long sys_read(int fd, void *buf, long n);
long sys_write(int fd, const void *buf, long n);
long sys_lseek(int fd, long off, int whence);
long sys_preadv(int fd, const struct k_iovec *iov, int iovcnt, long offset);
long sys_pwritev(int fd, const struct k_iovec *iov, int iovcnt, long offset);
long  sys_fsync(int fd);
long  sys_fstat_fd(int fd, void *statbuf);
long  sys_io_uring_setup(unsigned entries, void *params);
long  sys_io_uring_enter(int fd, unsigned to_submit, unsigned min_complete,
                         unsigned flags);
void *sys_fd_file(int fd);

struct _PCB386;
struct _tty;
struct _tty *posix_fd_tty(int fd);
int posix_fd_selectable(int fd, int for_write);

long sys_waitpid(int pid, int *status, int options);
long sys_spawn(const char *path, const char *params);
long sys_execve(const char *path, const char *params);
long sys_getdents(const char *path, char *buf, int buflen);

#endif

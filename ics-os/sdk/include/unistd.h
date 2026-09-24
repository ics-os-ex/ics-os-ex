#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>
#include <sys/types.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1

int close(int fd);
ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
long lseek(int fd, long off, int whence);
ssize_t pread(int fd, void *buf, size_t n, off_t offset);
ssize_t pwrite(int fd, const void *buf, size_t n, off_t offset);
int fsync(int fd);
int unlink(const char *path);
int rmdir(const char *path);
int mkdir(const char *path, mode_t mode);
char *getcwd(char *buf, size_t size);
int chdir(const char *path);
int isatty(int fd);
int access(const char *path, int mode);
int chown(const char *path, uid_t uid, gid_t gid);
int chmod(const char *path, mode_t mode);
void *sbrk(int inc);
unsigned int sleep(unsigned int seconds);
typedef unsigned int useconds_t;
int usleep(useconds_t usec);
void _exit(int status);
int getpid(void);
int kill(int pid, int sig);
pid_t vfork(void);
char *getlogin(void);
int gethostname(char *name, size_t len);
int getloadavg(double loadavg[], int nelem);
int fork(void);
int execv(const char *path, char *const argv[]);
int execvp(const char *file, char *const argv[]);
int dup(int oldfd);
int dup2(int oldfd, int newfd);
int pipe(int fd[2]);
int umask(int mask);
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
int mkstemp(char *template);
int ftruncate(int fd, off_t length);

/* Path / memory queries (GNU binutils: libbfd getpagesize, ld realpath,
   libiberty pathconf/_PC_PATH_MAX). */
char *realpath(const char *path, char *resolved);
long sysconf(int name);
int getpagesize(void);
long pathconf(const char *path, int name);

#define _SC_PAGESIZE    30
#define _SC_PAGE_SIZE   30
#define _SC_NPROCESSORS_CONF 83
#define _SC_NPROCESSORS_ONLN 84
#define _SC_PHYS_PAGES 80
#define _SC_AVPHYS_PAGES 81
#define _SC_CLK_TCK   2

#define _PC_NAME_MAX  0
#define _PC_PATH_MAX  1
#define _PC_LINK_MAX  2
#define _PC_MAX_CANON 3
#define _PC_MAX_INPUT 4

extern char **environ;

#endif

#ifndef ICSOS_VI_LIBBB_H
#define ICSOS_VI_LIBBB_H

#include "platform.h"
#include "../../../../sdk/dexsdk.h"

#define BB_VER "ICS-OS vi (BusyBox)"

#ifndef NULL
#define NULL 0
#endif

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef unsigned int uintptr_t;
typedef int intptr_t;
typedef int int32_t;

typedef signed char smallint;
typedef unsigned char smalluint;

typedef struct llist_t {
	struct llist_t *next;
	char *data;
} llist_t;

#define CONFIG_FEATURE_VI_MAX_LEN 4096

#define ENABLE_FEATURE_VI_COLON 1
#define ENABLE_FEATURE_VI_SEARCH 1
#define ENABLE_FEATURE_VI_YANKMARK 1
#define ENABLE_FEATURE_VI_VERBOSE_STATUS 0
#define ENABLE_FEATURE_VI_REGEX_SEARCH 0
#define ENABLE_FEATURE_VI_8BIT 0
#define ENABLE_FEATURE_VI_DOT_CMD 0
#define ENABLE_FEATURE_VI_SETOPTS 0
#define ENABLE_FEATURE_VI_READONLY 0
#define ENABLE_FEATURE_VI_ASK_TERMINAL 0
#define ENABLE_FEATURE_VI_COLON_EXPAND 0
#define ENABLE_FEATURE_VI_CRASHME 0
#define ENABLE_FEATURE_VI_UNDO 0
#define ENABLE_FEATURE_VI_UNDO_QUEUE 0
#define ENABLE_FEATURE_VI_USE_SIGNALS 0
#define ENABLE_FEATURE_VI_WIN_RESIZE 0

#if ENABLE_FEATURE_VI_COLON
#define IF_FEATURE_VI_COLON(...) __VA_ARGS__
#else
#define IF_FEATURE_VI_COLON(...)
#endif

#if ENABLE_FEATURE_VI_SEARCH
#define IF_FEATURE_VI_SEARCH(...) __VA_ARGS__
#else
#define IF_FEATURE_VI_SEARCH(...)
#endif

#if ENABLE_FEATURE_VI_YANKMARK
#define IF_FEATURE_VI_YANKMARK(...) __VA_ARGS__
#else
#define IF_FEATURE_VI_YANKMARK(...)
#endif

#if ENABLE_FEATURE_VI_VERBOSE_STATUS
#define IF_FEATURE_VI_VERBOSE_STATUS(...) __VA_ARGS__
#else
#define IF_FEATURE_VI_VERBOSE_STATUS(...)
#endif

#if ENABLE_FEATURE_VI_COLON_EXPAND
#define IF_FEATURE_VI_COLON_EXPAND(...) __VA_ARGS__
#else
#define IF_FEATURE_VI_COLON_EXPAND(...)
#endif

#if ENABLE_FEATURE_VI_READONLY
#define IF_FEATURE_VI_READONLY(...) __VA_ARGS__
#else
#define IF_FEATURE_VI_READONLY(...)
#endif

#if ENABLE_FEATURE_VI_CRASHME
#define IF_FEATURE_VI_CRASHME(...) __VA_ARGS__
#else
#define IF_FEATURE_VI_CRASHME(...)
#endif

#if ENABLE_FEATURE_VI_UNDO
#define IF_FEATURE_VI_UNDO(...) __VA_ARGS__
#else
#define IF_FEATURE_VI_UNDO(...)
#endif

#if ENABLE_FEATURE_VI_UNDO_QUEUE
#define IF_FEATURE_VI_UNDO_QUEUE(...) __VA_ARGS__
#else
#define IF_FEATURE_VI_UNDO_QUEUE(...)
#endif

#if ENABLE_FEATURE_VI_SETOPTS
#define IF_FEATURE_VI_SETOPTS(...) __VA_ARGS__
#else
#define IF_FEATURE_VI_SETOPTS(...)
#endif

#if ENABLE_FEATURE_VI_DOT_CMD
#define IF_FEATURE_VI_DOT_CMD(...) __VA_ARGS__
#else
#define IF_FEATURE_VI_DOT_CMD(...)
#endif

#if ENABLE_FEATURE_VI_ASK_TERMINAL
#define IF_FEATURE_VI_ASK_TERMINAL(...) __VA_ARGS__
#else
#define IF_FEATURE_VI_ASK_TERMINAL(...)
#endif

#if ENABLE_FEATURE_VI_WIN_RESIZE
#define IF_FEATURE_VI_WIN_RESIZE(...) __VA_ARGS__
#else
#define IF_FEATURE_VI_WIN_RESIZE(...)
#endif

#if ENABLE_FEATURE_VI_8BIT
#define IF_FEATURE_VI_8BIT(...) __VA_ARGS__
#else
#define IF_FEATURE_VI_8BIT(...)
#endif

#if ENABLE_FEATURE_VI_REGEX_SEARCH
#define IF_FEATURE_VI_REGEX_SEARCH(...) __VA_ARGS__
#else
#define IF_FEATURE_VI_REGEX_SEARCH(...)
#endif

#if ENABLE_FEATURE_VI_USE_SIGNALS
#define IF_FEATURE_VI_USE_SIGNALS(...) __VA_ARGS__
#else
#define IF_FEATURE_VI_USE_SIGNALS(...)
#endif

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define KEYCODE_BUFFER_SIZE 16

#define KEYCODE_UP       0x101
#define KEYCODE_DOWN     0x102
#define KEYCODE_LEFT     0x103
#define KEYCODE_RIGHT    0x104
#define KEYCODE_HOME     0x105
#define KEYCODE_END      0x106
#define KEYCODE_PAGEUP   0x107
#define KEYCODE_PAGEDOWN 0x108
#define KEYCODE_DELETE   0x109
#define KEYCODE_INSERT   0x10a
#define KEYCODE_FUN1     0x111
#define KEYCODE_FUN2     0x112
#define KEYCODE_FUN3     0x113
#define KEYCODE_FUN4     0x114
#define KEYCODE_FUN5     0x115
#define KEYCODE_FUN6     0x116
#define KEYCODE_FUN7     0x117
#define KEYCODE_FUN8     0x118
#define KEYCODE_FUN9     0x119
#define KEYCODE_FUN10    0x11a
#define KEYCODE_FUN11    0x11b
#define KEYCODE_FUN12    0x11c
#define KEYCODE_CURSOR_POS 0x120

#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR   0x0002
#define O_CREAT  0x0040
#define O_TRUNC  0x0200
#define O_APPEND 0x0400

#define FILE_DIRECTORY 0x200

#define S_IFDIR FILE_DIRECTORY
#define S_IWGRP 0020
#define S_IWOTH 0002

#define S_ISDIR(m) ((m) & FILE_DIRECTORY)
#define S_ISREG(m) (!((m) & FILE_DIRECTORY))

#define EAGAIN 11

#define STRERROR_FMT "%s"
#define STRERROR_ERRNO , strerror(errno)

struct stat {
	int size;
	int st_dev;
	int st_ino;
	int st_mode;
	short st_nlink;
	short st_uid;
	short st_gid;
	int st_rdev;
	int st_size;
	int st_atime;
	int st_mtime;
	int st_ctime;
};

#define NCCS 1
#define VERASE 0

struct termios {
	unsigned char c_cc[NCCS];
};

#define TERMIOS_RAW_CRNL 0x0001

struct pollfd {
	int fd;
	short events;
	short revents;
};

#define POLLIN 0x0001

extern int optind;
extern int errno;

struct globals;
extern struct globals *ptr_to_globals;
#define SET_PTR_TO_GLOBALS(x) (ptr_to_globals = (x))

void bb_simple_error_msg_and_die(const char *msg);
void bb_show_usage(void);
void bb_putchar(int c);
unsigned long bb_strtou(const char *arg, char **endp, int base);
void *xmalloc(size_t size);
void *xzalloc(size_t size);
void *xrealloc(void *ptr, size_t size);
char *xstrdup(const char *s);
char *xstrndup(const char *s, int n);
char *xasprintf(const char *fmt, ...);
char *xmalloc_open_read_close(const char *filename, size_t *sizep);
char *concat_path_file(const char *path, const char *filename);
char *llist_pop(llist_t **head);
void fputs_stdout(const char *s);
void fflush_all(void);
int safe_poll(struct pollfd *pfd, int nfds, int timeout);
ssize_t safe_read(int fd, void *buf, size_t count);
int safe_read_key(int fd, char *buf, int timeout);
ssize_t full_read(int fd, void *buf, size_t count);
ssize_t full_write(int fd, const void *buf, size_t count);
int getopt32(char **argv, const char *optstring, ...);
int set_termios_to_raw(int fd, struct termios *old, int flags);
int tcsetattr_stdin_TCSANOW(const struct termios *tp);

int snprintf(char *buffer, int size, const char *fmt, ...);
int vsnprintf(char *buffer, int size, const char *fmt, va_list args);
int isblank(int c);
int ispunct(int c);

int open(const char *path, int flags, ...);
int close(int fd);
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int ftruncate(int fd, unsigned int length);
int stat(const char *path, struct stat *st);
int getuid(void);
int system(const char *cmd);

void *memrchr(const void *s, int c, size_t n);
char *strchrnul(const char *s, int c);

#endif

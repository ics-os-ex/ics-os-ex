/* auto/config.h for the ICS-OS freestanding vim build (FEAT_TINY).
 *
 * This file is hand-maintained for the ICS-OS user-space SDK (it is NOT
 * produced by vim's ./configure, which assumes a hosted glibc environment).
 * Only capabilities that the ICS-OS SDK (sdk/include + sdk/posix.c +
 * sdk/tccsdk.c) actually provide are defined here. Anything the SDK does not
 * implement is left undefined so the matching #ifdef code is compiled out.
 *
 * Keep this in lockstep with the SDK: when a capability is added to the SDK,
 * define it here (and vice versa).
 */

/* ICS-OS is little-endian x86-64, not EBCDIC. */
/* #undef EBCDIC */

/* No X11 / GUI / Wayland on ICS-OS. */
/* #undef HAVE_X11 */
/* #undef HAVE_WAYLAND */

/* No terminfo / termcap; the kernel TTY drives VT100/xterm output directly. */
/* #undef TERMINFO */
/* #undef HAVE_OSPEED */
/* #undef HAVE_UP_BC_PC */
/* #undef HAVE_OUTFUNTYPE */
/* #undef HAVE_DEL_CURTERM */
/* #undef HAVE_TGETENT */
/* #undef TGETENT_ZERO_ERR */
/* #undef HAVE_TERMCAP_H */
/* #undef HAVE_TERMIO_H */

/* GCC provides __DATE__/__TIME__ and the unused attribute. */
#define HAVE_DATE_TIME 1
#define HAVE_ATTRIBUTE_UNUSED 1

/* Unix-like (POSIX) target. */
#define UNIX 1

/* Pointer/integer widths for x86-64 long mode. */
#define VIM_SIZEOF_INT 4
#define VIM_SIZEOF_LONG 8
#define SIZEOF_OFF_T 8
#define SIZEOF_TIME_T 8

/* wchar_t is 32 bits on this target. */
/* #undef SMALL_WCHAR_T */

/* The SDK memmove() is correct for overlapping regions. */
#define USEMEMMOVE 1
#define USEMAN_S 1

/* select() lives in <sys/select.h> and needs <sys/time.h> included first. */
#define SYS_SELECT_WITH_SYS_TIME 1
#define SELECT_TYPE_ARG234 (fd_set *)

/* No sub-second mtime in the SDK struct stat; leave ST_MTIM_NSEC undefined so
 * the b_mtime_ns bookkeeping in fileio.c/memline.c/evalfunc.c is compiled out. */
/* #undef ST_MTIM_NSEC */
/* The SDK struct stat has no st_blksize. */
/* #undef HAVE_ST_BLKSIZE */

/* ---- Functions the ICS-OS SDK provides (see sdk/posix.c, sdk/tccsdk.c) ---- */
#define HAVE_FSYNC 1
#define HAVE_FTRUNCATE 1
#define HAVE_GETCWD 1
#define HAVE_GETRLIMIT 1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_LSTAT 1
#define HAVE_MEMSET 1
#define HAVE_OPENDIR 1
#define HAVE_PUTENV 1
#define HAVE_QSORT 1
#define HAVE_RENAME 1
#define HAVE_SELECT 1
#define HAVE_SETENV 1
#define HAVE_SIGPROCMASK 1
#define HAVE_STRCOLL 1
#define HAVE_STRERROR 1
#define HAVE_STRFTIME 1
#define HAVE_STRPBRK 1
#define HAVE_STRTOL 1
#define HAVE_SYSCONF 1
#define HAVE_UTIME 1
#define HAVE_USLEEP 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_FLOCK 1

/* Not provided by the SDK (left undefined so callers fall back or compile out):
 * fchdir fchmod fchown lchown readlink realpath-into? realpath IS provided.
 * mkdtemp nanosleep posix_openpt setsid setpgid getpgid sigaction sigaltstack
 * sigset sigstack strcasecmp strncasecmp strptime tzset gmtime localtime_r
 * unsetenv mblen timer_create xattr sync dirfd sysinfo getpw* iconv.
 */

/* ---- Header files the ICS-OS SDK provides (sdk/include) ---- */
#define HAVE_DIRENT_H 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_MATH_H 1
#define HAVE_POLL_H 1
#define HAVE_SETJMP_H 1
#define HAVE_STDINT_H 1
#define HAVE_STRINGS_H 1
#define HAVE_SYS_IOCTL_H 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_SYS_POLL_H 1
#define HAVE_SYS_RESOURCE_H 1
#define HAVE_SYS_SELECT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_TERMIOS_H 1
#define HAVE_UNISTD_H 1
#define HAVE_SYS_WAIT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_WCHAR_H 1

/* No <locale.h>, <nl_types.h>, <langinfo.h>, <iconv.h>, <libgen.h>,
 * <libintl.h>, <wctype.h>, <sys/utsname.h>, <sys/statfs.h>, <sys/sysinfo.h>.
 */
/* #undef HAVE_LOCALE_H */
/* #undef HAVE_LANGINFO_H */
/* #undef HAVE_ICONV_H */
/* #undef HAVE_LIBGEN_H */
/* #undef HAVE_LIBINTL_H */
/* #undef HAVE_WCTYPE_H */
/* #undef HAVE_NL_LANGINFO_CODESET */

/* No dynamic loading (dlopen/dlsym) on ICS-OS. */
/* #undef HAVE_DLFCN_H */
/* #undef HAVE_DLOPEN */
/* #undef HAVE_DLSYM */

/* Feature level: smallest editor (no +eval, no +cmdline scripting). */
#define FEAT_TINY 1
/* #undef FEAT_NORMAL */
/* #undef FEAT_HUGE */
/* #undef FEAT_LUA */
/* #undef FEAT_PERL */
/* #undef FEAT_PYTHON */
/* #undef FEAT_PYTHON3 */
/* #undef FEAT_RUBY */
/* #undef FEAT_TCL */
/* #undef ENABLE_CSCOPE */
/* #undef WANT_SOCKETSERVER */
/* #undef FEAT_TERMINAL */
/* #undef FEAT_JOB_CHANNEL */

/* No /proc on ICS-OS; no XSMP. */
/* #undef PROC_EXE_LINK */
/* #undef USE_XSMP_INTERACT */
/* #undef HAVE_FD_CLOEXEC */
/* #undef HAVE_SYSCONF_SIGSTKSZ */

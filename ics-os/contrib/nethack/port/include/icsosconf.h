#ifndef ICSOSCONF_H
#define ICSOSCONF_H

/* ICS-OS port configuration */
#define ICSOS


/* Use tty window port with ANSI escape sequences */
#define TTY_GRAPHICS
#define ANSI_DEFAULT
#define TEXTCOLOR

/* Use POSIX-style type/prototype assumptions where needed */
#define POSIX_TYPES

/* No signals or shell escapes on ICS-OS */
#define NO_SIGNAL
#define NOSHELL

/* Port identity */
#ifndef PORT_ID
#define PORT_ID "ICS-OS"
#endif

/* Use DLB data library packaging */
#define DLB

/* File name length and permissions */
#ifndef FILENAME
#define FILENAME 256
#endif

#ifndef PATHLEN
#define PATHLEN 256
#endif

extern char hackdir[];

#ifndef FCMASK
#define FCMASK 0660
#endif

#ifndef FILENAME_CMP
#define FILENAME_CMP strcmp
#endif

/* String helpers typically mapped on UNIX */
#ifndef index
#define index strchr
#endif

#ifndef rindex
#define rindex strrchr
#endif

/* Input helper */
#ifndef tgetch
#define tgetch getchar
#endif

/* Data location inside ICS-OS */
#ifndef HACKDIR
#define HACKDIR "/nethack"
#endif

#endif

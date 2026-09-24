#ifndef _TERMIOS_H
#define _TERMIOS_H

/*
 * termios for ICS-OS.
 *
 * NOTE: struct termios layout must stay byte-identical to the kernel copy in
 * kernel/console/tty_tc.c (4 x unsigned int + unsigned char c_line +
 * unsigned char c_cc[32] = 49 bytes, padded to 52). The c_lflag bits ICANON
 * (0x02), ECHO (0x08) and ISIG (0x01) are read directly by the kernel line
 * discipline, so keep their values in sync.
 */

#include <sys/types.h>

#define NCCS 32

struct termios {
    unsigned int c_iflag;
    unsigned int c_oflag;
    unsigned int c_cflag;
    unsigned int c_lflag;
    unsigned char c_line;
    unsigned char c_cc[NCCS];
};

/* c_iflag */
#define IGNBRK    0x00000001
#define BRKINT    0x00000002
#define IGNPAR    0x00000004
#define PARMRK    0x00000008
#define INPCK     0x00000010
#define ISTRIP    0x00000020
#define INLCR     0x00000040
#define IGNCR     0x00000080
#define ICRNL     0x00000100
#define IUCLC     0x00000200
#define IXON      0x00000400
#define IXANY     0x00000800
#define IXOFF     0x00001000
#define IMAXBEL   0x00002000

/* c_oflag */
#define OPOST     0x00000001
#define OLCUC     0x00000002
#define ONLCR     0x00000010
#define OCRNL     0x00000020
#define ONOCR     0x00000040
#define ONLRET    0x00000080

/* c_cflag */
#define CSIZE     0x00000030
#define CS5       0x00000020
#define CS6       0x00000030
#define CS7       0x00000020
#define CS8       0x00000030
#define CSTOPB    0x00000040
#define CREAD     0x00000080
#define PARENB    0x00000100
#define CLOCAL    0x00001000

/* c_lflag (ICANON/ECHO/ISIG map 1:1 to the kernel TTY flags) */
#define ISIG      0x00000001
#define ICANON    0x00000002
#define XCASE     0x00000004
#define ECHO      0x00000008
#define ECHOE     0x00000010
#define ECHOK     0x00000020
#define ECHONL    0x00000040
#define NOFLSH    0x00000080
#define TOSTOP    0x00000100
#define IEXTEN    0x00008000

/* c_cc indices */
#define VINTR     0
#define VQUIT     1
#define VERASE    2
#define VKILL     3
#define VEOF      4
#define VTIME     5
#define VMIN      6
#define VSWTCH    7
#define VSTART    8
#define VSTOP     9
#define VSUSP     10
#define VEOL      11
#define VEOL2     12

/* tcsetattr actions */
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

/* tcflush queues */
#define TCIFLUSH  0
#define TCOFLUSH  1
#define TCIOFLUSH 2

int tcgetattr(int fd, struct termios *termios_p);
int tcsetattr(int fd, int optional_actions, const struct termios *termios_p);
int tcflush(int fd, int queue_selector);

/* POSIX speed constants (c_cflag baud field).  ICS-OS has no real serial
   line, so tcgetattr reports 0 and tcsetattr ignores c_cflag; these exist
   purely so termios-based applications (e.g. NetHack's speednum()) compile
   and the cfgetospeed()/cfsetospeed() accessors have a type to work on.
   Values follow the classic System V ordering used by NetHack's speednum(). */
typedef unsigned int speed_t;

#define B0       0
#define B50      1
#define B75      2
#define B110     3
#define B134     4
#define B150     5
#define B200     6
#define B300     7
#define B600     8
#define B1200    9
#define B1800    10
#define B2400    11
#define B4800    12
#define B9600    13
#define B19200   14
#define B38400   15

/* System V baud-rate field mask in c_cflag (classic 00377); cfgetospeed()/
   cfsetospeed() in posix.c mask c_cflag with this. */
#define CBAUD    0x000000FF

/* POSIX VDISABLE value: "no such character exists". */
#define _POSIX_VDISABLE 0

speed_t cfgetospeed(const struct termios *termios_p);
speed_t cfgetispeed(const struct termios *termios_p);
int     cfsetospeed(struct termios *termios_p, speed_t speed);
int     cfsetispeed(struct termios *termios_p, speed_t speed);

#endif

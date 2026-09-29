#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/poll.h>

int main(void)
{
    struct termios raw, canon;
    struct pollfd pfd;
    char c;
    int n;

    write(1, "keytest: POLLTEST READY\n", 24);
    if (tcgetattr(0, &canon)) { printf("keytest: FAIL tcgetattr errno=%d\n", errno); return 1; }
    raw = canon;
    raw.c_iflag &= ~(unsigned int)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
    raw.c_oflag &= ~(unsigned int)OPOST;
    raw.c_lflag &= ~(unsigned int)(ICANON | ECHO | ECHOE | ECHOK | ECHONL | ISIG);
    raw.c_cflag &= ~(unsigned int)(CSIZE | PARENB);
    raw.c_cflag |= (unsigned int)CS8;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &raw)) { printf("keytest: FAIL raw errno=%d\n", errno); return 1; }
    write(1, "\x1b[2J\x1b[H\x1b[?25l\x1b[?1049h", 22);
    write(1, "keytest: ALTTEST READY\n", 23);
    for (;;) {
        pfd.fd = 0;
        pfd.events = POLLIN;
        pfd.revents = 0;
        n = poll(&pfd, 1, 50);
        if (n < 0) { printf("keytest: FAIL poll errno=%d\n", errno); return 1; }
        if (n > 0)
            break;
    }
    if (read(0, &c, 1) != 1) { printf("keytest: FAIL read errno=%d\n", errno); return 1; }
    {
       char gb[24];
       int gl = sprintf(gb, "keytest: got 0x%02x\n", (unsigned int)(unsigned char)c);
       write(1, gb, gl);
    }
    write(1, "\x1b[?1049l\x1b[?25h", 16);
    if (tcsetattr(0, TCSANOW, &canon)) { printf("keytest: FAIL restore errno=%d\n", errno); return 1; }
    write(1, "keytest: PASS\n", 14);
    return 0;
}

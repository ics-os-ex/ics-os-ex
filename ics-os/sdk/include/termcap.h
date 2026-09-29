#ifndef ICSOS_TERMCAP_H
#define ICSOS_TERMCAP_H

/* Minimum size of the buffer passed to tgetent().  The ICS-OS built-in and
 * /icsos/etc/termcap entries are kept below this limit. */
#define TCBUFSIZ 2048

/*
 * Standard termcap interface.  tgetent() takes the caller's entry buffer
 * first and the terminal name second, matching POSIX/glibc and the call
 * order used by Vim and NetHack.
 */
int tgetent(char *buf, const char *id);
int tgetflag(const char *id);
int tgetnum(const char *id);
char *tgetstr(const char *id, char **buf);
void tputs(const char *str, int affcnt, int (*outc)(int));
char *tgoto(const char *cm, int col, int line);
char *tparam(const char *ctl, char *buf, int buflen,
             int row, int col, int row2, int col2);

#endif

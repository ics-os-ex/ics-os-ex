#ifndef ICSOS_STDIO_H
#define ICSOS_STDIO_H

#include "dexsdk.h"

#ifndef BUFSIZ
#define BUFSIZ 512
#endif

int sscanf(const char *str, const char *fmt, ...);
int fscanf(FILE *stream, const char *fmt, ...);
int rename(const char *oldpath, const char *newpath);
void rewind(FILE *stream);
void perror(const char *s);

#endif

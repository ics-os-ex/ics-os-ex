#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>
#include <stdarg.h>
#include <time.h>

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define BUFSIZ 1024
#define L_tmpnam 64
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

typedef unsigned int FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

int printf(const char *fmt, ...);
int fprintf(FILE *f, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int vsprintf(char *buf, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int sscanf(const char *str, const char *fmt, ...);

FILE *fopen(const char *path, const char *mode);
FILE *fdopen(int fd, const char *mode);
int fclose(FILE *f);
int fflush(FILE *f);
int feof(FILE *f);
int fgetc(FILE *f);
char *fgets(char *s, int n, FILE *f);
int fputc(int c, FILE *f);
int fputs(const char *s, FILE *f);
int fread(void *ptr, size_t size, size_t nmemb, FILE *f);
int fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);
int fseek(FILE *f, long off, int whence);
long ftell(FILE *f);
void rewind(FILE *f);
int ungetc(int c, FILE *f);
int putchar(int c);
int puts(const char *s);
int getchar(void);
char *gets(char *buf);
int fscanf(FILE *f, const char *fmt, ...);
int remove(const char *path);
int rename(const char *oldpath, const char *newpath);
void perror(const char *s);
int setvbuf(FILE *f, char *buf, int mode, size_t size);
int fileno(FILE *f);
int putc(int c, FILE *f);
int ferror(FILE *f);
int getc(FILE *f);
int clearerr(FILE *f);
#define getc(f) fgetc(f)

#endif

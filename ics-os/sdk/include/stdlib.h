#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 32767

void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
void *sbrk(int amt);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int val, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);
void *memchr(const void *s, int c, size_t n);

void exit(int status);
int atexit(void (*fn)(void));
void abort(void);
double atof(const char *s);
int putenv(char *string);
void *icsos_alloca(unsigned long n);
int atoi(const char *s);
long atol(const char *s);
long strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
long long strtoll(const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);
double strtod(const char *nptr, char **endptr);
float strtof(const char *nptr, char **endptr);
long double strtold(const char *nptr, char **endptr);

char *getenv(const char *name);
int setenv(const char *name, const char *value, int replace);
void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));
int abs(int n);
long labs(long n);
int rand(void);
void srand(unsigned int seed);
/* glibc-compatible 48-bit linear congruential generator. NetHack (SYSV
 * config) drives its game RNG through lrand48()/srand48() and relies on the
 * standard period (~2^48), so the implementation in posix.c must match the
 * glibc/ISO recurrence exactly. */
long lrand48(void);
void srand48(long seed);
int system(const char *cmd);
char *mktemp(char *template);

#endif

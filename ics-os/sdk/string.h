#ifndef ICSOS_STRING_H
#define ICSOS_STRING_H

#include "dexsdk.h"

char *strcpy(char *to, const char *from);
char *strncpy(char *dst, const char *src, size_t n);
char *strcat(char *s, const char *append);
char *strncat(char *dst, const char *src, size_t n);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
size_t strlen(const char *str);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *s, const char *find);
char *strtok(char *s, const char *delim);
size_t strspn(const char *s1, const char *s2);
size_t strcspn(const char *s1, const char *s2);
void *memcpy(void *dst, const void *src, unsigned int count);
void *memmove(void *dst, const void *src, unsigned int count);
void *memset(void *dst, int val, unsigned int count);
int memcmp(const void *s1, const void *s2, size_t n);

#endif

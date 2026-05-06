#ifndef ICSOS_STDLIB_H
#define ICSOS_STDLIB_H

#include "dexsdk.h"

int abs(int x);
long atol(const char *str);
void abort(void);
void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));

#endif

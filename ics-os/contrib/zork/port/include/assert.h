#ifndef ICSOS_ASSERT_H
#define ICSOS_ASSERT_H

#include "../../../../sdk/dexsdk.h"

#define assert(x) do { \
    if (!(x)) { \
        printf("assertion failed: %s\n", #x); \
        exit(1); \
    } \
} while (0)

#endif

#ifndef ICSOS_STDIO_H
#define ICSOS_STDIO_H

#include "../../../../sdk/dexsdk.h"

#ifndef FILENAME_MAX
#define FILENAME_MAX 255
#endif

#ifndef _IOFBF
#define _IOFBF 0
#endif
#ifndef _IOLBF
#define _IOLBF 1
#endif
#ifndef _IONBF
#define _IONBF 2
#endif

int setvbuf(FILE *stream, char *buf, int mode, size_t size);

#endif

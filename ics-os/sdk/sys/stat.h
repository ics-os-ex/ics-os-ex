#ifndef ICSOS_SYS_STAT_H
#define ICSOS_SYS_STAT_H

#include "../dexsdk.h"

#define S_IFDIR 0x200
#define S_IFREG 0x000
#define S_IWGRP 0020
#define S_IWOTH 0002

#define S_ISDIR(m) ((m) & S_IFDIR)
#define S_ISREG(m) (!((m) & S_IFDIR))

#endif

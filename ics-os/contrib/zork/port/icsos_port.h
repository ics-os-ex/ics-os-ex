#ifndef ICSOS_ZORK_PORT_H
#define ICSOS_ZORK_PORT_H

#include "../../../sdk/dexsdk.h"

int zork_init(int argc, char **argv);
int zork_getch(void);
int zork_putchar(int c);
int zork_getline(char *buf, int maxlen);
void zork_sleep_ms(int ms);

FILE *zork_fopen(const char *path, const char *mode);
int zork_fclose(FILE *f);
int zork_fseek(FILE *f, long off, int whence);
long zork_ftell(FILE *f);
size_t zork_fread(void *buf, size_t size, size_t n, FILE *f);
size_t zork_fwrite(const void *buf, size_t size, size_t n, FILE *f);
int zork_fflush(FILE *f);

void *zork_malloc(size_t size);
void zork_free(void *ptr);

#endif

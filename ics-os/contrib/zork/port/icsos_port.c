#include "icsos_port.h"

int zork_init(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return 0;
}

int zork_getch(void)
{
    return (int)getch();
}

int zork_putchar(int c)
{
    outc((char)c);
    return c;
}

int zork_getline(char *buf, int maxlen)
{
    if (!buf || maxlen <= 0) return 0;
    if (fgets(buf, maxlen, stdin) == 0)
        return 0;
    return (int)strlen(buf);
}

void zork_sleep_ms(int ms)
{
    if (ms <= 0) return;
    // DEX sleep is in ticks/seconds; use sleep(ms/1000) for coarse delay
    if (ms < 1000)
        sleep(1);
    else
        sleep(ms/1000);
}

FILE *zork_fopen(const char *path, const char *mode)
{
    return fopen(path, mode);
}

int zork_fclose(FILE *f)
{
    return fclose(f);
}

int zork_fseek(FILE *f, long off, int whence)
{
    return (int)dexsdk_systemcall(FXN_FSEEK, (int)f, off, whence, 0, 0);
}

long zork_ftell(FILE *f)
{
    return ftell(f);
}

size_t zork_fread(void *buf, size_t size, size_t n, FILE *f)
{
    return (size_t)fread(buf, (int)size, (int)n, f);
}

size_t zork_fwrite(const void *buf, size_t size, size_t n, FILE *f)
{
    return (size_t)fwrite((void*)buf, (int)size, (int)n, f);
}

int zork_fflush(FILE *f)
{
    return fflush(f);
}

void *zork_malloc(size_t size)
{
    return malloc(size);
}

void zork_free(void *ptr)
{
    free(ptr);
}

int setvbuf(FILE *stream, char *buf, int mode, size_t size)
{
    (void)stream;
    (void)buf;
    (void)mode;
    (void)size;
    return 0;
}

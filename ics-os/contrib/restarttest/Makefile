include ../../sdk/app.mk

CC = gcc
CFLAGS = $(APP_CFLAGS) -nostdinc -I$(SDK)/include
LDFLAGS = $(APP_LDFLAGS)
LIBS = $(SDK)/tccsdk.c $(SDK)/posix.c $(SDK)/libtcc1.c $(SDK)/crt1.c

all: restarttest.exe

restarttest.exe: restarttest.c $(LIBS)
	$(CC) $(CFLAGS) -o $@ restarttest.c $(LIBS) $(LDFLAGS)

install: all
	cp restarttest.exe ../../apps/

clean:
	rm -f restarttest.exe

.PHONY: all install clean

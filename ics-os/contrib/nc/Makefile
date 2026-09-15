include ../../sdk/app.mk

NAME=nc

SRC=$(NAME).c
EXE=$(NAME).exe

CC=gcc
ICSOS_ROOT=../..
SDK=../../sdk
CFLAGS=$(APP_CFLAGS) -nostdinc -I$(SDK)/include
LIBS=$(SDK)/tccsdk.c $(SDK)/posix.c $(SDK)/libtcc1.c $(SDK)/crt1.c

$(EXE): $(SRC) $(SDK)/tccsdk.c $(SDK)/posix.c
	$(CC) $(CFLAGS) $(APP_LDFLAGS) -o${EXE} $(SRC) $(LIBS)

install: $(EXE)
	mkdir -p $(ICSOS_ROOT)/apps
	cp $(EXE) $(ICSOS_ROOT)/apps

uninstall:
	rm -f $(ICSOS_ROOT)/apps/$(EXE)

clean:
	rm -f $(EXE)

include ../../sdk/app.mk

NAME=memcorrupt
SRC=memcorrupt.c
EXE=memcorrupt.exe
CC=gcc
ICSOS_ROOT=../..
SDK=../../sdk
CFLAGS=$(APP_CFLAGS) -nostdinc -I$(SDK)/include
LIBS=$(SDK)/tccsdk.c $(SDK)/posix.c $(SDK)/libtcc1.c $(SDK)/crt1.c

# memcorrupt: in-OS user-memory corruption detector (see memcorrupt.c).
$(EXE): $(SRC) $(SDK)/posix.c $(SDK)/tccsdk.c
	$(CC) $(CFLAGS) $(APP_LDFLAGS) -o${EXE} $(SRC) $(LIBS)

install: $(EXE)
	mkdir -p $(ICSOS_ROOT)/apps
	cp $(EXE) $(ICSOS_ROOT)/apps

uninstall:
	rm -f $(ICSOS_ROOT)/apps/$(EXE)

clean:
	rm -f $(EXE)

NAME=vi
EXE=$(NAME).exe

BUSYBOX=./busybox
PORT=./port
ICSOS_ROOT=../..
SDK=../../sdk

SRC=$(BUSYBOX)/editors/vi.c \
	$(PORT)/libbb_compat.c

CC=gcc
CFLAGS=-m32 -w -nostdlib -fno-builtin -static \
	-I$(PORT)/include -I$(BUSYBOX)/include -I$(BUSYBOX)
LIBS=$(SDK)/tccsdk.c $(SDK)/libtcc1.c $(SDK)/crt1.c

$(EXE): $(SRC)
	$(CC) $(CFLAGS) -o $(EXE) $(SRC) $(LIBS)

all: $(EXE)

install: $(EXE)
	cp $(EXE) $(ICSOS_ROOT)/apps

uninstall:
	rm -f $(ICSOS_ROOT)/apps/$(EXE)

clean:
	rm -f $(EXE)

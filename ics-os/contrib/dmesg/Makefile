CC=gcc
ICSOS_ROOT=../..
SDK=../../sdk
CFLAGS=-m32 -w -nostdlib -fno-builtin -static
LIBS=$(SDK)/tccsdk.c $(SDK)/libtcc1.c $(SDK)/crt1.c
EXE=dmesg.exe
SRC=dmesg.c

$(EXE): $(SRC)
	$(CC) $(CFLAGS) -o$(EXE) $(SRC) $(LIBS)

install: $(EXE)
	cp $(EXE) $(ICSOS_ROOT)/apps

uninstall:
	rm -f $(ICSOS_ROOT)/apps/$(EXE)

clean:
	rm -f $(EXE)

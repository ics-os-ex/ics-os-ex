include ../../sdk/app.mk

NAME=apuser
SRC=$(NAME).c
EXE=$(NAME).exe
CC=gcc
ICSOS_ROOT=../..
SDK=../../sdk
# Seed cc1 is not built with -mno-red-zone. Keep the same here so AP
# timer IRQs on the user stack match the cert's compiler processes.
CFLAGS=$(filter-out -mno-red-zone,$(APP_CFLAGS)) -msse2 -nostdinc -I$(SDK)/include
LIBS=$(SDK)/tccsdk.c $(SDK)/posix.c $(SDK)/libtcc1.c $(SDK)/crt1.c

$(EXE): $(SRC) $(SDK)/posix.c $(SDK)/tccsdk.c
	$(CC) $(CFLAGS) $(APP_LDFLAGS) -o$(EXE) $(SRC) $(LIBS)

install: $(EXE)
	mkdir -p $(ICSOS_ROOT)/apps
	cp $(EXE) $(ICSOS_ROOT)/apps

uninstall:
	rm -f $(ICSOS_ROOT)/apps/$(EXE)

clean:
	rm -f $(EXE)

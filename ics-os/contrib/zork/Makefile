# Zork/Z-Machine port (uses ICS-OS SDK stdlib)

CC=gcc
ICSOS_ROOT=../..
SDK=../../sdk
PORT=./port
PORT_INCLUDE=$(PORT)/include
INTERPRETER=./interpreter
COMMON_DIR=$(INTERPRETER)/src/common
DUMB_DIR=$(INTERPRETER)/src/dumb

VERSION=2.55
RELEASE_NOTES=ICS-OS port
COMMON_DEFINES=$(COMMON_DIR)/defs.h
HASH=$(COMMON_DIR)/hash.h

CFLAGS=-m32 -w -nostdlib -nostdinc -fno-builtin -static \
	-I$(PORT_INCLUDE) -I$(COMMON_DIR) -I$(DUMB_DIR) \
	-D__UNIX_PORT_FILE -DNO_BASENAME -DNO_STRDUP -DNO_STRNDUP -DNO_SOUND -DNO_BLORB
LIBS=$(SDK)/tccsdk.c $(SDK)/libtcc1.c $(SDK)/crt1.c

EXE=zork.exe
PORT_SRC=$(PORT)/icsos_port.c
COMMON_SRC=$(COMMON_DIR)/buffer.c $(COMMON_DIR)/err.c $(COMMON_DIR)/fastmem.c \
	$(COMMON_DIR)/files.c $(COMMON_DIR)/getopt.c $(COMMON_DIR)/hotkey.c \
	$(COMMON_DIR)/input.c $(COMMON_DIR)/main.c $(COMMON_DIR)/math.c \
	$(COMMON_DIR)/missing.c $(COMMON_DIR)/object.c $(COMMON_DIR)/process.c \
	$(COMMON_DIR)/quetzal.c $(COMMON_DIR)/random.c $(COMMON_DIR)/redirect.c \
	$(COMMON_DIR)/screen.c $(COMMON_DIR)/sound.c $(COMMON_DIR)/stream.c \
	$(COMMON_DIR)/table.c $(COMMON_DIR)/text.c $(COMMON_DIR)/variable.c

DUMB_SRC=$(DUMB_DIR)/dblorb.c $(DUMB_DIR)/dinit.c $(DUMB_DIR)/dinput.c \
	$(DUMB_DIR)/doutput.c $(DUMB_DIR)/dpic.c

SRC=$(COMMON_SRC) $(DUMB_SRC) $(PORT_SRC)

all: $(COMMON_DEFINES) $(HASH) $(EXE)

$(EXE): $(SRC)
	@if [ ! -d $(COMMON_DIR) ] || [ ! -d $(DUMB_DIR) ]; then \
		echo "Missing interpreter sources. Put a GPL-compatible Z-Machine interpreter in contrib/zork/interpreter."; \
		exit 1; \
	fi
	$(CC) $(CFLAGS) -o$(EXE) $(SRC) $(LIBS)
	@mkdir -p $(ICSOS_ROOT)/apps
	@cp $(EXE) $(ICSOS_ROOT)/apps

$(COMMON_DEFINES):
	@echo "** Generating $(COMMON_DEFINES)"
	@echo "#ifndef COMMON_DEFINES_H" > $@
	@echo "#define COMMON_DEFINES_H" >> $@
	@echo "#define RELEASE_NOTES \"$(RELEASE_NOTES)\"" >> $@
	@echo "#define MAX_UNDO_SLOTS 500" >> $@
	@echo "#define MAX_FILE_NAME 80" >> $@
	@echo "#define TEXT_BUFFER_SIZE 512" >> $@
	@echo "#define INPUT_BUFFER_SIZE 200" >> $@
	@echo "#define STACK_SIZE 1024" >> $@
	@echo "#define NO_BASENAME" >> $@
	@echo "#define NO_STRDUP" >> $@
	@echo "#define NO_STRNDUP" >> $@
	@echo "#define NO_SOUND" >> $@
	@echo "#define NO_BLORB" >> $@
	@echo "#endif /* COMMON_DEFINES_H */" >> $@

$(HASH):
	@echo "** Generating $(HASH)"
	@echo "#ifndef VERSION" > $@
	@echo "#define VERSION \"$(VERSION)\"" >> $@
	@echo "#endif" >> $@
	@echo "#ifndef RELEASE_NOTES" >> $@
	@echo "#define RELEASE_NOTES \"$(RELEASE_NOTES)\"" >> $@
	@echo "#endif" >> $@
	@echo "#define GIT_HASH \"ics-os\"" >> $@
	@echo "#define GIT_HASH_SHORT \"ics-os\"" >> $@
	@echo "#define GIT_DATE \"unknown\"" >> $@

install: $(EXE)
	cp $(EXE) $(ICSOS_ROOT)/apps

uninstall:
	rm -f $(ICSOS_ROOT)/apps/$(EXE)

clean:
	rm -f $(EXE)

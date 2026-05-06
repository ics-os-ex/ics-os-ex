NAME=nethack
EXE=$(NAME).exe

ROOT_DIR=$(CURDIR)
NETHACK_ROOT=$(ROOT_DIR)/nethack/NetHack-3.6.7
PORT_DIR=$(ROOT_DIR)/port
SDK=$(ROOT_DIR)/../../sdk
UTIL_DIR=$(NETHACK_ROOT)/util
DAT_DIR=$(NETHACK_ROOT)/dat

HOSTCC=gcc
HOSTCFLAGS=-O2 -Wall -I$(NETHACK_ROOT)/include
HOST_YACC=bison -d
HOST_LEX=flex

CC=gcc
CFLAGS=-m32 -w -nostdlib -fno-builtin -static \
	-DICSOS -I$(PORT_DIR)/include -I$(SDK) \
	-I$(NETHACK_ROOT)/include -I$(NETHACK_ROOT)/src
LIBS=$(SDK)/tccsdk.c $(SDK)/libtcc1.c $(SDK)/crt1.c

HOST_TOOLS=$(UTIL_DIR)/makedefs
HOST_GEN_TOOLS=$(UTIL_DIR)/lev_comp $(UTIL_DIR)/dgn_comp $(UTIL_DIR)/dlb
HOST_SRCS=$(NETHACK_ROOT)/util/makedefs.c \
	$(NETHACK_ROOT)/src/objects.c \
	$(NETHACK_ROOT)/src/monst.c

SRC_FILES=$(wildcard $(NETHACK_ROOT)/src/*.c) \
	$(PORT_DIR)/icsos_sys.c \
	$(NETHACK_ROOT)/win/tty/getline.c \
	$(NETHACK_ROOT)/win/tty/termcap.c \
	$(NETHACK_ROOT)/win/tty/topl.c \
	$(NETHACK_ROOT)/win/tty/wintty.c \
	$(NETHACK_ROOT)/sys/share/pcmain.c \
	$(NETHACK_ROOT)/sys/share/pcsys.c \
	$(NETHACK_ROOT)/sys/share/pcunix.c \
	$(NETHACK_ROOT)/sys/share/pctty.c \
	$(NETHACK_ROOT)/sys/share/random.c \
	$(NETHACK_ROOT)/sys/share/tclib.c \
	$(NETHACK_ROOT)/sys/share/pmatchregex.c

all: prepare
	@echo "Prepared NetHack headers/data. Next: make build"

prepare: $(HOST_TOOLS)
	@echo "Generating NetHack headers/data..."
	cd $(NETHACK_ROOT)/util && ./makedefs -odemvpqrshz

$(HOST_TOOLS): $(HOST_SRCS)
	$(HOSTCC) $(HOSTCFLAGS) -o $@ $(HOST_SRCS)

$(UTIL_DIR)/lev_yacc.c $(NETHACK_ROOT)/include/lev_comp.h: $(UTIL_DIR)/lev_comp.y
	cd $(UTIL_DIR) && $(HOST_YACC) -o lev_yacc.c --defines=../include/lev_comp.h lev_comp.y

$(UTIL_DIR)/lev_lex.c: $(UTIL_DIR)/lev_comp.l $(NETHACK_ROOT)/include/lev_comp.h
	cd $(UTIL_DIR) && $(HOST_LEX) -o lev_lex.c lev_comp.l

$(UTIL_DIR)/dgn_yacc.c $(NETHACK_ROOT)/include/dgn_comp.h: $(UTIL_DIR)/dgn_comp.y
	cd $(UTIL_DIR) && $(HOST_YACC) -o dgn_yacc.c --defines=../include/dgn_comp.h dgn_comp.y

$(UTIL_DIR)/dgn_lex.c: $(UTIL_DIR)/dgn_comp.l $(NETHACK_ROOT)/include/dgn_comp.h
	cd $(UTIL_DIR) && $(HOST_LEX) -o dgn_lex.c dgn_comp.l

$(UTIL_DIR)/lev_comp: $(UTIL_DIR)/lev_main.c $(UTIL_DIR)/lev_yacc.c $(UTIL_DIR)/lev_lex.c \
		$(NETHACK_ROOT)/src/alloc.c $(NETHACK_ROOT)/src/monst.c $(NETHACK_ROOT)/src/objects.c \
		$(NETHACK_ROOT)/src/decl.c $(NETHACK_ROOT)/src/drawing.c \
		$(NETHACK_ROOT)/sys/msdos/video.c $(UTIL_DIR)/panic.c
	$(HOSTCC) $(HOSTCFLAGS) -I$(NETHACK_ROOT)/src -I$(NETHACK_ROOT)/sys/msdos -DSTUBVIDEO -o $@ $^

$(UTIL_DIR)/dgn_comp: $(UTIL_DIR)/dgn_main.c $(UTIL_DIR)/dgn_yacc.c $(UTIL_DIR)/dgn_lex.c \
		$(NETHACK_ROOT)/src/alloc.c $(UTIL_DIR)/panic.c
	$(HOSTCC) $(HOSTCFLAGS) -I$(NETHACK_ROOT)/src -o $@ $^

$(UTIL_DIR)/dlb: $(UTIL_DIR)/dlb_main.c $(NETHACK_ROOT)/src/dlb.c $(NETHACK_ROOT)/src/alloc.c \
		$(UTIL_DIR)/panic.c
	$(HOSTCC) $(HOSTCFLAGS) -I$(NETHACK_ROOT)/src -DDLB -DDLBLIB -o $@ $^

data: $(HOST_GEN_TOOLS)
	@echo "Generating NetHack levels and nhdat..."
	cd $(DAT_DIR) && ../util/lev_comp bigroom.des
	cd $(DAT_DIR) && ../util/lev_comp castle.des
	cd $(DAT_DIR) && ../util/lev_comp endgame.des
	cd $(DAT_DIR) && ../util/lev_comp gehennom.des
	cd $(DAT_DIR) && ../util/lev_comp knox.des
	cd $(DAT_DIR) && ../util/lev_comp mines.des
	cd $(DAT_DIR) && ../util/lev_comp medusa.des
	cd $(DAT_DIR) && ../util/lev_comp oracle.des
	cd $(DAT_DIR) && ../util/lev_comp sokoban.des
	cd $(DAT_DIR) && ../util/lev_comp tower.des
	cd $(DAT_DIR) && ../util/lev_comp yendor.des
	cd $(DAT_DIR) && ../util/lev_comp Arch.des
	cd $(DAT_DIR) && ../util/lev_comp Barb.des
	cd $(DAT_DIR) && ../util/lev_comp Caveman.des
	cd $(DAT_DIR) && ../util/lev_comp Healer.des
	cd $(DAT_DIR) && ../util/lev_comp Knight.des
	cd $(DAT_DIR) && ../util/lev_comp Monk.des
	cd $(DAT_DIR) && ../util/lev_comp Priest.des
	cd $(DAT_DIR) && ../util/lev_comp Ranger.des
	cd $(DAT_DIR) && ../util/lev_comp Rogue.des
	cd $(DAT_DIR) && ../util/lev_comp Samurai.des
	cd $(DAT_DIR) && ../util/lev_comp Tourist.des
	cd $(DAT_DIR) && ../util/lev_comp Valkyrie.des
	cd $(DAT_DIR) && ../util/lev_comp Wizard.des
	cd $(DAT_DIR) && ../util/dgn_comp dungeon.pdf
	cd $(DAT_DIR) && rm -f dlb.lst
	cd $(DAT_DIR) && ls -1 data dungeon oracles options quest.dat rumors help hh cmdhelp history opthelp wizhelp license keyhelp symbols bogusmon engrave epitaph tribute > dlb.lst
	cd $(DAT_DIR) && ls -1 *.lev >> dlb.lst
	cd $(DAT_DIR) && ../util/dlb cvIf dlb.lst nhdat

build: $(EXE)

$(EXE): $(SRC_FILES)
	$(CC) $(CFLAGS) -o $(EXE) $(SRC_FILES) $(LIBS)

install: $(EXE) data
	cp $(EXE) $(ROOT_DIR)/../../apps
	@mkdir -p $(ROOT_DIR)/../../nethack
	cp $(DAT_DIR)/nhdat $(ROOT_DIR)/../../nethack
	@touch $(ROOT_DIR)/../../nethack/record

clean:
	rm -f $(EXE) $(HOST_TOOLS)

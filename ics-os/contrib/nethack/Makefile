# Build NetHack 3.6 (TTY graphics, freestanding ELF64) for ICS-OS.
#
# The NetHack source tree is vendored under ./src (uncommitted), following the
# same convention as the other contrib apps (vim, gcc, binutils). The recipe
# compiles the game core, the Unix/POSIX system layer, the termcap library
# (sys/share/tclib.c) and the TTY windowing system, then links everything
# against the ICS-OS user-space SDK.
#
# Terminal support: the kernel already exposes an 80x25 VT100/xterm console
# (kernel/console/tty_vt.c) with real tcgetattr/tcsetattr/TIOCGWINSZ syscalls
# and sets TERM=xterm at boot. A matching termcap entry is shipped at
# base/etc/termcap; NetHack's built-in termcap reader (tclib.c) loads it.
#
# The game's data files (dat/*) and the player's saves live in HACKDIR
# (config.h: /nethack); `install` copies the dat tree there.
include ../../sdk/app.mk

NAME=nethack
EXE=nethack.exe
.DEFAULT_GOAL := $(EXE)
CC=gcc
ICSOS_ROOT=../..
SDK=../../sdk
SRC=src
STAGE := /tmp/icsos-nethack
OBJDIR := $(STAGE)/obj
HACKDIR := /icsos/nethack

# NetHack 3.6 uses C99 constructs, so bump the language level above the SDK's
# gnu89 default (same treatment as the vim port).
NH_CFLAGS=$(subst -std=gnu89,-std=gnu99,$(APP_CFLAGS))
# -nostdinc: only the SDK headers are visible (freestanding). NetHack's own
# headers are resolved from the vendored src/include tree.
CFLAGS=$(NH_CFLAGS) -nostdinc -I$(SDK)/include -I$(SRC)/include
# The SDK support libraries must be compiled WITHOUT NetHack's include dir:
# NetHack's headers (e.g. its own sys/types.h, regex.h) would shadow the SDK's
# and break the SDK sources.  The SDK sources only ever include <...> headers,
# so restricting them to the SDK include tree is exactly what they need.
SDK_CFLAGS=$(NH_CFLAGS) -nostdinc -I$(SDK)/include
SDK_SRC=$(SDK)/tccsdk.c $(SDK)/posix.c $(SDK)/libtcc1.c $(SDK)/crt1.c
SDK_OBJ=$(patsubst %.c,$(OBJDIR)/sdk-%.o,$(notdir $(SDK_SRC)))
LIBS=$(SDK_OBJ)

# --- NetHack game core (src/src/*.c) ---------------------------------------
HACK_C = $(patsubst %,$(SRC)/src/%,\
  allmain.c alloc.c apply.c artifact.c attrib.c ball.c bones.c botl.c cmd.c \
  dbridge.c decl.c detect.c dig.c display.c dlb.c do.c do_name.c do_wear.c \
  dog.c dogmove.c dokick.c dothrow.c drawing.c dungeon.c eat.c end.c \
  engrave.c exper.c explode.c extralev.c files.c fountain.c hack.c hacklib.c \
  invent.c isaac64.c light.c lock.c mail.c makemon.c mapglyph.c mcastu.c \
  mhitm.c mhitu.c minion.c mklev.c mkmap.c mkmaze.c mkobj.c mkroom.c mon.c \
  mondata.c monmove.c monst.c mplayer.c mthrowu.c muse.c music.c o_init.c \
  objects.c objnam.c options.c pager.c pickup.c pline.c polyself.c potion.c \
  pray.c priest.c quest.c questpgr.c read.c rect.c region.c restore.c rip.c \
  rnd.c role.c rumors.c save.c shk.c shknam.c sit.c sounds.c sp_lev.c \
  spell.c steal.c steed.c sys.c teleport.c timeout.c topten.c track.c \
  trap.c u_init.c uhitm.c vault.c version.c vision.c weapon.c were.c \
  wield.c windows.c wizard.c worm.c worn.c write.c zap.c vis_tab.c)

# --- OS / system layer (sys/*) ---------------------------------------------
SYS_C = \
  $(SRC)/sys/share/ioctl.c $(SRC)/sys/share/unixtty.c $(SRC)/sys/unix/unixmain.c \
  $(SRC)/sys/unix/unixunix.c $(SRC)/sys/unix/unixres.c

# --- regex + termcap (sys/share) -------------------------------------------
# tclib.c implements the tgetent/tgetstr/tgetnum/tgetflag/tgoto termcap API
# that NetHack's TTY windowing uses (TERMLIB is defined via include/tcap.h).
EXTRA_C = $(SRC)/sys/share/posixregex.c $(SRC)/sys/share/tclib.c

# --- TTY windowing (win/tty) -----------------------------------------------
WIN_C = $(SRC)/win/tty/getline.c $(SRC)/win/tty/termcap.c $(SRC)/win/tty/topl.c $(SRC)/win/tty/wintty.c

ALL_C = $(HACK_C) $(SYS_C) $(EXTRA_C) $(WIN_C)
# Flatten to object names in a single dir (NetHack has no two sources sharing
# a basename across these sets).
OBJS = $(patsubst %.c,$(OBJDIR)/%.o,$(notdir $(ALL_C)))

# The generated data tables (pm.h, onames.h, date.h, vis_tab.h/.c) must exist
# before any object is compiled.
$(OBJDIR)/.stage:
	rm -rf $(OBJDIR)
	mkdir -p $(OBJDIR)
	@test -f $(SRC)/include/pm.h && test -f $(SRC)/include/onames.h \
	  && test -f $(SRC)/include/date.h && test -f $(SRC)/src/vis_tab.c \
	  || { echo "ERROR: NetHack data tables missing; run util/makedefs first"; exit 1; }
	@touch $(OBJDIR)/.stage

# Object rule: $(OBJDIR)/<basename>.o is built from the source in ALL_C whose
# path ends in /<basename>.c (or is exactly <basename>.c for the ./src core).
$(OBJDIR)/%.o: $(OBJDIR)/.stage
	@_src=""; \
	for _f in $(ALL_C); do \
	  case "$$_f" in */$*.c|$*.c) _src="$$_f"; break;; esac; \
	done; \
	if [ -z "$$_src" ]; then echo "no source for $*.c"; exit 1; fi; \
	$(CC) $(CFLAGS) -c "$$_src" -o $@

# SDK support libraries (no NetHack includes) ------------------------------
$(OBJDIR)/sdk-%.o: $(SDK)/%.c $(OBJDIR)/.stage
	$(CC) $(SDK_CFLAGS) -c $< -o $@

$(EXE): $(OBJS) $(SDK_OBJ)
	$(CC) $(CFLAGS) $(APP_LDFLAGS) -o $@ $(OBJS) $(LIBS)

# The nhdat DLB resource (NetHack's single packed data file) is prebuilt on the
# host (makedefs + dgn_comp + make_dlb, see dlb.lst) and shipped at
# src/dat/nhdat.  NetHack opens "nhdat" as a bare relative path against the
# process cwd (CHDIR/NOCWD_ASSUMPTIONS/VAR_PLAYGROUND are undefined), which the
# kernel sets to "/", so the test image ships it at the ISO root as /nhdat.
# Regenerate it on the host if it is missing (e.g. after a clean checkout):
#   cd src/dat && makedefs -voDmeqrsphz && dgn_comp dungeon.pdf && dlb cI dlb.lst
install: $(EXE) $(SRC)/dat/nhdat
	mkdir -p $(ICSOS_ROOT)/apps
	cp $(EXE) $(ICSOS_ROOT)/apps
	# Game data (symbols, *.des, help, options, ...) into /nethack.
	mkdir -p $(ICSOS_ROOT)/base/nethack
	cp -a $(SRC)/dat/. $(ICSOS_ROOT)/base/nethack/
	# The packed DLB resource NetHack actually reads at runtime.
	cp $(SRC)/dat/nhdat $(ICSOS_ROOT)/base/nethack/nhdat
	# SYSCF_FILE (sysconf) is required when SYSCF is defined (config.h).
	cp $(SRC)/sys/unix/sysconf $(ICSOS_ROOT)/base/nethack/sysconf
	# Ensure the termcap is present.
	mkdir -p $(ICSOS_ROOT)/base/etc
	@test -f $(ICSOS_ROOT)/base/etc/termcap || \
	  echo "ERROR: base/etc/termcap missing"

uninstall:
	rm -f $(ICSOS_ROOT)/apps/$(EXE)
	rm -rf $(ICSOS_ROOT)/base/nethack

clean:
	rm -f $(EXE)
	rm -rf $(STAGE)

.PHONY: install uninstall clean

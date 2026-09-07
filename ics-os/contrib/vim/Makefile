# Build vim (FEAT_TINY, freestanding ELF64) for ICS-OS.
#
# vim's source is vendored under the (uncommitted) repo-level /references tree,
# following the same convention as the other contrib toolchains (gcc, binutils,
# make). Override VIM_SRC to point elsewhere. The recipe stages the source into
# a temp dir, substitutes the ICS-OS auto/config.h, generates protodef.h from
# the checked-in proto/*.pro files, compiles the core objects with the SDK
# freestanding flags, and links them against the ICS-OS user-space SDK.
include ../../sdk/app.mk

NAME=vim
EXE=vim.exe
.DEFAULT_GOAL := $(EXE)
CC=gcc
ICSOS_ROOT=../..
SDK=../../sdk
VIM_SRC ?= ../../../references/vim-9.2.1031/src
STAGE := /tmp/icsos-vim
OBJDIR := $(STAGE)/obj

# -nostdinc: only the SDK headers are visible (freestanding). The staged tree
# provides vim's own headers, proto/, and auto/.
# vim 9.2 uses C99 constructs (for-loop initializers), so bump the language
# level above the SDK's default gnu89.
VIM_CFLAGS=$(subst -std=gnu89,-std=gnu99,$(APP_CFLAGS))
CFLAGS=$(VIM_CFLAGS) -nostdinc -I$(SDK)/include -I$(STAGE) -I$(STAGE)/proto -DHAVE_CONFIG_H
LIBS=$(SDK)/tccsdk.c $(SDK)/posix.c $(SDK)/libtcc1.c $(SDK)/crt1.c

# Core editor sources (the OBJ_COMMON list from vim's Makefile). Optional
# subsystems (GUI, hardcopy, spell, sixel/kitty/cairo, sockets, terminal) are
# feature-gated to empty objects under FEAT_TINY.
VIM_C = \
  alloc.c arabic.c arglist.c autocmd.c beval.c buffer.c change.c blob.c \
  blowfish.c cindent.c clientserver.c clipboard.c cmdexpand.c cmdhist.c \
  crypt.c crypt_zip.c debugger.c dict.c diff.c digraph.c drawline.c \
  drawscreen.c edit.c eval.c evalbuffer.c evalfunc.c evalvars.c \
  evalwindow.c ex_cmds.c ex_cmds2.c ex_docmd.c ex_eval.c ex_getln.c fileio.c \
  filepath.c findfile.c float.c fold.c fuzzy.c getchar.c gc.c gui_xim.c \
  hardcopy.c hardcopy_pango.c hardcopy_postscript.c hashtab.c help.c \
  highlight.c if_cscope.c if_xcmdsrv.c indent.c insexpand.c linematch.c \
  list.c locale.c logfile.c map.c mark.c match.c mbyte.c memline.c menu.c \
  misc1.c misc2.c mouse.c move.c normal.c ops.c option.c optionstr.c \
  os_unix.c pathdef.c popupmenu.c popupwin.c profiler.c pty.c quickfix.c \
  regexp.c register.c screen.c scriptfile.c search.c session.c sha256.c \
  sign.c sixel.c kitty.c cairo.c socketserver.c sound.c spell.c spellfile.c \
  spellsuggest.c strings.c syntax.c tabpanel.c tag.c term.c terminal.c \
  testing.c textformat.c textobject.c textprop.c time.c tuple.c typval.c ui.c \
  undo.c usercmd.c userfunc.c version.c vim9class.c vim9cmds.c \
  vim9compile.c vim9execute.c vim9expr.c vim9generics.c vim9instr.c \
  vim9script.c vim9type.c viminfo.c window.c bufwrite.c charset.c json.c \
  main.c memfile.c message.c icsos_stub.c

VIM_OBJS = $(patsubst %.c,$(OBJDIR)/%.o,$(VIM_C))

# Stage the source tree once (re-run when the config or sources change).
$(STAGE)/.stage: config.icsos.h icsos_stub.c $(VIM_SRC)/vim.h
	rm -rf $(STAGE)
	mkdir -p $(OBJDIR) $(STAGE)/auto
	cp -a $(VIM_SRC)/*.c $(VIM_SRC)/*.h $(STAGE)/
	cp -a $(VIM_SRC)/proto $(STAGE)/proto
	cp -a $(VIM_SRC)/xdiff $(STAGE)/xdiff
	-cp -a $(VIM_SRC)/auto/osdef.h $(STAGE)/auto/osdef.h
	cp config.icsos.h $(STAGE)/auto/config.h
	cp pathdef.c $(STAGE)/pathdef.c
	cp icsos_stub.c $(STAGE)/icsos_stub.c
	# protodef.h stays the checked-in stub; vim.h includes the real prototypes
	# via proto.h (which pulls in proto/*.pro, resolved through -Iproto).
	@touch $(STAGE)/.stage

$(OBJDIR)/%.o: $(STAGE)/.stage
	$(CC) $(CFLAGS) -c $(STAGE)/$*.c -o $@

$(EXE): $(VIM_OBJS)
	$(CC) $(CFLAGS) $(APP_LDFLAGS) -o $@ $(VIM_OBJS) $(LIBS)

install: $(EXE)
	mkdir -p $(ICSOS_ROOT)/apps
	cp $(EXE) $(ICSOS_ROOT)/apps

uninstall:
	rm -f $(ICSOS_ROOT)/apps/$(EXE)

clean:
	rm -f $(EXE)
	rm -rf $(STAGE)

.PHONY: install uninstall clean

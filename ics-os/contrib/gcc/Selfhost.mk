# Shared host/in-ICS-OS recipe for rebuilding the GCC 4.7.4 C frontend.
# The staging script normalizes each cc1 source to units/<object-stem>.c so
# GNU Make can use a deterministic pattern rule without shell source lookup.
# The recipe is parallel-safe: every object is compiled to its own output and
# the output directories are created once by the `prepare` target before any
# concurrent job runs.

ROOT ?= /icsos/gccsrc
OUT ?= /work
ifeq ($(origin CC),default)
CC := /icsos/apps/gcc.exe
endif
ifeq ($(origin AS),default)
AS := /icsos/apps/as.exe
endif
ifeq ($(origin AR),default)
AR := /icsos/apps/ar.exe
endif
ifeq ($(origin LD),default)
LD := /icsos/apps/ld.exe
endif
CP ?= /icsos/apps/cp.exe
MKDIR ?= /icsos/apps/mkdir.exe
RM ?= /icsos/apps/rm.exe
SEED ?= /icsos/seed
LDSCRIPT ?= /icsos/apps/ldscripts/elf_x86_64.xc
TOOLPREFIX ?=

OBJ := $(OUT)/gccobj
BASE_FLAGS := $(TOOLPREFIX) -m64 -std=gnu89 -w -nostdinc -nostdlib \
	-fno-builtin -static -ffreestanding -fno-pie -fno-pic \
	-fno-stack-protector -fno-asynchronous-unwind-tables \
	-fno-strict-aliasing -mcmodel=large -mno-red-zone
COMMON_INC := -DHAVE_CONFIG_H -I$(ROOT)/sdk/include -I$(ROOT)/conf/gcc \
	-I$(ROOT)/up/gcc -I$(ROOT)/up/libcpp -I$(ROOT)/up/libcpp/include \
	-I$(ROOT)/up/libiberty -I$(ROOT)/conf/gmp -I$(ROOT)/conf/mpfr \
	-I$(ROOT)/up/mpfr -I$(ROOT)/up/include
DEC_INC := -DHAVE_CONFIG_H -I$(ROOT)/up/libdecnumber \
	-I$(ROOT)/up/libdecnumber/bid -I$(ROOT)/up/libgcc \
	-I$(ROOT)/conf/decnumber -I$(ROOT)/sdk/include
Z_INC := -I$(ROOT)/up/zlib -I$(ROOT)/sdk/include
VERDEFS := -DBASEVER=\"4.7.4\" -DBUGURL=\"\" -DDATESTAMP=\"20260830\" \
	-DDEVPHASE=\"\" -DREVISION=\"\" -DPKGVERSION=\"4.7.4\" \
	-DTARGET_NAME=\"x86_64-ics-os\"
GASDEFS := -DHAVE_GAS_CFI_PERSONALITY_DIRECTIVE=1 \
	-DHAVE_GAS_CFI_DIRECTIVE=1 -DHAVE_COMDAT_GROUP=1 \
	-DHAVE_GAS_SHF_MERGE=1 -DHAVE_GAS_CFI_SECTIONS_DIRECTIVE=1 \
	-DHAVE_GAS_HIDDEN=1 -DHAVE_GAS_MAX_SKIP_P2ALIGN=65535 \
	-DHAVE_AS_GOTOFF_IN_DATA=1 -DHAVE_AS_IX86_FFREEP=1 \
	-DHAVE_AS_IX86_FILDQ=1 -DHAVE_AS_IX86_FILDS=1 \
	-DHAVE_AS_IX86_REP_LOCK_PREFIX=1 -DHAVE_AS_TLS=1 \
	-DHAVE_AS_GOTTPLTPCALL=1 -DHAVE_AS_TLSDIRECT=1 \
	-DHAVE_AS_CFI_SECTIONS=1 -DHAVE_AS_X86_CMPXCHG16B=1
GCC_INC := -I$(ROOT)/gen -I$(ROOT)/shims -I$(ROOT)/conf/gcc \
	-I$(ROOT)/sdk/include -I$(ROOT)/up/gcc -I$(ROOT)/up/gcc/c-family \
	-I$(ROOT)/up/gcc/common -I$(ROOT)/up/gcc/common/config/i386 \
	-I$(ROOT)/up/gcc/config \
	-I$(ROOT)/up/gcc/config/i386 -I$(ROOT)/up/libcpp \
	-I$(ROOT)/up/libcpp/include -I$(ROOT)/up/libiberty \
	-I$(ROOT)/conf/gmp -I$(ROOT)/conf/mpfr -I$(ROOT)/up/mpfr \
	-I$(ROOT)/up/mpc/src -I$(ROOT)/up/libdecnumber \
	-I$(ROOT)/up/libdecnumber/bid -I$(ROOT)/up/libgcc \
	-I$(ROOT)/up/zlib -I$(ROOT)/up/include
# The native GCC driver expands this deterministic profile into the long
# frontend flag/include vector after Make has spawned it. This keeps each job
# command below the in-OS Make command-buffer limit. Host validation may
# override GCC_FLAGS with the expanded flags above.
GCC_FLAGS ?= -ficsos-gcc-selfhost

include $(ROOT)/cc1-objs.mk

LIBCPP_NAMES := charset directives directives-only errors expr files identifiers \
	init lex line-map macro mkdeps pch symtab traditional
LIBIB_NAMES := argv basename choose-temp concat copysign cplus-dem crc32 \
	dyn-string ffs fibheap filename_cmp floatformat hashtab hex insque \
	lbasename make-relative-prefix make-temp-file md5 mempcpy objalloc \
	obstack partition splay-tree sort stack-limit stpcpy stpncpy strverscmp \
	unlink-if-ordinary xatexit xexit xmalloc xmemdup xstrdup xstrerror \
	xstrndup getopt getopt1 asprintf vasprintf cp-demangle safe-ctype \
	cp-demint physmem getruntime getpwd lrealpath
DEC_ROOT_NAMES := decContext decNumber
DEC_BID_NAMES := decimal32 decimal64 decimal128 bid2dpd_dpd2bid host-ieee32 \
	host-ieee64 host-ieee128
Z_NAMES := adler32 compress crc32 deflate infback inffast inflate inftrees \
	trees uncompr zutil

LIBCPP_OBJS := $(addprefix $(OBJ)/libcpp/,$(addsuffix .o,$(LIBCPP_NAMES)))
LIBIB_OBJS := $(addprefix $(OBJ)/libib/,$(addsuffix .o,$(LIBIB_NAMES)))
DEC_ROOT_OBJS := $(addprefix $(OBJ)/dec/,$(addsuffix .o,$(DEC_ROOT_NAMES)))
DEC_BID_OBJS := $(addprefix $(OBJ)/dec/bid/,$(addsuffix .o,$(DEC_BID_NAMES)))
DEC_OBJS := $(DEC_ROOT_OBJS) $(DEC_BID_OBJS)
Z_OBJS := $(addprefix $(OBJ)/z/,$(addsuffix .o,$(Z_NAMES)))
CC1_OBJS := $(addprefix $(OBJ)/cc1/,$(CC1_NAMES))
CC1_NORMAL_OBJS := $(filter-out $(OBJ)/cc1/ggc-page.o,$(CC1_OBJS))
CC1_A1_OBJS := $(wordlist 1,20,$(CC1_OBJS))
CC1_A2_OBJS := $(wordlist 21,40,$(CC1_OBJS))
CC1_A3_OBJS := $(wordlist 41,60,$(CC1_OBJS))
CC1_A4_OBJS := $(wordlist 61,80,$(CC1_OBJS))
CC1_A5_OBJS := $(wordlist 81,100,$(CC1_OBJS))
CC1_A6_OBJS := $(wordlist 101,120,$(CC1_OBJS))
CC1_A7_OBJS := $(wordlist 121,140,$(CC1_OBJS))
CC1_A8_OBJS := $(wordlist 141,160,$(CC1_OBJS))
CC1_A9_OBJS := $(wordlist 161,180,$(CC1_OBJS))
CC1_A10_OBJS := $(wordlist 181,200,$(CC1_OBJS))
CC1_A11_OBJS := $(wordlist 201,220,$(CC1_OBJS))
CC1_A12_OBJS := $(wordlist 221,240,$(CC1_OBJS))
CC1_A13_OBJS := $(wordlist 241,260,$(CC1_OBJS))
CC1_A14_OBJS := $(wordlist 261,280,$(CC1_OBJS))
CC1_A15_OBJS := $(wordlist 281,300,$(CC1_OBJS))
CC1_A16_OBJS := $(wordlist 301,320,$(CC1_OBJS))
CC1_A17_OBJS := $(wordlist 321,340,$(CC1_OBJS))
CC1_A18_OBJS := $(wordlist 341,349,$(CC1_OBJS))
CC1_ARCHIVES := $(OUT)/cc1-1.a $(OUT)/cc1-2.a $(OUT)/cc1-3.a \
	$(OUT)/cc1-4.a $(OUT)/cc1-5.a $(OUT)/cc1-6.a $(OUT)/cc1-7.a \
	$(OUT)/cc1-8.a $(OUT)/cc1-9.a $(OUT)/cc1-10.a $(OUT)/cc1-11.a \
	$(OUT)/cc1-12.a $(OUT)/cc1-13.a $(OUT)/cc1-14.a $(OUT)/cc1-15.a \
	$(OUT)/cc1-16.a $(OUT)/cc1-17.a $(OUT)/cc1-18.a

.PHONY: all frontend prepare
# Create every output directory once, up front. This is what makes `make -jN`
# safe: the per-object rules still call `mkdir -p` (idempotent), but by the
# time concurrent jobs start, the directories already exist, so no two jobs
# race on the same first-time directory creation.
prepare:
	$(MKDIR) -p $(OUT)
	$(MKDIR) -p $(OBJ)
	$(MKDIR) -p $(OBJ)/libcpp
	$(MKDIR) -p $(OBJ)/libib
	$(MKDIR) -p $(OBJ)/dec
	$(MKDIR) -p $(OBJ)/dec/bid
	$(MKDIR) -p $(OBJ)/z
	$(MKDIR) -p $(OBJ)/cc1
all: prepare $(OUT)/loop.o $(OUT)/as.exe $(OUT)/ld.exe
frontend: $(OUT)/cc1.exe

$(LIBCPP_OBJS): $(OBJ)/libcpp/%.o: $(ROOT)/up/libcpp/%.c
	$(MKDIR) -p $(dir $@)
	$(CC) -c $(BASE_FLAGS) $(COMMON_INC) $< -o $@
$(OUT)/libcpp.a: $(LIBCPP_OBJS)
	$(AR) rcs $@ $^

$(LIBIB_OBJS): $(OBJ)/libib/%.o: $(ROOT)/up/libiberty/%.c
	$(MKDIR) -p $(dir $@)
	$(CC) -c $(BASE_FLAGS) $(COMMON_INC) $< -o $@
$(OUT)/libib.a: $(LIBIB_OBJS)
	$(AR) rcs $@ $^

$(DEC_ROOT_OBJS): $(OBJ)/dec/%.o: $(ROOT)/up/libdecnumber/%.c
	$(MKDIR) -p $(dir $@)
	$(CC) -c $(BASE_FLAGS) $(DEC_INC) $< -o $@
$(DEC_BID_OBJS): $(OBJ)/dec/bid/%.o: $(ROOT)/up/libdecnumber/bid/%.c
	$(MKDIR) -p $(dir $@)
	$(CC) -c $(BASE_FLAGS) $(DEC_INC) $< -o $@
$(OUT)/libdec.a: $(DEC_OBJS)
	$(AR) rcs $@ $^

$(Z_OBJS): $(OBJ)/z/%.o: $(ROOT)/up/zlib/%.c
	$(MKDIR) -p $(dir $@)
	$(CC) -c $(BASE_FLAGS) $(Z_INC) $< -o $@
$(OUT)/libz.a: $(Z_OBJS)
	$(AR) rcs $@ $^

$(OBJ)/cc1/ggc-page.o: $(ROOT)/units/ggc-page.c
	$(MKDIR) -p $(dir $@)
	$(CC) -c $(GCC_FLAGS) -UHAVE_MMAP_ANON -UHAVE_MMAP_DEV_ZERO $< -o $@
$(CC1_NORMAL_OBJS): $(OBJ)/cc1/%.o: $(ROOT)/units/%.c
	$(MKDIR) -p $(dir $@)
	$(CC) -c $(GCC_FLAGS) $< -o $@
$(OUT)/cc1-1.a: $(CC1_A1_OBJS)
	$(AR) rcs $@ $^
$(OUT)/cc1-2.a: $(CC1_A2_OBJS)
	$(AR) rcs $@ $^
$(OUT)/cc1-3.a: $(CC1_A3_OBJS)
	$(AR) rcs $@ $^
$(OUT)/cc1-4.a: $(CC1_A4_OBJS)
	$(AR) rcs $@ $^
$(OUT)/cc1-5.a: $(CC1_A5_OBJS)
	$(AR) rcs $@ $^
$(OUT)/cc1-6.a: $(CC1_A6_OBJS)
	$(AR) rcs $@ $^
$(OUT)/cc1-7.a: $(CC1_A7_OBJS)
	$(AR) rcs $@ $^
$(OUT)/cc1-8.a: $(CC1_A8_OBJS)
	$(AR) rcs $@ $^
$(OUT)/cc1-9.a: $(CC1_A9_OBJS)
	$(AR) rcs $@ $^
$(OUT)/cc1-10.a: $(CC1_A10_OBJS)
	$(AR) rcs $@ $^
$(OUT)/cc1-11.a: $(CC1_A11_OBJS)
	$(AR) rcs $@ $^
$(OUT)/cc1-12.a: $(CC1_A12_OBJS)
	$(AR) rcs $@ $^
$(OUT)/cc1-13.a: $(CC1_A13_OBJS)
	$(AR) rcs $@ $^
$(OUT)/cc1-14.a: $(CC1_A14_OBJS)
	$(AR) rcs $@ $^
$(OUT)/cc1-15.a: $(CC1_A15_OBJS)
	$(AR) rcs $@ $^
$(OUT)/cc1-16.a: $(CC1_A16_OBJS)
	$(AR) rcs $@ $^
$(OUT)/cc1-17.a: $(CC1_A17_OBJS)
	$(AR) rcs $@ $^
$(OUT)/cc1-18.a: $(CC1_A18_OBJS)
	$(AR) rcs $@ $^

RUNTIME_NAMES := crt1 tccsdk libtcc1 posix setjmp
RUNTIME_OBJS := $(addprefix $(OUT)/,$(addsuffix .o,$(RUNTIME_NAMES)))
$(RUNTIME_OBJS): $(OUT)/%.o: $(ROOT)/sdk/%.c
	$(CC) -c $(BASE_FLAGS) -I$(ROOT)/sdk/include $< -o $@

$(OUT)/cc1.exe: $(CC1_ARCHIVES) $(OUT)/libib.a $(OUT)/libcpp.a \
	$(OUT)/libdec.a $(OUT)/libz.a $(RUNTIME_OBJS)
	$(LD) -T $(LDSCRIPT) $(RUNTIME_OBJS) --start-group $(CC1_ARCHIVES) \
		$(OUT)/libib.a $(OUT)/libcpp.a $(OUT)/libdec.a $(OUT)/libz.a \
		$(SEED)/libmpc.a $(SEED)/libmpfr.a $(SEED)/libgmp.a --end-group \
		-o $@

$(OUT)/gccnew.s: $(OUT)/cc1.exe $(ROOT)/gccdriver.c
	$(OUT)/cc1.exe -m64 -std=gnu89 -w -nostdinc -fno-builtin \
		-ffreestanding -fno-pie -fno-pic -fno-stack-protector \
		-fno-asynchronous-unwind-tables -fno-strict-aliasing \
		-mcmodel=large -mno-red-zone -I$(ROOT)/sdk/include \
		$(ROOT)/gccdriver.c -o $@
$(OUT)/gccdrv.o: $(OUT)/gccnew.s
	$(AS) --64 $< -o $@
$(OUT)/gcc.exe: $(OUT)/gccdrv.o $(RUNTIME_OBJS)
	$(LD) -T $(LDSCRIPT) $(OUT)/gccdrv.o $(RUNTIME_OBJS) -o $@
$(OUT)/as.exe:
	$(CP) $(AS) $@
$(OUT)/ld.exe:
	$(CP) $(LD) $@
$(OUT)/loop.o: $(OUT)/gcc.exe $(OUT)/as.exe $(OUT)/ld.exe
	$(OUT)/gcc.exe -B$(OUT) -c -m64 -std=gnu89 -w -nostdinc \
		-fno-builtin -ffreestanding -fno-pie -fno-pic -mcmodel=large \
		-mno-red-zone -I$(ROOT)/sdk/include $(ROOT)/gccdriver.c -o $@

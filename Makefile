# libmmd: Mermaid diagrams to SVG, in C.
# Needs GNU make 4.2 and a C compiler with -std=gnu2y: gcc 15 or later, or a recent clang.
#
#   make                      release build: library, shared library and CLI into build/release/
#   make MODE=debug           the same unoptimised, into build/debug/
#   make CC=clang CFLAGS=-march=native LDFLAGS=-fuse-ld=mold
#   make install [PREFIX=/usr] [DESTDIR=...]
#   make test                 what has to pass before a merge
#   make tools                every test, benchmark and developer program
#   make both [GCC=gcc] [CLANG=clang]   both compilers' builds of everything, warnings as errors
#   make asan | msan | oom    the tests under sanitizers and under allocation failure
#   make fuzz [N=1000] [SEED=1]
#   make pgo                  profile-guided build into build/pgo/
#
# What a build was made with is not remembered: a later make without the same CC, CFLAGS and so
# on builds again with the defaults, and `sudo make install` would do that as root. Settings
# that are meant to last go in config.mk (CC = clang, CFLAGS = -O3, PREFIX = /usr); the command
# line still wins over it.
#
# Link-time optimisation is a flag like any other, and the user's: CFLAGS reach the link too.
#   make CFLAGS='-flto=auto -ffat-lto-objects'
#   make CC=clang CFLAGS='-flto=thin -ffat-lto-objects' LDFLAGS=-fuse-ld=lld
# -ffat-lto-objects is for a libmmd.a that gets installed: it keeps machine code beside the
# compiler's own format, so any compiler and linker can still use it. lld reads clang's fat
# objects back for LTO; mold links their machine code instead, so with mold leave the flag out.
# The linker is one too: LDFLAGS=-fuse-ld=mold, and the same for wild, lld or bfd. gcc knows
# wild by that name from 16 on; before that, LDFLAGS=-B<a directory where ld is wild>.

VERSION := 0.1.0
SOMAJOR := 0

# optional, and the user's: see above
-include config.mk

PREFIX     ?= /usr/local
BINDIR     ?= $(PREFIX)/bin
LIBDIR     ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include
INSTALL    ?= install

MODE  ?= release
BUILD ?= build/$(MODE)

.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
MAKEFLAGS += -r

# ---- modes ----
# A mode is a set of optimisation flags and a directory of its own, so two modes never mix.
# release: speed and size that cost a user of the library nothing. Each function and object gets
# its own section and the link drops the ones nothing reaches, also from a program that links
# libmmd.a; calls between the library's exported functions are bound, and inlined, as within a
# program, not left open to LD_PRELOAD.
# asan and msan are what the targets of those names build with.
mode.release := -O2 -fno-semantic-interposition -ffunction-sections -fdata-sections
link.release := -Wl,--gc-sections
mode.debug   := -O0 -g3 -fno-omit-frame-pointer
san          := -O1 -g -fno-omit-frame-pointer -fno-sanitize-recover=all
mode.asan    := $(san) -fsanitize=address,undefined
mode.msan    := $(san) -fsanitize=memory -fsanitize-memory-track-origins
ifndef mode.$(MODE)
$(error MODE=$(MODE): expected release, debug, asan or msan)
endif

# ---- flags ----
# What the code needs comes first, then the mode: CPPFLAGS, CFLAGS, LDFLAGS and LDLIBS stay the
# user's, and win. `make CFLAGS=-O3` is a release build at -O3.
# -ffp-contract=off: with fused multiply-adds (-march=native) some layouts come out a digit apart,
# and the same source has to give the same SVG on every machine.
flags   := -std=gnu2y -fms-extensions -fno-math-errno -ffp-contract=off -fPIC -fvisibility=hidden \
           -Wall -Wextra -Wno-microsoft-anon-tag -Iinclude -Isrc -Itools -MMD -MP $(mode.$(MODE))
cflags  = $(flags) $(CPPFLAGS) $(CFLAGS)
ldflags = $(link.$(MODE)) $(LDFLAGS)
libs    = -lm $(LDLIBS)

# ---- layout ----
# What gets installed sits at the top of $(BUILD): libmmd.a, libmmd.so*, mmd.
# Everything else is where its source is: DIR/NAME.c gives $(BUILD)/DIR/NAME.o or $(BUILD)/DIR/NAME.
dirs := $(addprefix $(BUILD)/,src tools/qa tests bench)
objs := $(patsubst %.c,$(BUILD)/%.o,$(sort $(wildcard src/*.c)))
qa   := $(patsubst %.c,$(BUILD)/%.o,$(sort $(wildcard tools/qa/*.c)))
$(dirs):
	mkdir -p $@

# The compiler and its flags, in two files that are rewritten only when what they say changes.
# Objects depend on the first. Whatever is linked or archived depends on the second, which says
# all of the first too, since the link is given the compiler's flags as well, and lists the
# objects. So `make CFLAGS=-flto=auto` after `make` builds everything again,
# `make LDFLAGS=-fuse-ld=mold` only links again, a source that is added or deleted compiles
# nothing else but puts the libraries together again, and none of these mixes old with new.
sig.cflags  := $(strip $(CC) $(cflags))
sig.ldflags := $(strip $(sig.cflags) $(ldflags) $(libs) $(objs) $(qa))
cstamp      := $(BUILD)/cflags
lstamp      := $(BUILD)/ldflags
ifneq ($(sig.cflags),$(file <$(cstamp)))
$(cstamp): FORCE
endif
ifneq ($(sig.ldflags),$(file <$(lstamp)))
$(lstamp): FORCE
endif
$(cstamp) $(lstamp): export sig = $(sig.$(@F))
$(cstamp) $(lstamp): | $(dirs)
	@printf '%s\n' "$$sig" > $@
FORCE:

# ---- the library ----
soname := libmmd.so.$(SOMAJOR)
solib  := libmmd.so.$(VERSION)
libqa  := $(BUILD)/tools/qa/libqa.a

all: $(BUILD)/libmmd.a $(BUILD)/libmmd.so $(BUILD)/mmd
link = $(CC) $(cflags) $(ldflags) $(filter %.c %.a,$^) $(libs) -o $@

$(objs) $(qa): $(BUILD)/%.o: %.c $(cstamp)
	$(CC) $(cflags) -c $< -o $@
# rm: ar only adds, and would keep the object of a source that has since been renamed or deleted.
# $(lstamp) lists the objects, so a deletion that changes nothing else gets here too.
$(BUILD)/libmmd.a: $(objs) $(lstamp)
	rm -f $@
	$(AR) rcs $@ $(filter %.o,$^)
$(BUILD)/$(solib): $(objs) $(lstamp)
	$(CC) $(cflags) $(ldflags) -shared -Wl,-soname,$(soname) $(filter %.o,$^) $(libs) -o $@
$(BUILD)/$(soname): $(BUILD)/$(solib)
	ln -sf $(<F) $@
$(BUILD)/libmmd.so: $(BUILD)/$(soname)
	ln -sf $(<F) $@
$(BUILD)/mmd: tools/mmd.c $(BUILD)/libmmd.a $(lstamp)
	$(link)

# ---- test, benchmark and developer programs ----
# One source each, linked with the static library and the kit they share (tools/qa).
tools := $(patsubst %.c,$(BUILD)/%,$(filter-out tools/mmd.c,$(wildcard tools/*.c)))
tests := $(patsubst %.c,$(BUILD)/%,$(wildcard tests/*.c))
bench := $(patsubst %.c,$(BUILD)/%,$(wildcard bench/*.c))

tools: all $(tools) $(tests) $(bench)

$(libqa): $(qa) $(lstamp)
	rm -f $@
	$(AR) rcs $@ $(filter %.o,$^)
$(filter-out $(BUILD)/tests/shared,$(tools) $(tests) $(bench)): $(BUILD)/%: %.c $(libqa) $(BUILD)/libmmd.a $(lstamp)
	$(link)
# as a user of the installed library links it: -lmmd and no -lm, so that a libmmd.so which does
# not bring in what it needs itself fails here
$(BUILD)/tests/shared: tests/shared.c $(BUILD)/libmmd.so $(lstamp)
	$(CC) $(cflags) $(ldflags) $< -L$(BUILD) -lmmd $(LDLIBS) -Wl,-rpath,'$$ORIGIN/..' -o $@
# tests/oom.c fails the library's allocations one at a time
$(BUILD)/tests/oom: private ldflags += -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc

-include $(objs:.o=.d) $(qa:.o=.d) $(addsuffix .d,$(BUILD)/mmd $(tools) $(tests) $(bench))

# for clangd: the commands above, without running them. Written afresh each time it is asked for:
# it changes with the compiler, the flags and the list of sources, and takes no time.
comma  := ,
ccjson  = {"directory": "$(CURDIR)", "file": "$1", "command": "$(CC) $(cflags) -c $1"}
compile_commands.json: FORCE
	$(file >$@,[$(subst } {,}$(comma) {,$(strip $(foreach s,$(wildcard src/*.c tools/*.c tools/qa/*.c tests/*.c bench/*.c),$(call ccjson,$s))))])

# ---- install ----
install: all
	$(INSTALL) -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(INCLUDEDIR) $(DESTDIR)$(LIBDIR)/pkgconfig
	$(INSTALL) -m755 $(BUILD)/mmd $(DESTDIR)$(BINDIR)/
	$(INSTALL) -m644 include/mmd.h $(DESTDIR)$(INCLUDEDIR)/
	$(INSTALL) -m644 $(BUILD)/libmmd.a $(DESTDIR)$(LIBDIR)/
	$(INSTALL) -m755 $(BUILD)/$(solib) $(DESTDIR)$(LIBDIR)/
	ln -sf $(solib) $(DESTDIR)$(LIBDIR)/$(soname)
	ln -sf $(soname) $(DESTDIR)$(LIBDIR)/libmmd.so
	sed -e 's|@PREFIX@|$(PREFIX)|' -e 's|@LIBDIR@|$(LIBDIR)|' -e 's|@INCLUDEDIR@|$(INCLUDEDIR)|' \
	    -e 's|@VERSION@|$(VERSION)|' mmd.pc.in > $(DESTDIR)$(LIBDIR)/pkgconfig/mmd.pc

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/mmd $(DESTDIR)$(INCLUDEDIR)/mmd.h $(DESTDIR)$(LIBDIR)/pkgconfig/mmd.pc \
	      $(addprefix $(DESTDIR)$(LIBDIR)/,libmmd.a libmmd.so $(soname) $(solib))

# ---- tests ----
corpus := tests/cases/*.mmd tests/docs/*/*.mmd

test: all $(tests)
	$(BUILD)/tests/corpus $(BUILD)/mmd
	$(BUILD)/tests/shared
	$(BUILD)/tests/locale $(corpus)
	$(BUILD)/tests/writer
	$(BUILD)/tests/d3fmt tests/d3fmt_cases.tsv
	$(BUILD)/tests/mtheme tests/mtheme_cases.tsv
	$(BUILD)/tests/oom $(corpus)

# The targets below build somewhere of their own. BUILD is spelled out for each, since one given
# on the command line reaches the sub-make too; CFLAGS and the rest reach it as the user's.
# both and msan say which compiler as well: that is GCC and CLANG, not CC.
GCC   ?= gcc
CLANG ?= clang

both:
	$(MAKE) CC=$(GCC) BUILD=build/gcc CFLAGS='$(CFLAGS) -Werror' tools
	$(MAKE) CC=$(CLANG) BUILD=build/clang CFLAGS='$(CFLAGS) -Werror' tools

# The second half fills what malloc returns with a byte: output that changes with it was read
# from the arena before it was written. 0xA5 makes tiny negative doubles, 0x41 large positive ones.
asan:
	$(MAKE) MODE=asan BUILD=build/asan test
	for b in 165 65; do \
	    ASAN_OPTIONS=max_malloc_fill_size=1073741824:malloc_fill_byte=$$b build/asan/tests/corpus build/asan/mmd || exit; \
	done

# MemorySanitizer is clang's.
msan:
	$(MAKE) CC=$(CLANG) MODE=msan BUILD=build/msan test

# `make test` fails each malloc; with MMD_FAULT_INJECT the arena's allocations fail too.
oom:
	$(MAKE) MODE=asan BUILD=build/oom CPPFLAGS='$(CPPFLAGS) -DMMD_FAULT_INJECT' build/oom/tests/oom
	build/oom/tests/oom $(corpus)

N    ?= 1000
SEED ?= 1
fuzz:
	$(MAKE) MODE=asan BUILD=build/asan build/asan/mmd build/asan/tools/fuzz
	build/asan/tools/fuzz build/asan/mmd $(N) $(SEED)

# ---- profile-guided build ----
# A release build trained on the CLI rendering every input in the tree; CC, CFLAGS and LDFLAGS go
# with it. gcc names a profile after the object's path, so both stages build in the same
# directory. Only the CLI is trained: the other programs have a main() the profile does not fit.
# The compiler is asked what it is, since cc can be either, and only when this target runs.
# A training run may fail, as some inputs are meant to. That none of them left a profile may
# not: gcc would build on without one and say nothing.
pgo.dir   := $(abspath build/pgo)/profile
pgo.clang  = $(findstring clang,$(shell $(CC) --version 2>/dev/null))
pgo.use    = $(if $(pgo.clang),-Wno-backend-plugin,-fprofile-partial-training -Wno-missing-profile)
pgo:
	rm -rf build/pgo
	$(MAKE) MODE=release BUILD=build/pgo CFLAGS='$(CFLAGS) -fprofile-generate=$(pgo.dir)' build/pgo/mmd
	for f in $(corpus) tests/flow/*.mmd bench/*/*.mmd; do build/pgo/mmd $$f >/dev/null 2>&1 || :; done
	@test -n "$$(find $(pgo.dir) -name '*.gcda' -o -name '*.profraw' 2>/dev/null)" || \
	    { echo 'pgo: the training runs left no profile in $(pgo.dir)' >&2; exit 1; }
	$(if $(pgo.clang),$(shell $(CC) -print-prog-name=llvm-profdata) merge -o $(pgo.dir)/default.profdata $(pgo.dir))
	$(MAKE) MODE=release BUILD=build/pgo CFLAGS='$(CFLAGS) -fprofile-use=$(pgo.dir) $(pgo.use)' all build/pgo/bench/render

clean:
	rm -rf build

.PHONY: all tools install uninstall test both asan msan oom fuzz pgo clean FORCE

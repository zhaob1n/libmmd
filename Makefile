# libmmd: Mermaid diagrams to SVG, in C.
# Needs GNU make 4.3 and a C compiler with -std=gnu2y: gcc 15 or later, or a recent clang.
#
#   make                      release build: library, shared library and CLI into build/release/
#   make MODE=debug           the same unoptimised, into build/debug/
#   make LINK=shared          the CLI and the programs use libmmd.so, not a copy of libmmd.a
#   make CC=clang CFLAGS=-march=native LDFLAGS=-fuse-ld=mold
#   make install [PREFIX=/usr] [DESTDIR=...]
#   make test                 what has to pass before a merge
#   make tools                every test, benchmark and developer program
#   make both [GCC=gcc] [CLANG=clang]   both compilers' builds of everything, warnings as errors
#   make asan | msan | oom    the tests under sanitizers, with libmmd.so, and under allocation failure
#   make fuzz [N=1000] [SEED=1]
#   make pgo                  profile-guided build into build/pgo/
#
# What a build was made with is not remembered: a later make without the same CC, CFLAGS and so
# on builds again with the defaults, and `sudo make install` would do that as root. Settings
# that are meant to last go in config.mk (CC = clang, CFLAGS = -O3, PREFIX = /usr); the command
# line still wins over it.

VERSION := 0.1.0
SOMAJOR := 0

-include config.mk

PREFIX     ?= /usr/local
BINDIR     ?= $(PREFIX)/bin
LIBDIR     ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include
# INSTALL_PROGRAM='install -s' strips mmd and libmmd.so as they are installed
INSTALL         ?= install
INSTALL_PROGRAM ?= $(INSTALL)
INSTALL_DATA    ?= $(INSTALL) -m644

MODE  ?= release
BUILD ?= build/$(MODE)

.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
MAKEFLAGS += -r

# ---- modes ----
# release: speed and size that cost a user of the library nothing. Each function and object gets
# its own section and the link drops the ones nothing reaches, also from a program that links
# libmmd.a; calls between the library's exported functions are bound, and inlined, as within a
# program, not left open to LD_PRELOAD.
# -z defs: a libmmd.so that does not bring in what it uses itself (libm) fails to link. Not with
# the sanitizers, whose runtime clang leaves to the program.
mode.release := -O2 -fno-semantic-interposition -ffunction-sections -fdata-sections
link.release := -Wl,--gc-sections -Wl,-z,defs
mode.debug   := -O0 -g3 -fno-omit-frame-pointer
link.debug   := -Wl,-z,defs
san          := -O1 -g -fno-omit-frame-pointer -fno-sanitize-recover=all
mode.asan    := $(san) -fsanitize=address,undefined
# gnu2: clang keeps values in xmm16-31 across a call to __tls_get_addr, and MemorySanitizer's
# interceptor of it overwrites them (with AVX-512, libmmd.so read its theme numbers as 0).
# TLS descriptors do not call it.
mode.msan    := $(san) -fsanitize=memory -fsanitize-memory-track-origins -mtls-dialect=gnu2
ifndef mode.$(MODE)
$(error MODE=$(MODE): expected release, debug, asan or msan)
endif

# ---- flags ----
# What the code needs comes first, then the mode: CPPFLAGS, CFLAGS, LDFLAGS and LDLIBS stay the
# user's, and win. `make CFLAGS=-O3` is a release build at -O3.
# -ffp-contract=off: with fused multiply-adds (-march=native) some layouts come out a digit apart,
# and the same source has to give the same SVG on every machine.
# --as-needed: every link is given -lm, and a program that calls nothing in it, mmd with libmmd.so
# for one, should not name it.
flags   := -std=gnu2y -fms-extensions -fno-math-errno -ffp-contract=off -fPIC -fvisibility=hidden \
           -Wall -Wextra -Wno-microsoft-anon-tag -Iinclude -Itools -MMD -MP $(mode.$(MODE))
cflags  = $(flags) $(CPPFLAGS) $(CFLAGS)
ldflags = -Wl,--as-needed $(link.$(MODE)) $(LDFLAGS)
libs    = -lm $(LDLIBS)
# The library's own headers are for the library and the tests of its insides. Everything else is
# built as a user of the library is, from include/mmd.h.
priv.src/            := -Isrc
priv.tests/internal/ := -Isrc

# ---- linking ----
# The CLI, the tests, the tools and the benchmarks hold a copy of libmmd.a, and are linked again
# when it changes. LINK=shared makes them use libmmd.so: linked after it (|), and again only when
# their own objects, the flags or its soname change. They get no RUNPATH, so by hand they run
# with LD_LIBRARY_PATH=build/release.
# tests/internal always has libmmd.a: those tests call what libmmd.so does not export, or wrap
# the malloc it calls.
LINK ?= static
soname       := libmmd.so.$(SOMAJOR)
solib        := libmmd.so.$(VERSION)
lib.shared   := $(BUILD)/libmmd.so
lib.static   := $(BUILD)/libmmd.a
after.shared := |
ifndef lib.$(LINK)
$(error LINK=$(LINK): expected shared or static)
endif
libmmd := $(lib.$(LINK))

# ---- layout ----
src.lib   := $(wildcard src/*.c)
src.qa    := $(wildcard tools/qa/*.c)
src.test  := $(wildcard tests/*.c)
src.itest := $(wildcard tests/internal/*.c)
src.prog  := $(wildcard tools/*.c bench/*.c)
srcs      := $(src.lib) $(src.qa) $(src.test) $(src.itest) $(src.prog) bin/mmd.c
objs      := $(src.lib:%.c=$(BUILD)/%.o)
qa        := $(src.qa:%.c=$(BUILD)/%.o)
tests     := $(src.test:%.c=$(BUILD)/%)
itests    := $(src.itest:%.c=$(BUILD)/%)
progs     := $(src.prog:%.c=$(BUILD)/%)
dirs      := $(addprefix $(BUILD)/,src bin tools/qa tests/internal bench)
$(dirs):
	mkdir -p $@

# The compiler and its flags, in two files that are rewritten only when what they say changes.
# Objects depend on the first. Whatever is linked or archived depends on the second, which says
# all of the first too, since the link is given the compiler's flags as well, and lists the
# objects. So `make CFLAGS=-flto=auto` after `make` builds everything again,
# `make LDFLAGS=-fuse-ld=mold` only links again, a source that is added or deleted compiles
# nothing else but puts the libraries together again, and none of these mixes old with new.
sig.cflags  := $(strip $(CC) $(cflags))
sig.ldflags := $(strip $(sig.cflags) $(ldflags) $(libs) $(LINK) $(soname) $(objs) $(qa))
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
libqa := $(BUILD)/tools/qa/libqa.a

all: $(BUILD)/libmmd.a $(BUILD)/libmmd.so $(BUILD)/mmd

$(srcs:%.c=$(BUILD)/%.o): $(BUILD)/%.o: %.c $(cstamp)
	$(CC) $(priv.$(dir $<)) $(cflags) -c $< -o $@
# rm: ar only adds, and would keep the object of a source that has since been renamed or deleted.
# $(lstamp) lists the objects, so a deletion that changes nothing else gets here too.
$(BUILD)/libmmd.a: $(objs) $(lstamp)
	rm -f $@
	$(AR) rcs $@ $(objs)
$(BUILD)/$(solib): $(objs) $(lstamp)
	$(CC) $(cflags) $(ldflags) -shared -Wl,-soname,$(soname) $(objs) $(libs) -o $@
$(BUILD)/$(soname): $(BUILD)/$(solib)
	ln -sf $(<F) $@
$(BUILD)/libmmd.so: $(BUILD)/$(soname)
	ln -sf $(<F) $@
$(BUILD)/mmd: $(BUILD)/bin/mmd.o $(lstamp) $(after.$(LINK)) $(libmmd)
	$(CC) $(cflags) $(ldflags) $< $(libmmd) $(libs) -o $@

# ---- test, benchmark and developer programs ----
tools: all $(tests) $(itests) $(progs)

$(libqa): $(qa) $(lstamp)
	rm -f $@
	$(AR) rcs $@ $(qa)
$(tests) $(progs): $(BUILD)/%: $(BUILD)/%.o $(libqa) $(lstamp) $(after.$(LINK)) $(libmmd)
	$(CC) $(cflags) $(ldflags) $< $(libqa) $(libmmd) $(libs) -o $@
$(itests): $(BUILD)/%: $(BUILD)/%.o $(libqa) $(BUILD)/libmmd.a $(lstamp)
	$(CC) $(cflags) $(ldflags) $< $(libqa) $(BUILD)/libmmd.a $(libs) -o $@
# tests/internal/oom.c fails the library's allocations one at a time
$(BUILD)/tests/internal/oom: private ldflags += -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc

-include $(srcs:%.c=$(BUILD)/%.d)

# for clangd; written afresh each time, as it follows the compiler, the flags and the sources
comma  := ,
json    = $(subst ",\",$(subst \,\\,$1))
ccjson  = {"directory": "$(call json,$(CURDIR))", "file": "$1", "command": "$(call json,$(CC) $(priv.$(dir $1)) $(cflags) -c $1)"}
compile_commands.json: FORCE
	$(file >$@,[$(subst } {,}$(comma) {,$(strip $(foreach s,$(srcs),$(call ccjson,$s))))])

# ---- install ----
install: all
	$(INSTALL) -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(INCLUDEDIR) $(DESTDIR)$(LIBDIR)/pkgconfig
	$(INSTALL_PROGRAM) $(BUILD)/mmd $(DESTDIR)$(BINDIR)/
	$(INSTALL_DATA) include/mmd.h $(DESTDIR)$(INCLUDEDIR)/
	$(INSTALL_DATA) $(BUILD)/libmmd.a $(DESTDIR)$(LIBDIR)/
	$(INSTALL_PROGRAM) $(BUILD)/$(solib) $(DESTDIR)$(LIBDIR)/
	ln -sf $(solib) $(DESTDIR)$(LIBDIR)/$(soname)
	ln -sf $(soname) $(DESTDIR)$(LIBDIR)/libmmd.so
	sed -e 's|@PREFIX@|$(PREFIX)|' -e 's|@LIBDIR@|$(LIBDIR)|' -e 's|@INCLUDEDIR@|$(INCLUDEDIR)|' \
	    -e 's|@VERSION@|$(VERSION)|' mmd.pc.in | $(INSTALL_DATA) /dev/stdin $(DESTDIR)$(LIBDIR)/pkgconfig/mmd.pc

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/mmd $(DESTDIR)$(INCLUDEDIR)/mmd.h $(DESTDIR)$(LIBDIR)/pkgconfig/mmd.pc \
	      $(addprefix $(DESTDIR)$(LIBDIR)/,libmmd.a libmmd.so $(soname) $(solib))

# ---- tests ----
# With LINK=shared, a recipe that runs what it built says where libmmd.so is: $(out), which is
# $(BUILD) but for the targets further down that build somewhere of their own.
out     = $(BUILD)
ldpath := $(LD_LIBRARY_PATH)
export LD_LIBRARY_PATH = $(abspath $(out))$(ldpath:%=:%)
corpus := tests/cases/*.mmd tests/docs/*/*.mmd

test: all $(tests) $(itests)
	$(BUILD)/tests/internal/corpus $(BUILD)/mmd
	$(BUILD)/tests/api
	$(BUILD)/tests/locale $(corpus)
	$(BUILD)/tests/writer
	$(BUILD)/tests/internal/d3fmt tests/d3fmt_cases.tsv
	$(BUILD)/tests/internal/mtheme tests/mtheme_cases.tsv
	$(BUILD)/tests/internal/oom $(corpus)

# The targets below build somewhere of their own. BUILD is spelled out for each, since one given
# on the command line reaches the sub-make too; CFLAGS and the rest reach it as the user's.
# both and msan say which compiler as well: that is GCC and CLANG, not CC.
# asan, msan and fuzz say LINK=shared: `make test` holds libmmd.a, where a function that mmd.h
# forgot to export links all the same.
GCC   ?= gcc
CLANG ?= clang

both: both.gcc both.clang
both.gcc:   cc = $(GCC)
both.clang: cc = $(CLANG)
both.gcc both.clang: both.%:
	$(MAKE) CC=$(cc) BUILD=build/$* CFLAGS='$(CFLAGS) -Werror' tools

asan fuzz: out := build/asan
msan:      out := build/msan
oom:       out := build/oom

# The second half fills what malloc returns with a byte: output that changes with it was read
# from the arena before it was written. 0xA5 makes tiny negative doubles, 0x41 large positive ones.
asan:
	$(MAKE) MODE=asan BUILD=$(out) LINK=shared test
	for b in 165 65; do \
	    ASAN_OPTIONS=max_malloc_fill_size=1073741824:malloc_fill_byte=$$b $(out)/tests/internal/corpus $(out)/mmd || exit; \
	done

msan:
	$(MAKE) CC=$(CLANG) MODE=msan BUILD=$(out) LINK=shared test

# `make test` fails each malloc; with MMD_FAULT_INJECT the arena's allocations fail too.
oom:
	$(MAKE) MODE=asan BUILD=$(out) CPPFLAGS='$(CPPFLAGS) -DMMD_FAULT_INJECT' $(out)/tests/internal/oom
	$(out)/tests/internal/oom $(corpus)

N    ?= 1000
SEED ?= 1
fuzz:
	$(MAKE) MODE=asan BUILD=$(out) LINK=shared $(out)/mmd $(out)/tools/fuzz
	$(out)/tools/fuzz $(out)/mmd $(N) $(SEED)

# ---- profile-guided build ----
# A release build trained on the CLI rendering every input in the tree; CC, CFLAGS and LDFLAGS go
# with it. gcc names a profile after the object's path, so both stages build in the same
# directory. Only the CLI is trained: the other programs have a main() the profile does not fit.
# The compiler is asked what it is, since cc can be either, and only when this target is asked for.
# A training run may fail, as some inputs are meant to. That none of them left a profile may
# not: gcc would build on without one and say nothing.
ifneq ($(filter pgo,$(MAKECMDGOALS)),)
pgo.clang := $(findstring clang,$(shell $(CC) --version 2>/dev/null))
endif
pgo.use = $(if $(pgo.clang),-Wno-backend-plugin,-fprofile-partial-training -Wno-missing-profile)
pgo: out := build/pgo
pgo: pgo.dir = $(abspath $(out))/profile
pgo:
	rm -rf $(out)
	$(MAKE) MODE=release BUILD=$(out) CFLAGS='$(CFLAGS) -fprofile-generate=$(pgo.dir)' $(out)/mmd
	for f in $(corpus) tests/flow/*.mmd bench/*/*.mmd; do $(out)/mmd $$f >/dev/null 2>&1 || :; done
	@test -n "$$(find $(pgo.dir) -name '*.gcda' -o -name '*.profraw' 2>/dev/null)" || \
	    { echo 'pgo: the training runs left no profile in $(pgo.dir)' >&2; exit 1; }
	$(if $(pgo.clang),$$($(CC) -print-prog-name=llvm-profdata) merge -o $(pgo.dir)/default.profdata $(pgo.dir))
	$(MAKE) MODE=release BUILD=$(out) CFLAGS='$(CFLAGS) -fprofile-use=$(pgo.dir) $(pgo.use)' all $(out)/bench/render

clean:
	rm -rf build

.PHONY: all tools install uninstall test both both.gcc both.clang asan msan oom fuzz pgo clean FORCE

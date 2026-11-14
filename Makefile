# libmmd: Mermaid diagrams to SVG, in C.
# Needs GNU make and a C compiler with -std=gnu2y: gcc 15 or later, or a recent clang.
#
#   make                      library, shared library and CLI into build/
#   make CC=clang CFLAGS='-O2 -march=native' LTO=1
#   make install [PREFIX=/usr] [DESTDIR=...]
#   make test                 what has to pass before a merge
#   make tools                every test, benchmark and developer program
#   make both                 gcc and clang builds of everything, warnings as errors
#   make asan | msan | oom    the tests under sanitizers and under allocation failure
#   make fuzz [N=1000] [SEED=1]
#   make pgo                  profile-guided build into build-pgo/

VERSION := 0.3.0
SOMAJOR := 0

PREFIX     ?= /usr/local
BINDIR     ?= $(PREFIX)/bin
LIBDIR     ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include
INSTALL    ?= install

BUILD  ?= build
CFLAGS ?= -O2 -g
GCC    ?= gcc
CLANG  ?= clang

.DELETE_ON_ERROR:
.SUFFIXES:

# ---- flags ----
# What the code needs comes first: CPPFLAGS, CFLAGS, LDFLAGS and LDLIBS stay the user's, and win.
# -ffp-contract=off: with fused multiply-adds (-march=native) some layouts come out a digit apart,
# and the same source has to give the same SVG on every machine.
flags := -std=gnu2y -fms-extensions -fno-math-errno -ffp-contract=off -fPIC -fvisibility=hidden \
         -Wall -Wextra -Wno-microsoft-anon-tag -Iinclude -Isrc -Itools -MMD -MP
clang = $(findstring clang,$(shell $(CC) --version))

# Fat objects: libmmd.a keeps machine code, so any compiler and linker can still use it,
# and plain ar can index it. clang only reads the bitcode back through lld.
ifeq ($(LTO),1)
flags += -flto -ffat-lto-objects
ltold := $(if $(clang),-fuse-ld=lld)
endif

# PGO=generate|use is what `make pgo` passes down; the profile lives in the build directory.
# gcc names a profile after the object's path, so both stages have to build in the same one.
# Only the CLI is trained: the other programs have a main() the profile does not fit.
ifdef PGO
flags += -fprofile-$(PGO)=$(abspath $(BUILD))/profile
ifeq ($(PGO),use)
flags += $(if $(clang),-Wno-backend-plugin,-fprofile-partial-training -Wno-missing-profile)
endif
endif

cflags  = $(flags) $(CPPFLAGS) $(CFLAGS)
ldflags = $(ltold) $(LDFLAGS)
libs    = -lm $(LDLIBS)

# ---- the library ----
soname := libmmd.so.$(SOMAJOR)
solib  := libmmd.so.$(VERSION)
objs   := $(patsubst src/%.c,$(BUILD)/%.o,$(sort $(wildcard src/*.c)))

all: $(BUILD)/libmmd.a $(BUILD)/libmmd.so $(BUILD)/mmd
link = $(CC) $(cflags) $(ldflags) $(filter %.c %.a,$^) $(libs) -o $@

$(BUILD):
	mkdir -p $@
$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(cflags) -c $< -o $@
$(BUILD)/libmmd.a: $(objs)
	$(AR) rcs $@ $^
$(BUILD)/$(solib): $(objs)
	$(CC) $(cflags) $(ldflags) -shared -Wl,-soname,$(soname) $^ $(libs) -o $@
$(BUILD)/$(soname): $(BUILD)/$(solib)
	ln -sf $(<F) $@
$(BUILD)/libmmd.so: $(BUILD)/$(soname)
	ln -sf $(<F) $@
$(BUILD)/mmd: tools/mmd.c $(BUILD)/libmmd.a
	$(link)

# ---- test, benchmark and developer programs ----
# One source each, linked with the static library and the kit they share (tools/qa):
#   tools/NAME.c -> NAME   tests/NAME.c -> test-NAME   bench/NAME.c -> bench-NAME
qa    := $(patsubst tools/qa/%.c,$(BUILD)/%.o,$(sort $(wildcard tools/qa/*.c)))
tools := $(patsubst tools/%.c,$(BUILD)/%,$(wildcard tools/*.c))
tests := $(patsubst tests/%.c,$(BUILD)/test-%,$(wildcard tests/*.c))
bench := $(patsubst bench/%.c,$(BUILD)/bench-%,$(wildcard bench/*.c))

tools: all $(tools) $(tests) $(bench)

$(BUILD)/%.o: tools/qa/%.c | $(BUILD)
	$(CC) $(cflags) -c $< -o $@
$(BUILD)/libqa.a: $(qa)
	$(AR) rcs $@ $^
$(BUILD)/%: tools/%.c $(BUILD)/libqa.a $(BUILD)/libmmd.a
	$(link)
$(BUILD)/test-%: tests/%.c $(BUILD)/libqa.a $(BUILD)/libmmd.a
	$(link)
$(BUILD)/bench-%: bench/%.c $(BUILD)/libqa.a $(BUILD)/libmmd.a
	$(link)
# as a user of the installed library links it
$(BUILD)/test-shared: tests/shared.c $(BUILD)/libmmd.so
	$(CC) $(cflags) $(ldflags) $< -L$(BUILD) -lmmd -Wl,-rpath,'$$ORIGIN' -o $@
# tests/oom.c fails the library's allocations one at a time
$(BUILD)/test-oom: private ldflags += -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc

-include $(wildcard $(BUILD)/*.d)

# for clangd: the commands above, without running them
comma  := ,
ccjson  = {"directory": "$(CURDIR)", "file": "$1", "command": "$(CC) $(cflags) -c $1"}
compile_commands.json: Makefile
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
	$(BUILD)/test-corpus $(BUILD)/mmd
	$(BUILD)/test-shared
	$(BUILD)/test-locale $(corpus)
	$(BUILD)/test-writer
	$(BUILD)/test-d3fmt tests/d3fmt_cases.tsv
	$(BUILD)/test-mtheme tests/mtheme_cases.tsv
	$(BUILD)/test-oom $(corpus)

both:
	$(MAKE) CC=$(GCC) BUILD=build-gcc CFLAGS='$(CFLAGS) -Werror' tools
	$(MAKE) CC=$(CLANG) BUILD=build-clang CFLAGS='$(CFLAGS) -Werror' tools

san  := -O1 -g -fno-omit-frame-pointer -fno-sanitize-recover=all
asan := $(san) -fsanitize=address,undefined
msan := $(san) -fsanitize=memory -fsanitize-memory-track-origins

# The second half fills what malloc returns with a byte: output that changes with it was read
# from the arena before it was written. 0xA5 makes tiny negative doubles, 0x41 large positive ones.
asan:
	$(MAKE) BUILD=build-asan CFLAGS='$(asan)' test
	for b in 165 65; do \
	    ASAN_OPTIONS=max_malloc_fill_size=1073741824:malloc_fill_byte=$$b build-asan/test-corpus build-asan/mmd || exit; \
	done

# MemorySanitizer is clang's.
msan:
	$(MAKE) CC=$(CLANG) BUILD=build-msan CFLAGS='$(msan)' test

# `make test` fails each malloc; with MMD_FAULT_INJECT the arena's allocations fail too.
oom:
	$(MAKE) BUILD=build-oom CPPFLAGS=-DMMD_FAULT_INJECT CFLAGS='$(asan)' build-oom/test-oom
	build-oom/test-oom $(corpus)

N    ?= 1000
SEED ?= 1
fuzz:
	$(MAKE) BUILD=build-asan CFLAGS='$(asan)' build-asan/mmd build-asan/fuzz
	build-asan/fuzz build-asan/mmd $(N) $(SEED)

# ---- profile-guided build ----
# Trained on the CLI rendering every input in the tree: code a profile never saw is optimised for size.
pgo:
	rm -rf build-pgo
	$(MAKE) BUILD=build-pgo PGO=generate build-pgo/mmd
	for f in $(corpus) tests/flow/*.mmd bench/*/*.mmd; do build-pgo/mmd $$f >/dev/null 2>&1; done
	$(if $(clang),$(shell $(CC) -print-prog-name=llvm-profdata) merge -o build-pgo/profile/default.profdata build-pgo/profile)
	$(MAKE) -B BUILD=build-pgo PGO=use all build-pgo/bench-render

clean:
	rm -rf build build-*

.PHONY: all tools install uninstall test both asan msan oom fuzz pgo clean

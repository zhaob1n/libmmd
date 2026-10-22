CC ?= $(HOME)/.local/bin/clang
ifeq ($(origin CC),default)
CC := $(HOME)/.local/bin/clang
endif
IS_CLANG := $(shell $(CC) --version 2>/dev/null | grep -qi clang && echo 1)
WARN := -Wall -Wextra
ifeq ($(IS_CLANG),1)
WARN += -Wno-microsoft-anon-tag
endif
# Opt-in tuning: make MARCH=native, make LTO=1, make pgo (two-stage profile-guided build)
OPT ?= -O2
MARCH ?=
LTO ?=
PGO_GEN ?=
PGO_USE ?=
VERSION := 0.3.0
SOMAJOR := 0
PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib
INCDIR ?= $(PREFIX)/include
BINDIR ?= $(PREFIX)/bin
CFLAGS ?= $(OPT) -g
ifneq ($(MARCH),)
CFLAGS += -march=$(MARCH)
endif
AR := ar
ifeq ($(LTO),1)
CFLAGS += -flto
LDFLAGS += -flto
ifeq ($(IS_CLANG),1)
LDFLAGS += -fuse-ld=lld
AR := $(dir $(CC))llvm-ar
else
AR := gcc-ar
endif
endif
ifneq ($(PGO_GEN),)
CFLAGS += -fprofile-generate=$(PGO_GEN)
LDFLAGS += -fprofile-generate=$(PGO_GEN)
endif
ifneq ($(PGO_USE),)
ifeq ($(IS_CLANG),1)
CFLAGS += -fprofile-use=$(PGO_USE)/default.profdata -Wno-backend-plugin
else
CFLAGS += -fprofile-use=$(PGO_USE) -fprofile-correction -Wno-missing-profile
endif
endif
STD := $(shell echo 'int x;' | $(CC) -std=gnu2y -fsyntax-only -xc - >/dev/null 2>&1 && echo gnu2y || echo gnu23)
CFLAGS += -std=$(STD) -fms-extensions -fPIC -fvisibility=hidden $(WARN) -Iinclude -Isrc
LDLIBS = -lm
BUILD ?= build
DIAG_SRCS := $(wildcard src/d_*.c)
SRCS := src/base.c src/text.c src/flow_parse.c src/layout.c src/nsimplex.c src/flow_svg.c src/seq.c src/rich.c src/dgraph.c $(DIAG_SRCS) src/cmermaid.c
OBJS := $(SRCS:src/%.c=$(BUILD)/%.o)

SONAME := libcmermaid.so.$(SOMAJOR)

all: $(BUILD)/libcmermaid.a $(BUILD)/libcmermaid.so $(BUILD)/mmdc-c $(BUILD)/bench $(BUILD)/cmermaid.pc

$(BUILD)/%.o: src/%.c $(wildcard src/*.h) include/cmermaid.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD):
	mkdir -p $@
$(BUILD)/libcmermaid.a: $(OBJS)
	$(AR) rcs $@ $^
$(BUILD)/libcmermaid.so.$(VERSION): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -shared -Wl,-soname,$(SONAME) $^ $(LDLIBS) -o $@
$(BUILD)/libcmermaid.so: $(BUILD)/libcmermaid.so.$(VERSION)
	ln -sf libcmermaid.so.$(VERSION) $(BUILD)/$(SONAME)
	ln -sf libcmermaid.so.$(VERSION) $@
$(BUILD)/cmermaid.pc: cmermaid.pc.in Makefile | $(BUILD)
	sed -e 's|@PREFIX@|$(PREFIX)|' -e 's|@LIBDIR@|$(LIBDIR)|' -e 's|@INCDIR@|$(INCDIR)|' -e 's|@VERSION@|$(VERSION)|' $< > $@
$(BUILD)/mmdc-c: tools/mmdc.c $(BUILD)/libcmermaid.a
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(BUILD)/libcmermaid.a $(LDLIBS) -o $@
$(BUILD)/bench: bench/bench.c $(BUILD)/libcmermaid.a
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(BUILD)/libcmermaid.a $(LDLIBS) -o $@
$(BUILD)/test-shared: tests/shared_test.c $(BUILD)/libcmermaid.so $(BUILD)/cmermaid.pc
	$(CC) $(CFLAGS) $< -L$(BUILD) -lcmermaid -Wl,-rpath,'$$ORIGIN' -o $@

install: all
	install -d $(DESTDIR)$(LIBDIR)/pkgconfig $(DESTDIR)$(INCDIR) $(DESTDIR)$(BINDIR)
	install -m644 include/cmermaid.h $(DESTDIR)$(INCDIR)/
	install -m644 $(BUILD)/libcmermaid.a $(DESTDIR)$(LIBDIR)/
	install -m755 $(BUILD)/libcmermaid.so.$(VERSION) $(DESTDIR)$(LIBDIR)/
	ln -sf libcmermaid.so.$(VERSION) $(DESTDIR)$(LIBDIR)/$(SONAME)
	ln -sf $(SONAME) $(DESTDIR)$(LIBDIR)/libcmermaid.so
	install -m644 $(BUILD)/cmermaid.pc $(DESTDIR)$(LIBDIR)/pkgconfig/
	install -m755 $(BUILD)/mmdc-c $(DESTDIR)$(BINDIR)/

# Profile-guided build: train on bench inputs, then rebuild into build-pgo.
PGO_DIR := $(CURDIR)/build-pgo-prof
pgo:
	rm -rf $(PGO_DIR) build-pgo-gen build-pgo
	$(MAKE) BUILD=build-pgo-gen PGO_GEN=$(PGO_DIR) LTO=$(LTO) MARCH=$(MARCH) build-pgo-gen/bench
	for f in bench/inputs/*.mmd; do ./build-pgo-gen/bench $$f 20 >/dev/null; done
	if $(CC) --version | grep -qi clang; then $(HOME)/.local/bin/llvm-profdata merge -o $(PGO_DIR)/default.profdata $(PGO_DIR)/*.profraw; fi
	$(MAKE) BUILD=build-pgo PGO_USE=$(PGO_DIR) LTO=$(LTO) MARCH=$(MARCH)

test: all $(BUILD)/test-shared
	./tests/run_tests.sh $(BUILD)/mmdc-c
	$(BUILD)/test-shared

both:
	$(MAKE) CC=$(HOME)/.local/bin/clang BUILD=build
	$(MAKE) CC=gcc BUILD=build-gcc

asan:
	$(MAKE) BUILD=build-asan OPT="-O1 -fsanitize=address,undefined -fno-omit-frame-pointer" LDLIBS="-lm -fsanitize=address,undefined"

clean:
	rm -rf build build-gcc build-asan build-pgo build-pgo-gen build-pgo-prof
.PHONY: all test both asan clean install pgo

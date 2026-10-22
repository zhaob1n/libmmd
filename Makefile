CC ?= $(HOME)/.local/bin/clang
ifeq ($(origin CC),default)
CC := $(HOME)/.local/bin/clang
endif
IS_CLANG := $(shell $(CC) --version 2>/dev/null | grep -qi clang && echo 1)
WARN := -Wall -Wextra
ifeq ($(IS_CLANG),1)
WARN += -Wno-microsoft-anon-tag
endif
OPT ?= -O2 -march=native
CFLAGS ?= $(OPT) -g
STD := $(shell echo 'int x;' | $(CC) -std=gnu2y -fsyntax-only -xc - >/dev/null 2>&1 && echo gnu2y || echo gnu23)
CFLAGS += -std=$(STD) -fms-extensions $(WARN) -Iinclude -Isrc
LDLIBS = -lm
BUILD ?= build
SRCS := src/base.c src/text.c src/flow_parse.c src/layout.c src/nsimplex.c src/flow_svg.c src/seq.c src/cmermaid.c
OBJS := $(SRCS:src/%.c=$(BUILD)/%.o)

all: $(BUILD)/libcmermaid.a $(BUILD)/mmdc-c $(BUILD)/bench

$(BUILD)/%.o: src/%.c $(wildcard src/*.h) include/cmermaid.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD):
	mkdir -p $@
$(BUILD)/libcmermaid.a: $(OBJS)
	ar rcs $@ $^
$(BUILD)/mmdc-c: tools/mmdc.c $(BUILD)/libcmermaid.a
	$(CC) $(CFLAGS) $< $(BUILD)/libcmermaid.a $(LDLIBS) -o $@
$(BUILD)/bench: bench/bench.c $(BUILD)/libcmermaid.a
	$(CC) $(CFLAGS) $< $(BUILD)/libcmermaid.a $(LDLIBS) -o $@

test: all
	./tests/run_tests.sh $(BUILD)/mmdc-c

both:
	$(MAKE) CC=$(HOME)/.local/bin/clang BUILD=build
	$(MAKE) CC=gcc BUILD=build-gcc

asan:
	$(MAKE) BUILD=build-asan OPT="-O1 -fsanitize=address,undefined -fno-omit-frame-pointer" LDLIBS="-lm -fsanitize=address,undefined"

clean:
	rm -rf build build-gcc build-asan
.PHONY: all test both asan clean

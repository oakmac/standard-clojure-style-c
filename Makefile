# Makefile for Standard Clojure Style in C
#
# Works on Linux, macOS, and Windows via MSYS2 / Git Bash. On Windows with
# MSVC use CMake instead (see CMakeLists.txt and README.md).
#
#   make           build the static library and the standard-clj binary
#   make test      build and run the test suites
#   make test-asan build and run the test suites with Address + Undefined Behavior Sanitizer
#   make install   install the standard-clj binary to PREFIX/bin (default /usr/local)
#   make format    format the source code with clang-format
#   make format-check  fail if the source code is not formatted
#   make clean     remove build output

CC           ?= cc
CLANG_FORMAT ?= clang-format
BUILD        := build
PREFIX       ?= /usr/local

# the short git hash is shown by `standard-clj --version` (when available)
GIT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null)
ifneq ($(GIT_HASH),)
  VERSION_FLAGS := -DSCS_GIT_HASH=\"$(GIT_HASH)\"
endif

# Strict warnings, and every warning is an error.
WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes \
            -Wmissing-prototypes -Werror
CFLAGS   ?= -std=c99 -O2 $(WARNINGS)

# the files we format (vendor/ is left as-is)
SOURCES := standard-clojure-style.c standard-clojure-style.h cli/cli.c cli/cli.h cli/main.c \
           test/framework.h test/test_main.c test/test_cli.c

# cJSON (used by the CLI and the tests) is vendored, so it is compiled without -Werror
CJSON_CFLAGS := -std=c99 -O2

LIB_SOURCES := standard-clojure-style.c standard-clojure-style.h
CLI_SOURCES := cli/cli.c cli/cli.h cli/main.c

.PHONY: all test test-asan install format format-check clean

all: $(BUILD)/libstandard-clojure-style.a $(BUILD)/standard-clj

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/standard-clojure-style.o: standard-clojure-style.c standard-clojure-style.h | $(BUILD)
	$(CC) $(CFLAGS) -c standard-clojure-style.c -o $@

$(BUILD)/libstandard-clojure-style.a: $(BUILD)/standard-clojure-style.o
	$(AR) rcs $@ $^

$(BUILD)/cJSON.o: vendor/cjson/cJSON.c vendor/cjson/cJSON.h | $(BUILD)
	$(CC) $(CJSON_CFLAGS) -c vendor/cjson/cJSON.c -o $@

$(BUILD)/standard-clj: $(LIB_SOURCES) $(CLI_SOURCES) $(BUILD)/cJSON.o | $(BUILD)
	$(CC) $(CFLAGS) $(VERSION_FLAGS) standard-clojure-style.c cli/cli.c cli/main.c $(BUILD)/cJSON.o -o $@ -lm

# NOTE: the library test binary compiles standard-clojure-style.c via #include, so
# it is built from test/test_main.c (see the comment at the top of that file).
# The CLI test binary does the same with cli/cli.c.
$(BUILD)/test: test/test_main.c test/framework.h $(LIB_SOURCES) $(BUILD)/cJSON.o | $(BUILD)
	$(CC) $(CFLAGS) test/test_main.c $(BUILD)/cJSON.o -o $@ -lm

$(BUILD)/test-cli: test/test_cli.c test/framework.h $(LIB_SOURCES) cli/cli.c cli/cli.h $(BUILD)/cJSON.o | $(BUILD)
	$(CC) $(CFLAGS) test/test_cli.c standard-clojure-style.c $(BUILD)/cJSON.o -o $@ -lm

test: $(BUILD)/test $(BUILD)/test-cli
	$(BUILD)/test
	$(BUILD)/test-cli

ASAN_FLAGS := -std=c99 -g -O1 $(WARNINGS) -fsanitize=address,undefined -fno-omit-frame-pointer

$(BUILD)/test-asan: test/test_main.c test/framework.h $(LIB_SOURCES) vendor/cjson/cJSON.c | $(BUILD)
	$(CC) $(ASAN_FLAGS) test/test_main.c -o $@ -lm -Wno-error vendor/cjson/cJSON.c

$(BUILD)/test-cli-asan: test/test_cli.c test/framework.h $(LIB_SOURCES) cli/cli.c cli/cli.h vendor/cjson/cJSON.c | $(BUILD)
	$(CC) $(ASAN_FLAGS) test/test_cli.c standard-clojure-style.c -o $@ -lm -Wno-error vendor/cjson/cJSON.c

test-asan: $(BUILD)/test-asan $(BUILD)/test-cli-asan
	$(BUILD)/test-asan
	$(BUILD)/test-cli-asan

install: $(BUILD)/standard-clj
	install -d $(PREFIX)/bin
	install -m 755 $(BUILD)/standard-clj $(PREFIX)/bin/standard-clj

format:
	$(CLANG_FORMAT) -i $(SOURCES)

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(SOURCES)

clean:
	rm -rf $(BUILD)

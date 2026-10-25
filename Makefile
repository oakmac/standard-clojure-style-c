# Makefile for Standard Clojure Style in C
#
# Works on Linux, macOS, and Windows via MSYS2 / Git Bash. On Windows with
# MSVC use CMake instead (see CMakeLists.txt and README.md).
#
#   make           build the static library
#   make test      build and run the test suite
#   make test-asan build and run the test suite with Address + Undefined Behavior Sanitizer
#   make format    format the source code with clang-format
#   make format-check  fail if the source code is not formatted
#   make clean     remove build output

CC           ?= cc
CLANG_FORMAT ?= clang-format
BUILD        := build

# Strict warnings, and every warning is an error.
# NOTE: -Wno-unused-function is TEMPORARY while the port is in progress: helper
# functions are ported (and tested) before the code that calls them exists.
# Remove it once the port is complete.
WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes \
            -Wmissing-prototypes -Wno-unused-function -Werror
CFLAGS   ?= -std=c99 -O2 $(WARNINGS)

SOURCES := standard-clojure-style.c standard-clojure-style.h test/test_main.c

.PHONY: all test test-asan format format-check clean

all: $(BUILD)/libstandard-clojure-style.a

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/standard-clojure-style.o: standard-clojure-style.c standard-clojure-style.h | $(BUILD)
	$(CC) $(CFLAGS) -c standard-clojure-style.c -o $@

$(BUILD)/libstandard-clojure-style.a: $(BUILD)/standard-clojure-style.o
	$(AR) rcs $@ $^

# NOTE: the test binary compiles standard-clojure-style.c via #include, so it is
# built from test/test_main.c alone (see the comment at the top of that file)
$(BUILD)/test: test/test_main.c standard-clojure-style.c standard-clojure-style.h | $(BUILD)
	$(CC) $(CFLAGS) test/test_main.c -o $@

test: $(BUILD)/test
	$(BUILD)/test

$(BUILD)/test-asan: test/test_main.c standard-clojure-style.c standard-clojure-style.h | $(BUILD)
	$(CC) -std=c99 -g -O1 $(WARNINGS) -fsanitize=address,undefined -fno-omit-frame-pointer \
	  test/test_main.c -o $@

test-asan: $(BUILD)/test-asan
	$(BUILD)/test-asan

format:
	$(CLANG_FORMAT) -i $(SOURCES)

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(SOURCES)

clean:
	rm -rf $(BUILD)

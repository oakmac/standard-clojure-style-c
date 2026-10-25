# Standard Clojure Style in C

A port of [Standard Clojure Style] to C. Standard Clojure Style is a formatter for Clojure code
that follows a small set of simple, opinionated rules.

[Standard Clojure Style]:https://github.com/oakmac/standard-clojure-style-js

> **Status: work in progress.** This library does not format anything yet. The port is being
> built up in stages (see [Roadmap](#roadmap)) and will be ready to use when every test case from
> the JavaScript implementation passes.

## Goals

- **One C file.** The entire formatter lives in [`standard-clojure-style.c`](standard-clojure-style.c)
  with a small public header, [`standard-clojure-style.h`](standard-clojure-style.h). Drop both
  into your project, or build the static library.
- **Portable.** Plain C99 with no dependencies beyond the C standard library. Builds with GCC,
  Clang, and MSVC on Linux, macOS, and Windows.
- **In sync with the JavaScript implementation.** This is as close to a one-to-one port of
  `lib/standard-clojure-style.js` as C allows, and it is tested against the exact same test cases.
  Same input, same output.
- **Trustworthy.** A formatter rewrites your source code, so there are no clever tricks here: simple
  data structures, explicit memory management, strict compiler warnings, and sanitizer runs in CI.

## Public API

```c
#include "standard-clojure-style.h"

scs_format_result result = scs_format(source, source_len);
if (result.status == SCS_STATUS_SUCCESS) {
  fwrite(result.out, 1, result.out_len, stdout);
} else {
  fprintf(stderr, "error: %s\n", result.reason);
}
scs_format_result_free(&result);
```

The header documents every field. The API above exists today, but `scs_format` reports an error
until the port is complete. A `scs_parse` function that exposes the parse tree (the JavaScript
`parse` function) will be added along with the parser.

## Design Notes

- **Memory: one arena per call.** Formatting a file allocates many small objects whose lifetimes
  all end when `scs_format` returns. Every allocation comes from an arena (a bump allocator) that is
  released in one step at the end of the call. Nothing is freed individually, so there is no
  per-allocation bookkeeping and nothing to leak.
- **Errors: `setjmp` / `longjmp`.** A few conditions stop formatting outright (for example a `:use`
  inside the `ns` form, or running out of memory). `scs_format` calls `setjmp` once; any internal
  function can call `scs_throw`, which jumps back to `scs_format` with a message. Because all memory
  is in the arena, jumping out never leaks, and no function in between has to propagate error codes.
- **Strings are UTF-8 and indexes count code points.** The string helpers (`str_len`, `char_at`,
  `substr`, ...) count Unicode code points rather than bytes. This is what makes column alignment
  work for non-ASCII source code. Strings carry an explicit byte length, so a NUL byte in the input
  cannot truncate anything.
- **Integers are `int`.** Every index, length, and count in the formatting algorithm is an `int`,
  including the `-1` "not found" sentinels. Inputs larger than `INT_MAX` bytes are rejected up
  front.
- **No regular expressions.** The grammar needs a handful of small patterns (tokens, whitespace,
  comments, string bodies, ...). Each one is a short hand-written matcher with its own tests.
- **Output goes into growable buffers.** The formatter appends to a growable byte buffer rather
  than building the output by repeated string concatenation, so formatting a file is linear in its
  size.
- **No global state.** All state for one `scs_format` call lives in a context struct that is passed
  explicitly to every function. The grammar is immutable static data. The library is safe to call
  from multiple threads.
- **Nesting limit.** The parser is recursive, so pathological input (thousands of nested parens)
  could overflow the C stack. Instead, collections and reader macros may nest at most 500 levels
  deep; beyond that `scs_format` returns an error. Each level costs roughly 1 KB of stack, so the
  default stays well inside the 1 MB default thread stack on Windows. Real code nests a few dozen
  levels at most. Build with `-DSCS_MAX_NESTING_DEPTH=N` to change the limit.

## Development

You need a C compiler (GCC, Clang, or MSVC) and either `make` or CMake. For formatting you need
[clang-format] version 18 (other versions may format slightly differently).

[clang-format]:https://clang.llvm.org/docs/ClangFormat.html

```sh
# build and run the unit tests
make test

# run the unit tests with Address Sanitizer and Undefined Behavior Sanitizer
make test-asan

# format the code / check that the code is formatted
make format
make format-check

# run only the tests whose name contains some text
./build/test "parser: Simple Map"
```

The tests read the fixture files in `test_cases/` relative to the current directory, so run them from
the repository root (`make test` and `ctest` both do).

With CMake (this is what Windows / MSVC users should use):

```sh
cmake -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

All compiler warnings are turned on and treated as errors, and CI runs the test suite on Linux,
macOS, and Windows plus a sanitizer build on Linux.

The test suite is a single C file, [`test/test_main.c`](test/test_main.c), that `#include`s
`standard-clojure-style.c` directly so that it can test the internal (`static`) functions. Its one
dependency is [cJSON](https://github.com/DaveGamble/cJSON), vendored under `test/cjson/`, which
reads the expected structures in the `ns` test cases. The library itself has no dependencies.

## Project Layout

```
standard-clojure-style.c    the library (everything is in this one file)
standard-clojure-style.h    the public API
test/test_main.c            the test suite
test/cjson/                 cJSON, used by the test suite to read JSON (vendored, do not edit)
test_cases/                 test cases generated by the JavaScript project (do not edit)
Makefile                    build for Linux, macOS, MSYS2
CMakeLists.txt              build for Windows / MSVC and IDEs
```

## Keeping in Sync with Standard Clojure Style JavaScript

The JavaScript project is the source of truth for behavior. Two things keep this port honest:

1. **Same structure.** `standard-clojure-style.c` follows `lib/standard-clojure-style.js` section
   by section and function by function (JavaScript `camelCase` names become C `snake_case`), so a
   change to the JavaScript can be located and ported without re-deriving anything.
2. **Same test cases.** The JavaScript project generates its format, parser, and `ns` test cases as
   JSON files (`scripts/generate-test-json-files.js`) for the other language ports. This port runs
   those exact files. They are copied here verbatim and must not be edited by hand.

Like the [Lua port], releases of this library skip version numbers in order to match the version of
Standard Clojure Style JavaScript they implement.

[Lua port]:https://github.com/oakmac/standard-clojure-style-lua

## Roadmap

- [x] project skeleton: build, public header, arena allocator, error handling, test runner
- [x] string, stack, and array helpers, tested against the JavaScript internals tests
- [x] parser (terminal parsers, sequence parsers, grammar), tested against `test_parser/`
- [x] `ns` form parsing, tested against `test_parse_ns/`
- [ ] formatter, tested against `test_format/`
- [ ] first release

## License

[ISC License](LICENSE.md)

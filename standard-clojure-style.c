/*
 * standard-clojure-style.c - an implementation of Standard Clojure Style in C
 * https://github.com/oakmac/standard-clojure-style-c
 *
 * Copyright (c) 2026, Chris Oakman
 * Released under the ISC license
 * https://github.com/oakmac/standard-clojure-style-c/blob/master/LICENSE.md
 *
 * This is a port of Standard Clojure Style JavaScript:
 * https://github.com/oakmac/standard-clojure-style-js
 *
 * Everything in this file except the public API is `static`. The test suite
 * (test/test_main.c) #includes this file directly in order to test the internal
 * functions.
 */

#include "standard-clojure-style.h"

#include <limits.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* -----------------------------------------------------------------------------
 * Compiler Portability
 */

/* marks a function that never returns (used for error handling below) */
#if defined(__GNUC__) || defined(__clang__)
#define SCS_NORETURN __attribute__((noreturn))
#elif defined(_MSC_VER)
#define SCS_NORETURN __declspec(noreturn)
#else
#define SCS_NORETURN
#endif

/* -----------------------------------------------------------------------------
 * Memory Management: Arena
 *
 * Formatting a file allocates many small objects (parse tree nodes, strings,
 * arrays) whose lifetimes all end at the same moment: when scs_format()
 * returns. An arena (also called a "bump allocator" or "region") fits this
 * pattern exactly:
 *
 * - memory is handed out by bumping an offset inside a large block
 * - when the current block is full, a new block is allocated and linked to it
 * - nothing is ever freed individually; the whole arena is released at once
 *
 * There is no bookkeeping per allocation, nothing to free along the way, and
 * nothing that can leak or be freed twice. Every allocation is zero-initialized
 * and aligned to SCS_ARENA_ALIGN bytes.
 */

/* alignment of every pointer returned by the arena, in bytes */
#define SCS_ARENA_ALIGN ((size_t)16)

/* usable size of a block unless a single allocation needs more */
#define SCS_ARENA_DEFAULT_BLOCK_SIZE ((size_t)64 * 1024)

typedef struct scs_arena_block scs_arena_block;

struct scs_arena_block {
  scs_arena_block *prev; /* the block that was current before this one (or NULL) */
  size_t capacity;       /* usable bytes following the header */
  size_t used;           /* bytes handed out from this block so far */
};

/* The usable memory of a block starts this many bytes after the block pointer.
 * The header size is rounded up so that the first allocation is aligned. */
#define SCS_ARENA_BLOCK_HEADER_SIZE                                                                \
  (((sizeof(scs_arena_block) + SCS_ARENA_ALIGN - 1) / SCS_ARENA_ALIGN) * SCS_ARENA_ALIGN)

typedef struct scs_arena {
  scs_arena_block *current; /* the block allocations are currently served from (or NULL) */
  size_t block_size;        /* minimum capacity of a newly created block */
} scs_arena;

static void arena_init(scs_arena *arena, size_t block_size) {
  arena->current = NULL;
  arena->block_size = block_size;
}

static char *arena_block_data(scs_arena_block *block) {
  return (char *)block + SCS_ARENA_BLOCK_HEADER_SIZE;
}

/* Creates a new block with at least min_capacity usable bytes and makes it the
 * current block. Returns false if the system allocator failed. */
static bool arena_add_block(scs_arena *arena, size_t min_capacity) {
  size_t capacity = arena->block_size;
  if (capacity < min_capacity) {
    capacity = min_capacity;
  }
  if (capacity > SIZE_MAX - SCS_ARENA_BLOCK_HEADER_SIZE) {
    return false;
  }

  /* calloc zeroes the whole block up front. Every byte of a block is handed
   * out at most once, so every allocation is zero-initialized. */
  scs_arena_block *block = calloc(1, SCS_ARENA_BLOCK_HEADER_SIZE + capacity);
  if (!block) {
    return false;
  }

  block->prev = arena->current;
  block->capacity = capacity;
  block->used = 0;
  arena->current = block;
  return true;
}

/* Returns a pointer to `size` bytes of zero-initialized memory that stays valid
 * until arena_free_all() is called. Returns NULL if the system allocator failed.
 * A size of 0 still returns a valid (unique) pointer. */
static void *arena_try_alloc(scs_arena *arena, size_t size) {
  if (size == 0) {
    size = 1;
  }
  if (size > SIZE_MAX - (SCS_ARENA_ALIGN - 1)) {
    return NULL;
  }
  /* round up to a multiple of the alignment so the next allocation is aligned too */
  size = ((size + SCS_ARENA_ALIGN - 1) / SCS_ARENA_ALIGN) * SCS_ARENA_ALIGN;

  scs_arena_block *block = arena->current;
  if (!block || block->capacity - block->used < size) {
    if (!arena_add_block(arena, size)) {
      return NULL;
    }
    block = arena->current;
  }

  void *ptr = arena_block_data(block) + block->used;
  block->used += size;
  return ptr;
}

/* Releases every block. The arena can be used again afterwards. */
static void arena_free_all(scs_arena *arena) {
  scs_arena_block *block = arena->current;
  while (block) {
    scs_arena_block *prev = block->prev;
    free(block);
    block = prev;
  }
  arena->current = NULL;
}

/* -----------------------------------------------------------------------------
 * Context and Error Handling
 *
 * A few conditions stop formatting outright: the ns form contains :use, a
 * :gen-class value is malformed, or the system is out of memory. These are
 * reported with setjmp()/longjmp(), the standard C mechanism for a non-local
 * exit. scs_format() calls setjmp() once. Any internal function can call
 * scs_throw(), which records a message and jumps straight back to scs_format().
 * Because all memory lives in the arena, nothing leaks when we jump, and no
 * function in between needs to check or propagate an error code.
 *
 * All mutable state for one scs_format() call lives in an scs_ctx that is
 * passed explicitly to every function. There is no global mutable state in
 * this library.
 */

typedef struct scs_ctx {
  scs_arena arena;
  jmp_buf error_jmp;
  const char *error_msg; /* NULL until an error is thrown; a static or arena string */
} scs_ctx;

/* records an error message and jumps back to scs_format() */
static SCS_NORETURN void scs_throw(scs_ctx *ctx, const char *msg) {
  ctx->error_msg = msg;
  longjmp(ctx->error_jmp, 1);
}

/* allocates zero-initialized memory from the arena; throws on out-of-memory */
static void *scs_alloc(scs_ctx *ctx, size_t size) {
  void *ptr = arena_try_alloc(&ctx->arena, size);
  if (!ptr) {
    scs_throw(ctx, "out of memory");
  }
  return ptr;
}

/* -----------------------------------------------------------------------------
 * Strings
 *
 * Strings are immutable values: a pointer plus a byte length. The bytes are
 * always followed by a NUL terminator so they can be handed to the C standard
 * library, but `len` is authoritative (the input may contain NUL bytes).
 *
 * All text is UTF-8. The indexes and lengths used by the string helpers
 * (str_len, char_at, substr, ...) count Unicode code points, not bytes. This is
 * what makes column alignment work for non-ASCII source code. Byte lengths are
 * only used internally, for memory management.
 *
 * Every index, length, and count in the formatting algorithm is an `int`,
 * including the -1 "not found" sentinel values. Inputs larger than INT_MAX
 * bytes are rejected at the public API boundary.
 */

typedef struct scs_str {
  const char *data; /* never NULL; points at "" for an empty string */
  int len;          /* length in bytes, not counting the NUL terminator */
} scs_str;

/* -----------------------------------------------------------------------------
 * Node Types
 */

typedef struct scs_node scs_node;

/* a growable array of nodes */
typedef struct scs_node_list {
  scs_node **items; /* NULL until the first item is added */
  int size;
  int capacity;
} scs_node_list;

/* An AST node:
 * - start_idx: start position in the input (inclusive), in code points
 * - end_idx: end position in the input (exclusive), in code points
 * - children: child nodes
 * - name: name of the node (a static string such as "token" or ".open");
 *   NULL for an unnamed node
 * - text: raw text of the node (only for terminal nodes like strings and tokens);
 *   empty for every other node */
struct scs_node {
  int id;
  const char *name;
  scs_str text;
  int start_idx;
  int end_idx;
  scs_node_list children;
  /* these fields are used by the format_nodes function */
  int orig_col_idx;
  int printed_col_idx;
  int printed_line_idx;
  bool was_slurped_up;
};

/* -----------------------------------------------------------------------------
 * Public API
 */

/* Copies a NUL-terminated string into malloc'd memory so it can outlive the
 * arena. Returns NULL if the allocation failed.
 * (strdup() is POSIX, not C99, so we write our own.) */
static char *cstr_dup_malloc(const char *s) {
  size_t len = strlen(s);
  char *copy = malloc(len + 1);
  if (!copy) {
    return NULL;
  }
  memcpy(copy, s, len + 1);
  return copy;
}

static scs_format_result error_result(const char *reason) {
  scs_format_result result;
  memset(&result, 0, sizeof(result));
  result.status = SCS_STATUS_ERROR;
  result.reason = cstr_dup_malloc(reason);
  return result;
}

const char *scs_version(void) {
  return SCS_VERSION;
}

/* Parses input_txt (Clojure code) and returns it formatted according to
 * Standard Clojure Style. See standard-clojure-style.h for details. */
scs_format_result scs_format(const char *input, size_t input_len) {
  if (input == NULL) {
    return error_result("input must not be NULL");
  }
  if (input_len > (size_t)INT_MAX) {
    return error_result("input is too large (more than INT_MAX bytes)");
  }

  /* The context is heap allocated on purpose. C requires local variables that
   * change between setjmp() and longjmp() to be `volatile`; keeping all of our
   * state behind a pointer that never changes sidesteps that rule entirely. */
  scs_ctx *ctx = calloc(1, sizeof(scs_ctx));
  if (!ctx) {
    return error_result("out of memory");
  }
  arena_init(&ctx->arena, SCS_ARENA_DEFAULT_BLOCK_SIZE);

  if (setjmp(ctx->error_jmp) == 0) {
    /* TODO: the formatter has not been ported yet. Until it is, every call
     * reports an error rather than pretending to have formatted anything. */
    scs_throw(ctx, "scs_format is not implemented yet");
  }

  /* we only get here via scs_throw() */
  scs_format_result result = error_result(ctx->error_msg);
  arena_free_all(&ctx->arena);
  free(ctx);
  return result;
}

void scs_format_result_free(scs_format_result *result) {
  if (!result) {
    return;
  }
  free(result->out);
  free(result->reason);
  result->out = NULL;
  result->out_len = 0;
  result->reason = NULL;
}

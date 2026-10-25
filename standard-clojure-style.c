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
  int id_counter;        /* the last id handed out by create_id() */
  int nesting_depth;     /* how deeply nested the form being parsed is */
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
 * Strings are immutable views: a pointer plus a byte length. They are NOT
 * guaranteed to be NUL-terminated (a substring is a view into its parent, with
 * no copy), so always use `len` rather than strlen(). Strings created in the
 * arena do get an extra NUL byte after their contents, purely as a convenience
 * when printing them while debugging.
 *
 * All text is UTF-8. The indexes and lengths used by the string helpers
 * (str_len, char_at, substr, str_last_index_of, ...) count Unicode code points,
 * not bytes. This is what makes column alignment work for non-ASCII source
 * code. Byte lengths are only used internally, for memory management and for
 * searching. Malformed UTF-8 never causes an error: an invalid byte is treated
 * as a one-byte character and passed through untouched.
 *
 * Every index, length, and count in the formatting algorithm is an `int`,
 * including the -1 "not found" sentinel values. Inputs larger than INT_MAX
 * bytes are rejected at the public API boundary.
 */

typedef struct scs_str {
  const char *data; /* may be NULL only when len is 0 (a zeroed scs_str is a valid empty string) */
  int len;          /* length in bytes */
} scs_str;

static const scs_str EMPTY_STR = {"", 0};

/* memcmp() and memcpy() must not be given a NULL pointer, even for zero bytes,
 * so every comparison and copy of string bytes goes through these two helpers */
static bool bytes_eq(const char *a, const char *b, int len) {
  return len == 0 || memcmp(a, b, (size_t)len) == 0;
}

static void copy_bytes(char *dst, const char *src, int len) {
  if (len > 0) {
    memcpy(dst, src, (size_t)len);
  }
}

/* makes an scs_str from a string literal (compile-time length) */
#define STR(literal) ((scs_str){(literal), (int)(sizeof(literal) - 1)})

/* makes an scs_str from a NUL-terminated string (runtime length) */
static scs_str str_from_cstr(const char *s) {
  scs_str result;
  result.data = s;
  result.len = (int)strlen(s);
  return result;
}

static bool str_eq(scs_str a, scs_str b) {
  return a.len == b.len && bytes_eq(a.data, b.data, a.len);
}

static bool str_eq_cstr(scs_str a, const char *b) {
  return str_eq(a, str_from_cstr(b));
}

/* orders two strings by their bytes (which orders UTF-8 text by code point);
 * returns a negative number, zero, or a positive number */
static int str_compare(scs_str a, scs_str b) {
  int min_len = a.len < b.len ? a.len : b.len;
  int cmp = min_len == 0 ? 0 : memcmp(a.data, b.data, (size_t)min_len);
  if (cmp != 0) {
    return cmp;
  }
  if (a.len == b.len) {
    return 0;
  }
  return a.len < b.len ? -1 : 1;
}

/* does the string contain at least one character? */
static bool str_has_chars(scs_str s) {
  return s.len > 0;
}

/* Returns the number of bytes in the UTF-8 character that starts at byte_idx
 * (1 to 4). An invalid lead byte, a stray continuation byte, or a character
 * that is cut off by the end of the string counts as one byte, so a loop that
 * steps by this value always makes progress and always stays in bounds. */
static int utf8_char_bytes(scs_str s, int byte_idx) {
  unsigned char lead = (unsigned char)s.data[byte_idx];
  int num_bytes = 1;
  if (lead >= 0xF0 && lead <= 0xF7) {
    num_bytes = 4;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    num_bytes = 3;
  } else if (lead >= 0xC0 && lead <= 0xDF) {
    num_bytes = 2;
  }
  int remaining = s.len - byte_idx;
  if (num_bytes > remaining) {
    num_bytes = remaining;
  }
  return num_bytes;
}

/* Decodes the UTF-8 character that starts at byte_idx. Returns its code point,
 * or -1 if the bytes are not valid UTF-8. *num_bytes receives the number of
 * bytes consumed (see utf8_char_bytes). */
static int utf8_decode(scs_str s, int byte_idx, int *num_bytes) {
  int n = utf8_char_bytes(s, byte_idx);
  *num_bytes = n;

  unsigned char lead = (unsigned char)s.data[byte_idx];
  if (n == 1) {
    return lead < 0x80 ? (int)lead : -1;
  }

  int code_point = 0;
  if (n == 2) {
    code_point = lead & 0x1F;
  } else if (n == 3) {
    code_point = lead & 0x0F;
  } else {
    code_point = lead & 0x07;
  }

  for (int i = 1; i < n; i++) {
    unsigned char b = (unsigned char)s.data[byte_idx + i];
    if ((b & 0xC0) != 0x80) {
      return -1;
    }
    code_point = (code_point << 6) | (b & 0x3F);
  }
  return code_point;
}

/* returns the length of a string in code points */
static int str_len(scs_str s) {
  int count = 0;
  int byte_idx = 0;
  while (byte_idx < s.len) {
    byte_idx += utf8_char_bytes(s, byte_idx);
    count++;
  }
  return count;
}

/* Returns the byte offset where the code point at index cp_idx starts.
 * Returns s.len when cp_idx equals the number of code points in s, and -1 when
 * cp_idx is negative or beyond the end of the string. */
static int cp_to_byte_offset(scs_str s, int cp_idx) {
  if (cp_idx < 0) {
    return -1;
  }
  int byte_idx = 0;
  int cp = 0;
  while (cp < cp_idx) {
    if (byte_idx >= s.len) {
      return -1;
    }
    byte_idx += utf8_char_bytes(s, byte_idx);
    cp++;
  }
  return byte_idx;
}

/* returns the number of code points that start before byte_off */
static int byte_offset_to_cp(scs_str s, int byte_off) {
  int count = 0;
  int byte_idx = 0;
  while (byte_idx < byte_off && byte_idx < s.len) {
    byte_idx += utf8_char_bytes(s, byte_idx);
    count++;
  }
  return count;
}

/* returns the character at code point index n (as a one character string),
 * or an empty string when n is out of bounds */
static scs_str char_at(scs_str s, int n) {
  int byte_idx = cp_to_byte_offset(s, n);
  if (byte_idx < 0 || byte_idx >= s.len) {
    return EMPTY_STR;
  }
  scs_str result;
  result.data = s.data + byte_idx;
  result.len = utf8_char_bytes(s, byte_idx);
  return result;
}

/* Returns the substring of s beginning at start_idx (inclusive) and ending at
 * end_idx (exclusive), both in code points. Pass -1 as end_idx to mean "until
 * the end of the string". Returns an empty string when start_idx is out of
 * bounds or not before end_idx. */
static scs_str substr(scs_str s, int start_idx, int end_idx) {
  int len = str_len(s);
  if (start_idx < 0 || start_idx > len) {
    return EMPTY_STR;
  }
  if (end_idx < 0 || end_idx > len) {
    end_idx = len;
  }
  if (end_idx <= start_idx) {
    return EMPTY_STR;
  }

  int start_byte = cp_to_byte_offset(s, start_idx);
  int end_byte = start_byte;
  for (int cp = start_idx; cp < end_idx; cp++) {
    end_byte += utf8_char_bytes(s, end_byte);
  }

  scs_str result;
  result.data = s.data + start_byte;
  result.len = end_byte - start_byte;
  return result;
}

/* Allocates a buffer in the arena for a string of `len` bytes. The buffer has
 * one extra byte, already NUL, after the contents. */
static char *str_alloc_buf(scs_ctx *ctx, int len) {
  return scs_alloc(ctx, (size_t)len + 1);
}

/* throws if adding the two lengths would overflow an int */
static int checked_len_add(scs_ctx *ctx, int a, int b) {
  if (a > INT_MAX - b) {
    scs_throw(ctx, "string is too large");
  }
  return a + b;
}

/* copies a string into the arena */
static scs_str str_copy(scs_ctx *ctx, scs_str s) {
  char *buf = str_alloc_buf(ctx, s.len);
  copy_bytes(buf, s.data, s.len);
  scs_str result;
  result.data = buf;
  result.len = s.len;
  return result;
}

static scs_str str_concat(scs_ctx *ctx, scs_str s1, scs_str s2) {
  int len = checked_len_add(ctx, s1.len, s2.len);
  char *buf = str_alloc_buf(ctx, len);
  copy_bytes(buf, s1.data, s1.len);
  copy_bytes(buf + s1.len, s2.data, s2.len);
  scs_str result;
  result.data = buf;
  result.len = len;
  return result;
}

static scs_str str_concat3(scs_ctx *ctx, scs_str s1, scs_str s2, scs_str s3) {
  int len = checked_len_add(ctx, checked_len_add(ctx, s1.len, s2.len), s3.len);
  char *buf = str_alloc_buf(ctx, len);
  copy_bytes(buf, s1.data, s1.len);
  copy_bytes(buf + s1.len, s2.data, s2.len);
  copy_bytes(buf + s1.len + s2.len, s3.data, s3.len);
  scs_str result;
  result.data = buf;
  result.len = len;
  return result;
}

/* returns text repeated n times (an empty string when n is zero or negative) */
static scs_str repeat_string(scs_ctx *ctx, scs_str text, int n) {
  if (n <= 0 || text.len == 0) {
    return EMPTY_STR;
  }
  if (text.len > INT_MAX / n) {
    scs_throw(ctx, "string is too large");
  }
  int len = text.len * n;
  char *buf = str_alloc_buf(ctx, len);
  for (int i = 0; i < n; i++) {
    memcpy(buf + (i * text.len), text.data, (size_t)text.len);
  }
  scs_str result;
  result.data = buf;
  result.len = len;
  return result;
}

/* Returns the byte offset of the first occurrence of needle in s at or after
 * from_byte, or -1 if there is none. An empty needle matches at from_byte. */
static int str_find_bytes(scs_str s, scs_str needle, int from_byte) {
  if (from_byte < 0) {
    from_byte = 0;
  }
  if (needle.len == 0) {
    return from_byte <= s.len ? from_byte : s.len;
  }
  for (int i = from_byte; i + needle.len <= s.len; i++) {
    if (s.data[i] == needle.data[0] && memcmp(s.data + i, needle.data, (size_t)needle.len) == 0) {
      return i;
    }
  }
  return -1;
}

/* does needle exist inside of s? */
static bool str_includes(scs_str s, scs_str needle) {
  return str_find_bytes(s, needle, 0) != -1;
}

/* returns the code point index of the last occurrence of needle inside of s,
 * or -1 if needle is not found (an empty needle is found at the end of s) */
static int str_last_index_of(scs_str s, scs_str needle) {
  if (needle.len == 0) {
    return str_len(s);
  }
  for (int i = s.len - needle.len; i >= 0; i--) {
    if (s.data[i] == needle.data[0] && memcmp(s.data + i, needle.data, (size_t)needle.len) == 0) {
      return byte_offset_to_cp(s, i);
    }
  }
  return -1;
}

/* returns the code point index of the first occurrence of needle inside of s,
 * starting the search at code point index from_idx, or -1 if needle is not found */
static int str_index_of_from(scs_str s, scs_str needle, int from_idx) {
  int len = str_len(s);
  if (from_idx < 0) {
    from_idx = 0;
  }
  if (from_idx > len) {
    from_idx = len;
  }
  int from_byte = cp_to_byte_offset(s, from_idx);
  int found_byte = str_find_bytes(s, needle, from_byte);
  if (found_byte == -1) {
    return -1;
  }
  return byte_offset_to_cp(s, found_byte);
}

/* Returns s with the ASCII letters a-z converted to upper case. Other
 * characters are left alone. The only caller uses this to decide whether a
 * symbol starts with an upper case letter, which is a question about ASCII
 * identifiers in practice. */
static scs_str ascii_to_upper_case(scs_ctx *ctx, scs_str s) {
  char *buf = str_alloc_buf(ctx, s.len);
  for (int i = 0; i < s.len; i++) {
    char c = s.data[i];
    if (c >= 'a' && c <= 'z') {
      c = (char)(c - ('a' - 'A'));
    }
    buf[i] = c;
  }
  scs_str result;
  result.data = buf;
  result.len = s.len;
  return result;
}

/* Is code_point whitespace? Uses the same definition of whitespace as
 * JavaScript's String.prototype.trim(): the Unicode "Space Separator" category
 * plus tab, line feed, vertical tab, form feed, carriage return, line and
 * paragraph separators, and the byte order mark. */
static bool is_trim_whitespace(int code_point) {
  switch (code_point) {
  case 0x0009: /* tab */
  case 0x000A: /* line feed */
  case 0x000B: /* vertical tab */
  case 0x000C: /* form feed */
  case 0x000D: /* carriage return */
  case 0x0020: /* space */
  case 0x00A0: /* no-break space */
  case 0x1680: /* ogham space mark */
  case 0x2028: /* line separator */
  case 0x2029: /* paragraph separator */
  case 0x202F: /* narrow no-break space */
  case 0x205F: /* medium mathematical space */
  case 0x3000: /* ideographic space */
  case 0xFEFF: /* byte order mark */
    return true;
  default:
    return (code_point >= 0x2000 && code_point <= 0x200A); /* en quad ... hair space */
  }
}

/* removes trailing whitespace */
static scs_str rtrim(scs_str s) {
  int end = 0; /* one past the last non-whitespace byte */
  int byte_idx = 0;
  while (byte_idx < s.len) {
    int num_bytes = 0;
    int code_point = utf8_decode(s, byte_idx, &num_bytes);
    if (!is_trim_whitespace(code_point)) {
      end = byte_idx + num_bytes;
    }
    byte_idx += num_bytes;
  }
  scs_str result;
  result.data = s.data;
  result.len = end;
  return result;
}

/* removes leading and trailing whitespace */
static scs_str str_trim(scs_str s) {
  int byte_idx = 0;
  while (byte_idx < s.len) {
    int num_bytes = 0;
    int code_point = utf8_decode(s, byte_idx, &num_bytes);
    if (!is_trim_whitespace(code_point)) {
      break;
    }
    byte_idx += num_bytes;
  }
  scs_str rest;
  rest.data = s.data + byte_idx;
  rest.len = s.len - byte_idx;
  return rtrim(rest);
}

/* does s contain any non-whitespace characters?
 * equivalent to str_trim(s).len > 0 but stops at the first such character */
static bool str_has_non_whitespace_chars(scs_str s) {
  int byte_idx = 0;
  while (byte_idx < s.len) {
    int num_bytes = 0;
    int code_point = utf8_decode(s, byte_idx, &num_bytes);
    if (!is_trim_whitespace(code_point)) {
      return true;
    }
    byte_idx += num_bytes;
  }
  return false;
}

static bool str_starts_with(scs_str s, scs_str prefix) {
  return prefix.len <= s.len && bytes_eq(s.data, prefix.data, prefix.len);
}

static bool str_ends_with(scs_str s, scs_str suffix) {
  return suffix.len <= s.len && bytes_eq(s.data + (s.len - suffix.len), suffix.data, suffix.len);
}

/* replaces the first occurrence of find inside of s with replace
 * (returns s unchanged when s is empty, find is empty, or find is not found) */
static scs_str str_replace_first(scs_ctx *ctx, scs_str s, scs_str find, scs_str replace) {
  if (s.len == 0 || find.len == 0) {
    return s;
  }
  int found_byte = str_find_bytes(s, find, 0);
  if (found_byte == -1) {
    return s;
  }
  scs_str head;
  head.data = s.data;
  head.len = found_byte;
  scs_str tail;
  tail.data = s.data + found_byte + find.len;
  tail.len = s.len - found_byte - find.len;
  return str_concat3(ctx, head, replace, tail);
}

/* replaces every occurrence of find inside of s with replace, scanning left to
 * right without overlap (returns s unchanged when find is empty or not found) */
static scs_str str_replace_all(scs_ctx *ctx, scs_str s, scs_str find, scs_str replace) {
  if (find.len == 0) {
    return s;
  }

  int num_found = 0;
  int search_from = 0;
  while (true) {
    int found_byte = str_find_bytes(s, find, search_from);
    if (found_byte == -1) {
      break;
    }
    num_found++;
    search_from = found_byte + find.len;
  }
  if (num_found == 0) {
    return s;
  }

  /* new length = s.len + num_found * (replace.len - find.len), checked for overflow */
  int len = s.len;
  for (int i = 0; i < num_found; i++) {
    len = checked_len_add(ctx, len, replace.len) - find.len;
  }

  char *buf = str_alloc_buf(ctx, len);
  int out = 0;
  search_from = 0;
  while (true) {
    int found_byte = str_find_bytes(s, find, search_from);
    if (found_byte == -1) {
      break;
    }
    copy_bytes(buf + out, s.data + search_from, found_byte - search_from);
    out += found_byte - search_from;
    copy_bytes(buf + out, replace.data, replace.len);
    out += replace.len;
    search_from = found_byte + find.len;
  }
  copy_bytes(buf + out, s.data + search_from, s.len - search_from);

  scs_str result;
  result.data = buf;
  result.len = len;
  return result;
}

/* converts CRLF line endings to LF (returns s unchanged when there are none) */
static scs_str crlf_to_lf(scs_ctx *ctx, scs_str txt) {
  return str_replace_all(ctx, txt, STR("\r\n"), STR("\n"));
}

/* -----------------------------------------------------------------------------
 * Growable Arrays
 *
 * Every list type in this file has the same three fields: items, size, and
 * capacity. list_grow() makes room for one more item and LIST_PUSH() appends
 * one. Lists live in the arena: when a list outgrows its storage the items are
 * copied to a larger allocation and the old one is simply abandoned.
 */

/* returns storage with room for at least size + 1 items (the same storage if
 * it already has room, otherwise a larger copy) */
static void *list_grow(scs_ctx *ctx, void *items, int size, int *capacity, size_t item_size) {
  if (size < *capacity) {
    return items;
  }
  int new_capacity = *capacity == 0 ? 8 : checked_len_add(ctx, *capacity, *capacity);
  void *new_items = scs_alloc(ctx, (size_t)new_capacity * item_size);
  if (size > 0) {
    memcpy(new_items, items, (size_t)size * item_size);
  }
  *capacity = new_capacity;
  return new_items;
}

#define LIST_PUSH(ctx, list, item)                                                                 \
  do {                                                                                             \
    (list)->items =                                                                                \
        list_grow((ctx), (list)->items, (list)->size, &(list)->capacity, sizeof(*(list)->items));  \
    (list)->items[(list)->size] = (item);                                                          \
    (list)->size++;                                                                                \
  } while (0)

/* -----------------------------------------------------------------------------
 * String Builder
 *
 * For building a string piece by piece. Appending is amortized constant time;
 * the finished contents are read with strbuf_to_str().
 */

typedef struct scs_strbuf {
  char *data; /* NULL until the first append */
  int len;
  int capacity;
} scs_strbuf;

static void strbuf_append(scs_ctx *ctx, scs_strbuf *buf, scs_str s) {
  int needed = checked_len_add(ctx, buf->len, s.len);
  if (needed > buf->capacity) {
    int new_capacity = buf->capacity == 0 ? 64 : buf->capacity;
    while (new_capacity < needed) {
      new_capacity = checked_len_add(ctx, new_capacity, new_capacity);
    }
    char *new_data = str_alloc_buf(ctx, new_capacity);
    copy_bytes(new_data, buf->data, buf->len);
    buf->data = new_data;
    buf->capacity = new_capacity;
  }
  copy_bytes(buf->data + buf->len, s.data, s.len);
  buf->len = needed;
}

/* the contents so far, as a string that stays valid until the next append */
static scs_str strbuf_to_str(const scs_strbuf *buf) {
  scs_str s;
  s.data = buf->data ? buf->data : "";
  s.len = buf->len;
  return s;
}

/* -----------------------------------------------------------------------------
 * String Lists
 */

/* a growable array of strings */
typedef struct scs_str_list {
  scs_str *items; /* NULL until the first item is added */
  int size;
  int capacity;
} scs_str_list;

static void str_list_push(scs_ctx *ctx, scs_str_list *list, scs_str s) {
  LIST_PUSH(ctx, list, s);
}

/* returns the last string in the list, or an empty string if the list is empty */
static scs_str str_list_last(const scs_str_list *list) {
  if (list->size == 0) {
    return EMPTY_STR;
  }
  return list->items[list->size - 1];
}

/* returns a new list containing every item except the last one */
static scs_str_list str_list_drop_last(scs_ctx *ctx, const scs_str_list *list) {
  scs_str_list result = {NULL, 0, 0};
  for (int i = 0; i < list->size - 1; i++) {
    str_list_push(ctx, &result, list->items[i]);
  }
  return result;
}

/* joins the strings in a list with sep between each pair */
static scs_str str_join(scs_ctx *ctx, const scs_str_list *list, scs_str sep) {
  if (list->size == 0) {
    return EMPTY_STR;
  }
  int len = 0;
  for (int i = 0; i < list->size; i++) {
    len = checked_len_add(ctx, len, list->items[i].len);
    if (i > 0) {
      len = checked_len_add(ctx, len, sep.len);
    }
  }

  char *buf = str_alloc_buf(ctx, len);
  int out = 0;
  for (int i = 0; i < list->size; i++) {
    if (i > 0) {
      copy_bytes(buf + out, sep.data, sep.len);
      out += sep.len;
    }
    copy_bytes(buf + out, list->items[i].data, list->items[i].len);
    out += list->items[i].len;
  }

  scs_str result;
  result.data = buf;
  result.len = len;
  return result;
}

/* splits s on every occurrence of sep (which must not be empty); consecutive
 * separators produce empty strings, and the result always has at least one item */
static scs_str_list str_split(scs_ctx *ctx, scs_str s, scs_str sep) {
  scs_str_list result = {NULL, 0, 0};
  int piece_start = 0;
  while (true) {
    int found_byte = str_find_bytes(s, sep, piece_start);
    scs_str piece;
    piece.data = s.data + piece_start;
    if (found_byte == -1) {
      piece.len = s.len - piece_start;
      str_list_push(ctx, &result, piece);
      return result;
    }
    piece.len = found_byte - piece_start;
    str_list_push(ctx, &result, piece);
    piece_start = found_byte + sep.len;
  }
}

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
 * Node Lists and Stack Operations
 *
 * A node list doubles as a stack: push and pop operate on its end.
 */

static void node_list_push(scs_ctx *ctx, scs_node_list *list, scs_node *node) {
  LIST_PUSH(ctx, list, node);
}

/* removes and returns the last node, or NULL if the list is empty */
static scs_node *node_list_pop(scs_node_list *list) {
  if (list->size == 0) {
    return NULL;
  }
  list->size--;
  return list->items[list->size];
}

/* returns the node idx_from_back positions from the end (0 is the last node),
 * or NULL if there is no such node */
static scs_node *node_list_peek(const scs_node_list *list, int idx_from_back) {
  int max_idx = list->size - 1;
  if (idx_from_back < 0 || idx_from_back > max_idx) {
    return NULL;
  }
  return list->items[max_idx - idx_from_back];
}

/* returns the last node, or NULL if the list is empty */
static scs_node *node_list_last(const scs_node_list *list) {
  return node_list_peek(list, 0);
}

/* -----------------------------------------------------------------------------
 * Node Construction
 */

/* returns a new unique id (unique within one context) */
static int create_id(scs_ctx *ctx) {
  ctx->id_counter = ctx->id_counter + 1;
  return ctx->id_counter;
}

static scs_node *node_new(scs_ctx *ctx, const char *name, int start_idx, int end_idx,
                          scs_str text) {
  scs_node *node = scs_alloc(ctx, sizeof(scs_node));
  node->id = create_id(ctx);
  node->name = name;
  node->text = text;
  node->start_idx = start_idx;
  node->end_idx = end_idx;
  node->orig_col_idx = -1;
  node->printed_col_idx = -1;
  node->printed_line_idx = -1;
  node->was_slurped_up = false;
  return node;
}

/* -----------------------------------------------------------------------------
 * Parsers
 *
 * The parser is a small parser-combinator library. A parser is a struct with a
 * parse function and whatever configuration that function needs. Terminal
 * parsers match text directly; sequence parsers combine other parsers. Each
 * parse function is given the full input and a position and returns a node (or
 * NULL when it does not match). The grammar further below is built from static
 * const instances of these structs, so it is immutable and shared by all calls.
 *
 * While parsing, positions and node indexes are BYTE offsets into the input,
 * which keeps matching simple and fast. parse() converts every node's indexes
 * to code points once the whole tree has been built.
 */

typedef struct scs_parser scs_parser;

typedef scs_node *(*scs_parse_fn)(scs_ctx *ctx, const scs_parser *parser, scs_str txt, int pos);

/* A text matcher: returns the number of bytes matched at pos, or -1 for no
 * match. Used by pattern parsers in place of regular expressions. */
typedef int (*scs_match_fn)(scs_str txt, int pos);

struct scs_parser {
  scs_parse_fn parse;
  const char *name; /* the name given to matched nodes; NULL for an unnamed node */

  /* terminal parsers */
  char ch;            /* Char, NotChar */
  scs_str str;        /* String */
  scs_match_fn match; /* Pattern */

  /* sequence parsers */
  const scs_parser *const *parsers; /* Seq, Choice */
  int num_parsers;
  const scs_parser *parser; /* Repeat, Optional, Named */
  int min_matches;          /* Repeat */
};

#define NUM_PARSERS(arr) ((int)(sizeof(arr) / sizeof((arr)[0])))

/* a view of the input from byte offset start (inclusive) to end (exclusive) */
static scs_str str_slice(scs_str s, int start, int end) {
  scs_str result;
  result.data = s.data + start;
  result.len = end - start;
  return result;
}

/* -----------------------------------------------------------------------------
 * Terminal Parsers
 */

/* matches any single character */
static scs_node *parse_any_char(scs_ctx *ctx, const scs_parser *p, scs_str txt, int pos) {
  if (pos < 0 || pos >= txt.len) {
    return NULL;
  }
  int end = pos + utf8_char_bytes(txt, pos);
  return node_new(ctx, p->name, pos, end, str_slice(txt, pos, end));
}

/* matches one specific character */
static scs_node *parse_char(scs_ctx *ctx, const scs_parser *p, scs_str txt, int pos) {
  if (pos < 0 || pos >= txt.len || txt.data[pos] != p->ch) {
    return NULL;
  }
  return node_new(ctx, p->name, pos, pos + 1, str_slice(txt, pos, pos + 1));
}

/* matches any single character except one */
static scs_node *parse_not_char(scs_ctx *ctx, const scs_parser *p, scs_str txt, int pos) {
  if (pos < 0 || pos >= txt.len || txt.data[pos] == p->ch) {
    return NULL;
  }
  int end = pos + utf8_char_bytes(txt, pos);
  return node_new(ctx, p->name, pos, end, str_slice(txt, pos, end));
}

/* matches a specific string */
static scs_node *parse_string(scs_ctx *ctx, const scs_parser *p, scs_str txt, int pos) {
  if (pos < 0 || pos + p->str.len > txt.len || !bytes_eq(txt.data + pos, p->str.data, p->str.len)) {
    return NULL;
  }
  int end = pos + p->str.len;
  return node_new(ctx, p->name, pos, end, str_slice(txt, pos, end));
}

/* matches whatever the parser's match function matches */
static scs_node *parse_pattern(scs_ctx *ctx, const scs_parser *p, scs_str txt, int pos) {
  if (pos < 0 || pos > txt.len) {
    return NULL;
  }
  int num_bytes = p->match(txt, pos);
  if (num_bytes < 0) {
    return NULL;
  }
  int end = pos + num_bytes;
  return node_new(ctx, p->name, pos, end, str_slice(txt, pos, end));
}

/* -----------------------------------------------------------------------------
 * Sequence Parsers
 */

/* Adds node to the list of children. A named node is added as-is; an unnamed
 * node is flattened, so its children are added instead. */
static void append_children(scs_ctx *ctx, scs_node_list *children, scs_node *node) {
  if (node->name != NULL) {
    node_list_push(ctx, children, node);
  } else {
    for (int i = 0; i < node->children.size; i++) {
      append_children(ctx, children, node->children.items[i]);
    }
  }
}

/* matches a linear sequence of parsers */
static scs_node *parse_seq(scs_ctx *ctx, const scs_parser *p, scs_str txt, int pos) {
  scs_node_list children = {NULL, 0, 0};
  int end_idx = pos;

  for (int j = 0; j < p->num_parsers; j++) {
    const scs_parser *parser = p->parsers[j];
    scs_node *possible_node = parser->parse(ctx, parser, txt, end_idx);
    if (!possible_node) {
      return NULL;
    }
    append_children(ctx, &children, possible_node);
    end_idx = possible_node->end_idx;
  }

  scs_node *node = node_new(ctx, p->name, pos, end_idx, EMPTY_STR);
  node->children = children;
  return node;
}

/* matches the first of several parsers that matches */
static scs_node *parse_choice(scs_ctx *ctx, const scs_parser *p, scs_str txt, int pos) {
  for (int i = 0; i < p->num_parsers; i++) {
    const scs_parser *parser = p->parsers[i];
    scs_node *possible_node = parser->parse(ctx, parser, txt, pos);
    if (possible_node) {
      return possible_node;
    }
  }
  return NULL;
}

/* Matches the child parser zero or more times (at least min_matches times).
 * The node is named only when something was matched. */
static scs_node *parse_repeat(scs_ctx *ctx, const scs_parser *p, scs_str txt, int pos) {
  scs_node_list children = {NULL, 0, 0};
  int end_idx = pos;

  while (true) {
    scs_node *node = p->parser->parse(ctx, p->parser, txt, end_idx);
    if (!node) {
      break;
    }
    append_children(ctx, &children, node);
    end_idx = node->end_idx;
  }

  if (children.size < p->min_matches) {
    return NULL;
  }

  const char *name = end_idx > pos ? p->name : NULL;
  scs_node *node = node_new(ctx, name, pos, end_idx, EMPTY_STR);
  node->children = children;
  return node;
}

/* matches the child parser, or matches nothing (an empty unnamed node) */
static scs_node *parse_optional(scs_ctx *ctx, const scs_parser *p, scs_str txt, int pos) {
  scs_node *node = p->parser->parse(ctx, p->parser, txt, pos);
  if (node && node->text.len > 0) {
    return node;
  }
  return node_new(ctx, NULL, pos, pos, EMPTY_STR);
}

/* Gives a name to whatever the child parser matches. An unnamed node is
 * renamed in place; a named node is wrapped in a new node. */
static scs_node *parse_named(scs_ctx *ctx, const scs_parser *p, scs_str txt, int pos) {
  scs_node *node = p->parser->parse(ctx, p->parser, txt, pos);
  if (!node) {
    return NULL;
  }
  if (node->name == NULL) {
    node->name = p->name;
    return node;
  }
  scs_node *wrapper = node_new(ctx, p->name, node->start_idx, node->end_idx, EMPTY_STR);
  node_list_push(ctx, &wrapper->children, node);
  return wrapper;
}

/* -----------------------------------------------------------------------------
 * Text Matchers
 *
 * The small patterns the grammar needs, written by hand. Each returns the
 * number of bytes matched at pos, or -1 for no match.
 */

/* the characters that separate tokens: space, comma, the ASCII control
 * characters used as whitespace, and the Unicode space separators */
static bool is_token_whitespace(int code_point) {
  switch (code_point) {
  case ' ':
  case ',':
  case '\n':
  case '\r':
  case '\t':
  case '\f':
  case 0x000B: /* vertical tab */
  case 0x001C: /* file separator */
  case 0x001D: /* group separator */
  case 0x001E: /* record separator */
  case 0x001F: /* unit separator */
  case 0x2028: /* line separator */
  case 0x2029: /* paragraph separator */
  case 0x1680: /* ogham space mark */
  case 0x2008: /* punctuation space */
  case 0x2009: /* thin space */
  case 0x200A: /* hair space */
  case 0x205F: /* medium mathematical space */
  case 0x3000: /* ideographic space */
    return true;
  default:
    return (code_point >= 0x2000 && code_point <= 0x2006); /* en quad ... six-per-em space */
  }
}

/* characters that cannot start a token */
static bool is_token_head_char(int code_point) {
  switch (code_point) {
  case '(':
  case ')':
  case '[':
  case ']':
  case '{':
  case '}':
  case '"':
  case '@':
  case '~':
  case '^':
  case ';':
  case '`':
  case '#':
  case '\'':
    return true;
  default:
    return false;
  }
}

/* characters that end a token */
static bool is_token_tail_char(int code_point) {
  switch (code_point) {
  case '(':
  case ')':
  case '[':
  case ']':
  case '{':
  case '}':
  case '"':
  case '@':
  case '^':
  case ';':
  case '`':
    return true;
  default:
    return false;
  }
}

/* a backslash followed by one of these is a character literal like \( or \space's first char */
static bool is_char_literal_char(char c) {
  switch (c) {
  case '(':
  case ')':
  case '[':
  case ']':
  case '{':
  case '}':
  case '"':
  case '@':
  case '^':
  case ';':
  case '`':
  case ',':
  case ' ':
    return true;
  default:
    return false;
  }
}

/* a line terminator for the purposes of a string body escape */
static bool is_line_terminator(int code_point) {
  return code_point == '\n' || code_point == '\r' || code_point == 0x2028 || code_point == 0x2029;
}

static bool is_ascii_letter(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* a token: either a character literal (a backslash followed by a delimiter), or
 * a run of characters that does not start with a head character and stops at a
 * tail character or whitespace; either may be prefixed with ## (as in ##Inf) */
static int match_token_body(scs_str txt, int pos) {
  if (pos >= txt.len) {
    return -1;
  }
  if (txt.data[pos] == '\\' && pos + 1 < txt.len && is_char_literal_char(txt.data[pos + 1])) {
    return 2;
  }

  int num_bytes = 0;
  int code_point = utf8_decode(txt, pos, &num_bytes);
  if (is_token_head_char(code_point) || is_token_whitespace(code_point)) {
    return -1;
  }
  int idx = pos + num_bytes;
  while (idx < txt.len) {
    code_point = utf8_decode(txt, idx, &num_bytes);
    if (is_token_tail_char(code_point) || is_token_whitespace(code_point)) {
      break;
    }
    idx += num_bytes;
  }
  return idx - pos;
}

static int match_token(scs_str txt, int pos) {
  if (pos + 2 <= txt.len && txt.data[pos] == '#' && txt.data[pos + 1] == '#') {
    int body = match_token_body(txt, pos + 2);
    if (body > 0) {
      return 2 + body;
    }
  }
  return match_token_body(txt, pos);
}

/* one or more whitespace characters */
static int match_whitespace(scs_str txt, int pos) {
  int idx = pos;
  while (idx < txt.len) {
    int num_bytes = 0;
    int code_point = utf8_decode(txt, idx, &num_bytes);
    if (!is_token_whitespace(code_point)) {
      break;
    }
    idx += num_bytes;
  }
  return idx > pos ? idx - pos : -1;
}

/* a semicolon and everything up to (not including) the end of the line */
static int match_comment(scs_str txt, int pos) {
  if (pos >= txt.len || txt.data[pos] != ';') {
    return -1;
  }
  int idx = pos + 1;
  while (idx < txt.len && txt.data[idx] != '\n') {
    idx++;
  }
  return idx - pos;
}

/* the opening of a string: a double quote, optionally preceded by # (a regex) */
static int match_string_open(scs_str txt, int pos) {
  if (pos < txt.len && txt.data[pos] == '"') {
    return 1;
  }
  if (pos + 1 < txt.len && txt.data[pos] == '#' && txt.data[pos + 1] == '"') {
    return 2;
  }
  return -1;
}

/* the body of a string: runs of ordinary characters and backslash escapes
 * (a backslash may escape any character except a line terminator) */
static int match_string_body(scs_str txt, int pos) {
  int idx = pos;
  while (idx < txt.len) {
    char c = txt.data[idx];
    if (c == '"') {
      break;
    }
    if (c == '\\') {
      if (idx + 1 >= txt.len) {
        break;
      }
      int num_bytes = 0;
      int escaped = utf8_decode(txt, idx + 1, &num_bytes);
      if (is_line_terminator(escaped)) {
        break;
      }
      idx += 1 + num_bytes;
    } else {
      idx++;
    }
  }
  return idx > pos ? idx - pos : -1;
}

/* The opening of a namespaced map with an alias, such as #:alias{ or #::alias{.
 * The alias must start with a letter and may continue with lower case letters,
 * or any ASCII character from '.' through '_' (digits, upper case letters, and
 * punctuation such as ':' and '/'). Note that '-' is not in that range. */
static int match_namespaced_map_open(scs_str txt, int pos) {
  int idx = pos;
  if (idx + 1 >= txt.len || txt.data[idx] != '#' || txt.data[idx + 1] != ':') {
    return -1;
  }
  idx += 2;
  if (idx < txt.len && txt.data[idx] == ':') {
    idx++;
  }
  if (idx >= txt.len || !is_ascii_letter(txt.data[idx])) {
    return -1;
  }
  idx++;
  while (idx < txt.len) {
    char c = txt.data[idx];
    bool is_alias_char = (c >= 'a' && c <= 'z') || (c >= '.' && c <= '_');
    if (!is_alias_char) {
      break;
    }
    idx++;
  }
  if (idx >= txt.len || txt.data[idx] != '{') {
    return -1;
  }
  return idx + 1 - pos;
}

/* returns the length of prefix if txt has it at pos, otherwise -1 */
static int match_prefix(scs_str txt, int pos, const char *prefix) {
  int len = (int)strlen(prefix);
  if (pos + len <= txt.len && memcmp(txt.data + pos, prefix, (size_t)len) == 0) {
    return len;
  }
  return -1;
}

/* the opening of a list, anonymous function, reader conditional, or eval form:
 * ( #( #?( #?@( #=( */
static int match_parens_open(scs_str txt, int pos) {
  static const char *const openers[] = {"#?@(", "#?(", "#=(", "#(", "("};
  for (size_t i = 0; i < sizeof(openers) / sizeof(openers[0]); i++) {
    int len = match_prefix(txt, pos, openers[i]);
    if (len > 0) {
      return len;
    }
  }
  return -1;
}

/* a metadata marker: ^ or #^ */
static int match_meta_marker(scs_str txt, int pos) {
  int len = match_prefix(txt, pos, "#^");
  if (len > 0) {
    return len;
  }
  return match_prefix(txt, pos, "^");
}

/* a reader macro marker that wraps the following form: @ ' ` ~@ ~ #' */
static int match_wrap_marker(scs_str txt, int pos) {
  static const char *const markers[] = {"@", "'", "`", "~@", "~", "#'"};
  for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); i++) {
    int len = match_prefix(txt, pos, markers[i]);
    if (len > 0) {
      return len;
    }
  }
  return -1;
}

/* -----------------------------------------------------------------------------
 * Grammar
 *
 * The grammar is recursive (a list contains forms, and a form may be a list),
 * so a few parsers are declared before they are defined. Every parser is a
 * static const struct built with designated initializers: the fields that are
 * not mentioned are zero / NULL.
 */

/* Collections and reader macros may nest this many levels deep before parsing
 * stops with an error. This guards against a stack overflow on pathological
 * input: each level of nesting uses roughly 1 KB of C stack, and the default
 * thread stack is 1 MB on Windows and 512 KB for secondary threads on macOS.
 * Real code nests a few dozen levels at most. Override with
 * -DSCS_MAX_NESTING_DEPTH=N when building if your environment needs a
 * different limit. */
#ifndef SCS_MAX_NESTING_DEPTH
#define SCS_MAX_NESTING_DEPTH 500
#endif

static const scs_parser p_gap;
static const scs_parser p_form;

/* string */
static const scs_parser p_string_open = {
    .parse = parse_pattern, .name = ".open", .match = match_string_open};
static const scs_parser p_string_body_text = {
    .parse = parse_pattern, .name = ".body", .match = match_string_body};
static const scs_parser p_string_body = {
    .parse = parse_optional, .name = NULL, .parser = &p_string_body_text};
static const scs_parser p_string_close_quote = {.parse = parse_char, .name = ".close", .ch = '"'};
static const scs_parser p_string_close = {
    .parse = parse_optional, .name = NULL, .parser = &p_string_close_quote};
static const scs_parser *const string_parsers[] = {&p_string_open, &p_string_body, &p_string_close};
static const scs_parser p_string = {.parse = parse_seq,
                                    .name = "string",
                                    .parsers = string_parsers,
                                    .num_parsers = NUM_PARSERS(string_parsers)};

/* token, whitespace, comment */
static const scs_parser p_token = {.parse = parse_pattern, .name = "token", .match = match_token};
static const scs_parser p_whitespace = {
    .parse = parse_pattern, .name = "whitespace", .match = match_whitespace};
static const scs_parser p_comment = {
    .parse = parse_pattern, .name = "comment", .match = match_comment};

/* zero or more gaps (whitespace, comments, discarded forms) */
static const scs_parser p_gaps = {.parse = parse_repeat, .name = NULL, .parser = &p_gap};

/* discard: #_ form */
static const scs_parser p_discard_marker = {
    .parse = parse_string, .name = "marker", .str = {"#_", 2}};
static const scs_parser p_discard_body = {.parse = parse_named, .name = ".body", .parser = &p_form};
static const scs_parser *const discard_parsers[] = {&p_discard_marker, &p_gaps, &p_discard_body};
static const scs_parser p_discard = {.parse = parse_seq,
                                     .name = "discard",
                                     .parsers = discard_parsers,
                                     .num_parsers = NUM_PARSERS(discard_parsers)};

/* braces: maps, sets, and namespaced maps */
static const scs_parser p_braces_open_map = {.parse = parse_char, .name = ".open", .ch = '{'};
static const scs_parser p_braces_open_set = {
    .parse = parse_string, .name = ".open", .str = {"#{", 2}};
static const scs_parser p_braces_open_auto_ns = {
    .parse = parse_string, .name = ".open", .str = {"#::{", 4}};
static const scs_parser p_braces_open_ns = {
    .parse = parse_pattern, .name = ".open", .match = match_namespaced_map_open};
static const scs_parser *const braces_open_parsers[] = {&p_braces_open_map, &p_braces_open_set,
                                                        &p_braces_open_auto_ns, &p_braces_open_ns};
static const scs_parser p_braces_open = {.parse = parse_choice,
                                         .name = NULL,
                                         .parsers = braces_open_parsers,
                                         .num_parsers = NUM_PARSERS(braces_open_parsers)};
static const scs_parser p_braces_error = {.parse = parse_not_char, .name = "error", .ch = '}'};
static const scs_parser *const braces_body_parsers[] = {&p_gap, &p_form, &p_braces_error};
static const scs_parser p_braces_body_item = {.parse = parse_choice,
                                              .name = NULL,
                                              .parsers = braces_body_parsers,
                                              .num_parsers = NUM_PARSERS(braces_body_parsers)};
static const scs_parser p_braces_body = {
    .parse = parse_repeat, .name = ".body", .parser = &p_braces_body_item};
static const scs_parser p_braces_close_brace = {.parse = parse_char, .name = ".close", .ch = '}'};
static const scs_parser p_braces_close = {
    .parse = parse_optional, .name = NULL, .parser = &p_braces_close_brace};
static const scs_parser *const braces_parsers[] = {&p_braces_open, &p_braces_body, &p_braces_close};
static const scs_parser p_braces = {.parse = parse_seq,
                                    .name = "braces",
                                    .parsers = braces_parsers,
                                    .num_parsers = NUM_PARSERS(braces_parsers)};

/* brackets: vectors */
static const scs_parser p_brackets_open = {.parse = parse_char, .name = ".open", .ch = '['};
static const scs_parser p_brackets_error = {.parse = parse_not_char, .name = "error", .ch = ']'};
static const scs_parser *const brackets_body_parsers[] = {&p_gap, &p_form, &p_brackets_error};
static const scs_parser p_brackets_body_item = {.parse = parse_choice,
                                                .name = NULL,
                                                .parsers = brackets_body_parsers,
                                                .num_parsers = NUM_PARSERS(brackets_body_parsers)};
static const scs_parser p_brackets_body = {
    .parse = parse_repeat, .name = ".body", .parser = &p_brackets_body_item};
static const scs_parser p_brackets_close_bracket = {
    .parse = parse_char, .name = ".close", .ch = ']'};
static const scs_parser p_brackets_close = {
    .parse = parse_optional, .name = NULL, .parser = &p_brackets_close_bracket};
static const scs_parser *const brackets_parsers[] = {&p_brackets_open, &p_brackets_body,
                                                     &p_brackets_close};
static const scs_parser p_brackets = {.parse = parse_seq,
                                      .name = "brackets",
                                      .parsers = brackets_parsers,
                                      .num_parsers = NUM_PARSERS(brackets_parsers)};

/* parens: lists, anonymous functions, reader conditionals */
static const scs_parser p_parens_open = {
    .parse = parse_pattern, .name = ".open", .match = match_parens_open};
static const scs_parser p_parens_error = {.parse = parse_not_char, .name = "error", .ch = ')'};
static const scs_parser *const parens_body_parsers[] = {&p_gap, &p_form, &p_parens_error};
static const scs_parser p_parens_body_item = {.parse = parse_choice,
                                              .name = NULL,
                                              .parsers = parens_body_parsers,
                                              .num_parsers = NUM_PARSERS(parens_body_parsers)};
static const scs_parser p_parens_body = {
    .parse = parse_repeat, .name = ".body", .parser = &p_parens_body_item};
static const scs_parser p_parens_close_paren = {.parse = parse_char, .name = ".close", .ch = ')'};
static const scs_parser p_parens_close = {
    .parse = parse_optional, .name = NULL, .parser = &p_parens_close_paren};
static const scs_parser *const parens_parsers[] = {&p_parens_open, &p_parens_body, &p_parens_close};
static const scs_parser p_parens = {.parse = parse_seq,
                                    .name = "parens",
                                    .parsers = parens_parsers,
                                    .num_parsers = NUM_PARSERS(parens_parsers)};

/* meta: one or more ^metadata markers followed by the form they annotate */
static const scs_parser p_meta_marker = {
    .parse = parse_pattern, .name = ".marker", .match = match_meta_marker};
static const scs_parser p_meta_data = {.parse = parse_named, .name = ".meta", .parser = &p_form};
static const scs_parser *const meta_item_parsers[] = {&p_meta_marker, &p_gaps, &p_meta_data,
                                                      &p_gaps};
static const scs_parser p_meta_item = {.parse = parse_seq,
                                       .name = NULL,
                                       .parsers = meta_item_parsers,
                                       .num_parsers = NUM_PARSERS(meta_item_parsers)};
static const scs_parser p_meta_items = {
    .parse = parse_repeat, .name = NULL, .parser = &p_meta_item, .min_matches = 1};
static const scs_parser p_meta_body = {.parse = parse_named, .name = ".body", .parser = &p_form};
static const scs_parser *const meta_parsers[] = {&p_meta_items, &p_meta_body};
static const scs_parser p_meta = {.parse = parse_seq,
                                  .name = "meta",
                                  .parsers = meta_parsers,
                                  .num_parsers = NUM_PARSERS(meta_parsers)};

/* wrap: a reader macro marker followed by the form it applies to */
static const scs_parser p_wrap_marker = {
    .parse = parse_pattern, .name = ".marker", .match = match_wrap_marker};
static const scs_parser p_wrap_body = {.parse = parse_named, .name = ".body", .parser = &p_form};
static const scs_parser *const wrap_parsers[] = {&p_wrap_marker, &p_gaps, &p_wrap_body};
static const scs_parser p_wrap = {.parse = parse_seq,
                                  .name = "wrap",
                                  .parsers = wrap_parsers,
                                  .num_parsers = NUM_PARSERS(wrap_parsers)};

/* tagged: #tag form */
static const scs_parser p_tagged_hash = {.parse = parse_char, .name = NULL, .ch = '#'};
static const scs_parser p_tagged_tag = {.parse = parse_named, .name = ".tag", .parser = &p_token};
static const scs_parser p_tagged_body = {.parse = parse_named, .name = ".body", .parser = &p_form};
static const scs_parser *const tagged_parsers[] = {&p_tagged_hash, &p_gaps, &p_tagged_tag, &p_gaps,
                                                   &p_tagged_body};
static const scs_parser p_tagged = {.parse = parse_seq,
                                    .name = "tagged",
                                    .parsers = tagged_parsers,
                                    .num_parsers = NUM_PARSERS(tagged_parsers)};

/* A gap is whitespace, a comment, or a discarded form. The three cannot start
 * with the same character, so we dispatch on the first character. */
static scs_node *parse_gap(scs_ctx *ctx, const scs_parser *p, scs_str txt, int pos) {
  (void)p;
  if (pos < 0 || pos >= txt.len) {
    return NULL;
  }
  int num_bytes = 0;
  int code_point = utf8_decode(txt, pos, &num_bytes);
  if (is_token_whitespace(code_point)) {
    return p_whitespace.parse(ctx, &p_whitespace, txt, pos);
  }
  if (code_point == ';') {
    return p_comment.parse(ctx, &p_comment, txt, pos);
  }
  if (code_point == '#') {
    return p_discard.parse(ctx, &p_discard, txt, pos);
  }
  return NULL;
}

static const scs_parser p_gap = {.parse = parse_gap, .name = NULL};

/* the forms that can start with # (regex string, set, namespaced map, anonymous
 * function, reader conditional, var quote, metadata, ##Inf, tagged literal) */
static const scs_parser *const hash_form_parsers[] = {&p_token, &p_string, &p_parens, &p_braces,
                                                      &p_wrap,  &p_meta,   &p_tagged};
static const scs_parser p_hash_form = {.parse = parse_choice,
                                       .name = NULL,
                                       .parsers = hash_form_parsers,
                                       .num_parsers = NUM_PARSERS(hash_form_parsers)};

/* parses a form that may contain other forms, enforcing the nesting limit */
static scs_node *parse_nested_form(scs_ctx *ctx, const scs_parser *p, scs_str txt, int pos) {
  ctx->nesting_depth++;
  if (ctx->nesting_depth > SCS_MAX_NESTING_DEPTH) {
    scs_throw(ctx, "forms are nested too deeply");
  }
  scs_node *node = p->parse(ctx, p, txt, pos);
  ctx->nesting_depth--;
  return node;
}

/* A form is a token, string, collection, or a reader macro applied to a form.
 * The first character decides which, except for #, which is ambiguous and is
 * resolved by trying the # forms in order. */
static scs_node *parse_form(scs_ctx *ctx, const scs_parser *p, scs_str txt, int pos) {
  (void)p;
  if (pos < 0 || pos >= txt.len) {
    return NULL;
  }
  char ch = txt.data[pos];
  if (ch == '(') {
    return parse_nested_form(ctx, &p_parens, txt, pos);
  } else if (ch == '[') {
    return parse_nested_form(ctx, &p_brackets, txt, pos);
  } else if (ch == '{') {
    return parse_nested_form(ctx, &p_braces, txt, pos);
  } else if (ch == '"') {
    return p_string.parse(ctx, &p_string, txt, pos);
  } else if (ch == '@' || ch == '\'' || ch == '`' || ch == '~') {
    return parse_nested_form(ctx, &p_wrap, txt, pos);
  } else if (ch == '^') {
    return parse_nested_form(ctx, &p_meta, txt, pos);
  } else if (ch == '#') {
    return parse_nested_form(ctx, &p_hash_form, txt, pos);
  } else {
    return p_token.parse(ctx, &p_token, txt, pos);
  }
}

static const scs_parser p_form = {.parse = parse_form, .name = NULL};

/* source: gaps and forms until the end of the input; anything else is an error node */
static const scs_parser p_source_error = {.parse = parse_any_char, .name = "error"};
static const scs_parser *const source_item_parsers[] = {&p_gap, &p_form, &p_source_error};
static const scs_parser p_source_item = {.parse = parse_choice,
                                         .name = NULL,
                                         .parsers = source_item_parsers,
                                         .num_parsers = NUM_PARSERS(source_item_parsers)};
static const scs_parser p_source = {
    .parse = parse_repeat, .name = "source", .parser = &p_source_item};

/* -----------------------------------------------------------------------------
 * Parse
 */

/* converts the byte offsets in a tree to code point indexes using a lookup table */
static void convert_node_indexes(scs_node *node, const int *cp_at_byte) {
  node->start_idx = cp_at_byte[node->start_idx];
  node->end_idx = cp_at_byte[node->end_idx];
  for (int i = 0; i < node->children.size; i++) {
    convert_node_indexes(node->children.items[i], cp_at_byte);
  }
}

/* parses input text (Clojure code) into a tree of nodes */
static scs_node *parse(scs_ctx *ctx, scs_str txt) {
  ctx->nesting_depth = 0;
  scs_node *tree = p_source.parse(ctx, &p_source, txt, 0);

  /* node indexes are byte offsets at this point; they only differ from code
   * point indexes when the input contains multi-byte characters */
  bool is_ascii = true;
  for (int i = 0; i < txt.len; i++) {
    if ((unsigned char)txt.data[i] >= 0x80) {
      is_ascii = false;
      break;
    }
  }
  if (!is_ascii) {
    int *cp_at_byte = scs_alloc(ctx, ((size_t)txt.len + 1) * sizeof(int));
    int cp = 0;
    int byte_idx = 0;
    while (byte_idx < txt.len) {
      int num_bytes = utf8_char_bytes(txt, byte_idx);
      for (int i = 0; i < num_bytes; i++) {
        cp_at_byte[byte_idx + i] = cp;
      }
      byte_idx += num_bytes;
      cp++;
    }
    cp_at_byte[txt.len] = cp;
    convert_node_indexes(tree, cp_at_byte);
  }

  return tree;
}

/* -----------------------------------------------------------------------------
 * Format Helpers
 */

static bool node_name_is(const scs_node *n, const char *name) {
  return n->name != NULL && strcmp(n->name, name) == 0;
}

/* a whitespace node whose text contains a newline */
static bool is_newline_node(const scs_node *n) {
  return node_name_is(n, "whitespace") && str_includes(n->text, STR("\n"));
}

static bool is_whitespace_node(const scs_node *n) {
  return node_name_is(n, "whitespace");
}

/* a comment needs a space before it when the line does not already end with
 * whitespace or an opening bracket */
static bool comment_needs_space_before(scs_str line_txt, scs_str node_txt) {
  return str_starts_with(node_txt, STR(";")) && line_txt.len != 0 &&
         !str_ends_with(line_txt, STR(" ")) && !str_ends_with(line_txt, STR("(")) &&
         !str_ends_with(line_txt, STR("[")) && !str_ends_with(line_txt, STR("{"));
}

/* a comment needs a space after its semicolons unless one is already there or
 * the comment is only semicolons (";foo" yes, "; foo" no, ";;" no) */
static bool comment_needs_space_inside(scs_str comment_txt) {
  int idx = 0;
  while (idx < comment_txt.len && comment_txt.data[idx] == ';') {
    idx++;
  }
  if (idx == 0) {
    return true;
  }
  if (idx == comment_txt.len) {
    return false;
  }
  return comment_txt.data[idx] != ' ';
}

/* Removes leading commas and spaces, the newline(s) after them, and the spaces
 * after the newline(s), then any trailing whitespace. Text that does not start
 * with that pattern only has its trailing whitespace removed. */
static scs_str remove_leading_whitespace(scs_str txt) {
  int idx = 0;
  while (idx < txt.len && (txt.data[idx] == ',' || txt.data[idx] == ' ')) {
    idx++;
  }
  if (idx < txt.len && txt.data[idx] == '\n') {
    while (idx < txt.len && txt.data[idx] == '\n') {
      idx++;
    }
    while (idx < txt.len && txt.data[idx] == ' ') {
      idx++;
    }
    scs_str rest;
    rest.data = txt.data + idx;
    rest.len = txt.len - idx;
    return rtrim(rest);
  }
  return rtrim(txt);
}

/* is ch a space or comma character? */
static bool is_space_or_comma(scs_str ch) {
  return ch.len == 1 && (ch.data[0] == ' ' || ch.data[0] == ',');
}

/* removes trailing spaces and commas (but not newlines: this operates on a single line) */
static scs_str remove_trailing_whitespace(scs_str txt) {
  int end_idx = txt.len;
  while (end_idx > 0 && (txt.data[end_idx - 1] == ' ' || txt.data[end_idx - 1] == ',')) {
    end_idx--;
  }
  scs_str result;
  result.data = txt.data;
  result.len = end_idx;
  return result;
}

/* returns the text after the last newline (all of txt when there is no newline) */
static scs_str remove_chars_up_to_newline(scs_str txt) {
  return substr(txt, str_last_index_of(txt, STR("\n")) + 1, -1);
}

/* is there a comma after the last newline? */
static bool txt_has_commas_after_newline(scs_str s) {
  int last_newline_idx = str_last_index_of(s, STR("\n"));
  if (last_newline_idx == -1) {
    return false;
  }
  return str_index_of_from(s, STR(","), last_newline_idx) != -1;
}

/* the number of characters after the last newline in a newline node */
static int num_spaces_after_newline(const scs_node *newline_node) {
  return str_len(newline_node->text) - str_last_index_of(newline_node->text, STR("\n")) - 1;
}

static bool is_newline_node_with_comma_on_next_line(const scs_node *n) {
  return n != NULL && is_newline_node(n) && txt_has_commas_after_newline(n->text);
}

/* -----------------------------------------------------------------------------
 * Node Predicates
 */

/* does the node have a non-empty name equal to this one? (node may be NULL) */
static bool node_name_eq(const scs_node *n, const char *name) {
  return n != NULL && node_name_is(n, name);
}

/* does the node have text equal to this? (node may be NULL) */
static bool node_text_is(const scs_node *n, const char *text) {
  return n != NULL && str_eq_cstr(n->text, text);
}

static bool node_contains_text(const scs_node *n) {
  return n != NULL && n->text.len > 0;
}

static bool is_node_with_non_blank_text(const scs_node *n) {
  return node_contains_text(n) && n->text.data[0] != ' ';
}

static bool is_ns_node(const scs_node *n) {
  return node_name_eq(n, "token") && node_text_is(n, "ns");
}

static bool is_use_node(const scs_node *n) {
  return node_text_is(n, ":use") || node_text_is(n, "use");
}

static bool is_require_node(const scs_node *n) {
  return node_text_is(n, ":require") || node_text_is(n, "require");
}

static bool is_require_macros_keyword(const scs_node *n) {
  return node_text_is(n, ":require-macros");
}

static bool is_refer_clojure_node(const scs_node *n) {
  return node_text_is(n, ":refer-clojure") || node_text_is(n, "refer-clojure");
}

static bool is_exclude_keyword(const scs_node *n) {
  return node_text_is(n, ":exclude");
}

static bool is_only_keyword(const scs_node *n) {
  return node_text_is(n, ":only");
}

static bool is_rename_keyword(const scs_node *n) {
  return node_text_is(n, ":rename");
}

static bool is_as_keyword(const scs_node *n) {
  return node_text_is(n, ":as");
}

static bool is_as_alias_keyword(const scs_node *n) {
  return node_text_is(n, ":as-alias");
}

static bool is_refer_keyword(const scs_node *n) {
  return node_text_is(n, ":refer");
}

static bool is_default_keyword(const scs_node *n) {
  return node_text_is(n, ":default");
}

static bool is_refer_macros_keyword(const scs_node *n) {
  return node_text_is(n, ":refer-macros");
}

static bool is_include_macros_node(const scs_node *n) {
  return node_text_is(n, ":include-macros");
}

static bool is_boolean_node(const scs_node *n) {
  return node_text_is(n, "true") || node_text_is(n, "false");
}

static bool is_all_node(const scs_node *n) {
  return node_text_is(n, ":all");
}

static bool is_keyword_node(const scs_node *n) {
  return n != NULL && str_starts_with(n->text, STR(":"));
}

static bool is_import_node(const scs_node *n) {
  return node_text_is(n, ":import") || node_text_is(n, "import");
}

static bool is_comma_node(const scs_node *n) {
  return node_name_is(n, "whitespace") && str_includes(n->text, STR(","));
}

static bool is_namespaced_map_opener(const scs_node *n) {
  return node_name_eq(n, ".open") && str_starts_with(n->text, STR("#:")) &&
         str_ends_with(n->text, STR("{"));
}

static bool is_paren_opener(const scs_node *n) {
  if (!node_name_eq(n, ".open")) {
    return false;
  }
  return node_text_is(n, "(") || node_text_is(n, "[") || node_text_is(n, "{") ||
         node_text_is(n, "#{") || node_text_is(n, "#(") || node_text_is(n, "#?(") ||
         node_text_is(n, "#?@(") || is_namespaced_map_opener(n);
}

static bool is_paren_closer(const scs_node *n) {
  return node_name_eq(n, ".close") &&
         (node_text_is(n, ")") || node_text_is(n, "]") || node_text_is(n, "}"));
}

static bool is_token_node(const scs_node *n) {
  return node_name_is(n, "token");
}

static bool is_tag_node(const scs_node *n) {
  return node_name_is(n, ".tag");
}

/* a complete string node: opener, body, closer */
static bool is_string_node(const scs_node *n) {
  return node_name_eq(n, "string") && n->children.size == 3 &&
         node_name_is(n->children.items[1], ".body");
}

static scs_str get_text_from_string_node(const scs_node *n) {
  return n->children.items[1]->text;
}

static bool is_comment_node(const scs_node *n) {
  return node_name_is(n, "comment");
}

static bool is_reader_comment_node(const scs_node *n) {
  return node_name_is(n, "discard");
}

static bool is_discard_node(const scs_node *n) {
  return node_name_eq(n, "marker") && node_text_is(n, "#_");
}

static bool is_standard_clj_ignore_keyword(const scs_node *n) {
  return node_name_is(n, "token") && node_text_is(n, ":standard-clj/ignore");
}

static bool is_standard_clj_ignore_file_keyword(const scs_node *n) {
  return node_name_eq(n, "token") && node_text_is(n, ":standard-clj/ignore-file");
}

static bool node_contains_text_and_not_whitespace(const scs_node *n) {
  return node_contains_text(n) && !is_whitespace_node(n);
}

static bool is_map_literal_opener(const scs_node *n) {
  return node_name_eq(n, ".open") && node_text_is(n, "{");
}

static bool is_vector_literal_opener(const scs_node *n) {
  return node_name_eq(n, ".open") && node_text_is(n, "[");
}

static bool is_single_paren_opener(const scs_node *n) {
  return node_name_eq(n, ".open") && node_text_is(n, "(");
}

static bool is_anon_fn_opener(const scs_node *n) {
  return node_name_eq(n, ".open") && node_text_is(n, "#(");
}

static bool is_set_literal_opener(const scs_node *n) {
  return node_name_eq(n, ".open") && node_text_is(n, "#{");
}

static bool is_reader_conditional_opener(const scs_node *n) {
  return node_name_eq(n, ".open") && (node_text_is(n, "#?(") || node_text_is(n, "#?@("));
}

/* a braces node with an opener, a body, and a closing } */
static bool is_opening_brace_node(const scs_node *n) {
  return node_name_eq(n, "braces") && n->children.size == 3 &&
         node_name_is(n->children.items[2], ".close") && node_text_is(n->children.items[2], "}");
}

/* a braces node that is a map literal: { ... } */
static bool is_map_literal_node(const scs_node *n) {
  if (!node_name_eq(n, "braces") || n->children.size < 2) {
    return false;
  }
  const scs_node *last = n->children.items[n->children.size - 1];
  return is_map_literal_opener(n->children.items[0]) && node_name_is(last, ".close") &&
         node_text_is(last, "}");
}

/* a brackets node that is a vector literal: [ ... ] */
static bool is_vector_literal_node(const scs_node *n) {
  if (!node_name_eq(n, "brackets") || n->children.size < 2) {
    return false;
  }
  const scs_node *last = n->children.items[n->children.size - 1];
  return is_vector_literal_opener(n->children.items[0]) && node_name_is(last, ".close") &&
         node_text_is(last, "]");
}

static bool is_meta_marker(const scs_node *n) {
  return node_name_eq(n, ".marker") && (node_text_is(n, "^") || node_text_is(n, "#^"));
}

static bool is_string_opener(const scs_node *n) {
  return node_name_eq(n, ".open") && (node_text_is(n, "\"") || node_text_is(n, "#?\""));
}

static bool is_string_closer(const scs_node *n) {
  return node_name_eq(n, ".close") && node_text_is(n, "\"");
}

static bool is_gen_class_node(const scs_node *n) {
  return node_text_is(n, ":gen-class");
}

/* -----------------------------------------------------------------------------
 * :gen-class Keys
 *
 * These are all of the :gen-class keys and their value types:
 * https://github.com/clojure/clojure/blob/master/src/clj/clojure/genclass.clj
 *
 *   :name aname
 *   :extends aclass
 *   :implements [interface ...]
 *   :init name
 *   :constructors {[param-types] [super-param-types], ...}
 *   :post-init name
 *   :methods [ [name [param-types] return-type], ...]
 *   :main boolean
 *   :factory name
 *   :state name
 *   :exposes {protected-field-name {:get name :set name}, ...}
 *   :exposes-methods {super-method-name exposed-name, ...}
 *   :prefix string
 *   :impl-ns name
 *   :load-impl-ns boolean
 */

typedef enum scs_gen_class_key {
  GEN_CLASS_NAME,
  GEN_CLASS_EXTENDS,
  GEN_CLASS_IMPLEMENTS,
  GEN_CLASS_INIT,
  GEN_CLASS_CONSTRUCTORS,
  GEN_CLASS_POST_INIT,
  GEN_CLASS_METHODS,
  GEN_CLASS_MAIN,
  GEN_CLASS_FACTORY,
  GEN_CLASS_STATE,
  GEN_CLASS_EXPOSES,
  GEN_CLASS_EXPOSES_METHODS,
  GEN_CLASS_PREFIX,
  GEN_CLASS_IMPL_NS,
  GEN_CLASS_LOAD_IMPL_NS,
  GEN_CLASS_NUM_KEYS
} scs_gen_class_key;

/* the key names, in the order of the clojure.core.genclass documentation
 * (which is also the order they are printed in) */
static const char *const gen_class_key_names[GEN_CLASS_NUM_KEYS] = {
    "name",      "extends",         "implements", "init",    "constructors",
    "post-init", "methods",         "main",       "factory", "state",
    "exposes",   "exposes-methods", "prefix",     "impl-ns", "load-impl-ns"};

/* returns the key for a keyword such as :name, or -1 if it is not a :gen-class key */
static int gen_class_key_from_keyword(scs_str txt) {
  if (!str_starts_with(txt, STR(":"))) {
    return -1;
  }
  scs_str name = str_slice(txt, 1, txt.len);
  for (int key = 0; key < GEN_CLASS_NUM_KEYS; key++) {
    if (str_eq_cstr(name, gen_class_key_names[key])) {
      return key;
    }
  }
  return -1;
}

static bool is_gen_class_keyword(const scs_node *n) {
  return n != NULL && gen_class_key_from_keyword(n->text) != -1;
}

/* the :gen-class keys whose value is a symbol or string */
static bool is_gen_class_name_key(int key) {
  return key == GEN_CLASS_NAME || key == GEN_CLASS_EXTENDS || key == GEN_CLASS_INIT ||
         key == GEN_CLASS_POST_INIT || key == GEN_CLASS_FACTORY || key == GEN_CLASS_STATE ||
         key == GEN_CLASS_IMPL_NS;
}

static bool is_gen_class_boolean_key(int key) {
  return key == GEN_CLASS_MAIN || key == GEN_CLASS_LOAD_IMPL_NS;
}

/* -----------------------------------------------------------------------------
 * Tree Traversal
 */

typedef void (*scs_node_visitor)(scs_node *node, void *data);

/* calls f on node and on every node beneath it, in document order */
static void recurse_all_children(scs_node *node, scs_node_visitor f, void *data) {
  f(node, data);
  for (int i = 0; i < node->children.size; i++) {
    recurse_all_children(node->children.items[i], f, data);
  }
}

typedef struct text_collector {
  scs_ctx *ctx;
  scs_strbuf buf;
} text_collector;

static void collect_node_text(scs_node *n, void *data) {
  text_collector *collector = data;
  /* a .tag node's text does not include its leading # */
  if (is_tag_node(n)) {
    strbuf_append(collector->ctx, &collector->buf, STR("#"));
  }
  if (n->text.len > 0) {
    strbuf_append(collector->ctx, &collector->buf, n->text);
  }
}

/* returns all of the text found within a node and its children */
static scs_str get_text_from_root_node(scs_ctx *ctx, scs_node *root_node) {
  text_collector collector = {ctx, {NULL, 0, 0}};
  recurse_all_children(root_node, collect_node_text, &collector);
  return strbuf_to_str(&collector.buf);
}

/* returns the metadata forms attached by a meta node, including their markers,
 * in source order */
static scs_str_list get_metadata_strings_from_meta_node(scs_ctx *ctx, scs_node *meta_node) {
  scs_str_list metadata = {NULL, 0, 0};
  scs_str marker = EMPTY_STR;
  for (int idx = 0; idx < meta_node->children.size; idx++) {
    scs_node *child_node = meta_node->children.items[idx];
    if (is_meta_marker(child_node)) {
      marker = child_node->text;
    } else if (marker.len > 0 && node_name_is(child_node, ".meta")) {
      str_list_push(ctx, &metadata,
                    str_concat(ctx, marker, get_text_from_root_node(ctx, child_node)));
      marker = EMPTY_STR;
    }
  }
  return metadata;
}

/* returns the wrapped body node from a meta node, or NULL */
static scs_node *get_body_node_from_meta_node(scs_node *meta_node) {
  for (int idx = 0; idx < meta_node->children.size; idx++) {
    scs_node *child_node = meta_node->children.items[idx];
    if (node_name_is(child_node, ".body")) {
      return child_node;
    }
  }
  return NULL;
}

static void remember_last_node_with_text(scs_node *n, void *data) {
  if (n->text.len > 0) {
    *(scs_node **)data = n;
  }
}

/* returns the last node with text within a node and its children, or NULL */
static scs_node *get_last_child_node_with_text(scs_node *root_node) {
  scs_node *last_node = NULL;
  recurse_all_children(root_node, remember_last_node_with_text, &last_node);
  return last_node;
}

typedef struct node_collector {
  scs_ctx *ctx;
  scs_node_list nodes;
} node_collector;

static void collect_node(scs_node *n, void *data) {
  node_collector *collector = data;
  node_list_push(collector->ctx, &collector->nodes, n);
}

/* returns every node in the tree as a flat list, in document order */
static scs_node_list flatten_tree(scs_ctx *ctx, scs_node *tree) {
  node_collector collector = {ctx, {NULL, 0, 0}};
  recurse_all_children(tree, collect_node, &collector);
  return collector.nodes;
}

/* returns the next node at or after idx that has text, or NULL */
static scs_node *find_next_node_with_text(const scs_node_list *nodes, int idx) {
  for (; idx < nodes->size; idx++) {
    if (nodes->items[idx]->text.len > 0) {
      return nodes->items[idx];
    }
  }
  return NULL;
}

/* returns the next node at or after idx that is not whitespace, or NULL */
static scs_node *find_next_non_whitespace_node(const scs_node_list *nodes, int idx) {
  for (; idx < nodes->size; idx++) {
    if (!is_whitespace_node(nodes->items[idx])) {
      return nodes->items[idx];
    }
  }
  return NULL;
}

/* -----------------------------------------------------------------------------
 * Parsed ns Form
 *
 * The data structure that describes an ns form, built by parse_ns() and
 * printed from scratch by format_ns(). Optional parts are pointers that are
 * NULL when the ns form does not contain them. Optional strings are empty when
 * absent (an ns form never contains an empty symbol or comment).
 */

/* a symbol, and the reader conditional platform (such as :clj) it appears under */
typedef struct scs_ns_symbol {
  scs_str symbol;
  scs_str platform;
} scs_ns_symbol;

typedef struct scs_ns_symbol_list {
  scs_ns_symbol *items;
  int size;
  int capacity;
} scs_ns_symbol_list;

/* one entry of a :rename map */
typedef struct scs_ns_rename {
  scs_str from_symbol;
  scs_str to_symbol;
  scs_str platform;
} scs_ns_rename;

typedef struct scs_ns_rename_list {
  scs_ns_rename *items;
  int size;
  int capacity;
} scs_ns_rename_list;

/* one key / value pair of the ns metadata map */
typedef struct scs_ns_metadata {
  scs_str key;
  scs_str value;
} scs_ns_metadata;

typedef struct scs_ns_metadata_list {
  scs_ns_metadata *items;
  int size;
  int capacity;
} scs_ns_metadata_list;

typedef struct scs_ns_refer_clojure {
  scs_ns_symbol_list *exclude;
  scs_ns_symbol_list *only;
  scs_ns_rename_list *rename;
} scs_ns_refer_clojure;

/* a :require (or :require-macros) libspec */
typedef struct scs_ns_require {
  scs_str symbol;
  bool symbol_is_string; /* a string require such as ["react" :as react] */
  scs_str as;
  scs_str as_alias;
  scs_str default_symbol; /* :default (ClojureScript) */
  bool refer_all;         /* :refer :all */
  scs_ns_symbol_list *refer;
  scs_ns_symbol_list *exclude;
  bool has_include_macros;
  bool include_macros;
  scs_str_list *refer_macros;
  scs_ns_rename_list *rename;
  scs_str_list *metadata;
  scs_str_list *comments_above;
  scs_str comment_after;
  scs_str platform;
  int prefix_list_id; /* set while parsing a prefix list; 0 once parsing is complete */
} scs_ns_require;

typedef struct scs_ns_require_list {
  scs_ns_require *items;
  int size;
  int capacity;
} scs_ns_require_list;

/* one package of an :import form */
typedef struct scs_ns_import {
  scs_str package;
  scs_str_list classes;
  scs_str_list *comments_above;
  scs_str comment_after;
  scs_str platform;
} scs_ns_import;

typedef struct scs_ns_import_list {
  scs_ns_import *items;
  int size;
  int capacity;
} scs_ns_import_list;

typedef struct scs_gen_class_constructor {
  scs_str metadata;
  scs_str_list parameter_types;
  scs_str_list super_parameter_types;
} scs_gen_class_constructor;

typedef struct scs_gen_class_constructor_list {
  scs_gen_class_constructor *items;
  int size;
  int capacity;
} scs_gen_class_constructor_list;

typedef struct scs_gen_class_method {
  scs_str metadata;
  scs_str name;
  scs_str_list parameter_types;
  scs_str return_type;
} scs_gen_class_method;

typedef struct scs_gen_class_method_list {
  scs_gen_class_method *items;
  int size;
  int capacity;
} scs_gen_class_method_list;

typedef struct scs_gen_class_expose {
  scs_str field_name;
  scs_str getter;
  scs_str setter;
} scs_gen_class_expose;

typedef struct scs_gen_class_expose_list {
  scs_gen_class_expose *items;
  int size;
  int capacity;
} scs_gen_class_expose_list;

typedef struct scs_gen_class_exposes_method {
  scs_str super_method_name;
  scs_str exposed_name;
} scs_gen_class_exposes_method;

typedef struct scs_gen_class_exposes_method_list {
  scs_gen_class_exposes_method *items;
  int size;
  int capacity;
} scs_gen_class_exposes_method_list;

/* One :gen-class key. Which value fields are meaningful depends on the key:
 * name-like keys and :prefix use `value`, boolean keys use `bool_value`, and
 * the structured keys (:implements, :constructors, :methods, :exposes,
 * :exposes-methods) use the lists in scs_ns_gen_class. */
typedef struct scs_gen_class_entry {
  bool present;   /* the key appeared in the :gen-class form */
  bool has_value; /* a value was parsed for it */
  scs_str value;
  bool bool_value;
  scs_str metadata;
  scs_str_list *comments_above;
  scs_str comment_after;
} scs_gen_class_entry;

typedef struct scs_ns_gen_class {
  bool is_empty; /* (:gen-class) with no keys */
  scs_str platform;
  scs_str_list *comments_above;
  scs_str comment_after;
  scs_gen_class_entry entries[GEN_CLASS_NUM_KEYS];
  scs_ns_symbol_list implements;
  scs_gen_class_constructor_list constructors;
  scs_gen_class_method_list methods;
  scs_gen_class_expose_list exposes;
  scs_gen_class_exposes_method_list exposes_methods;
} scs_ns_gen_class;

typedef struct scs_ns {
  scs_str ns_symbol; /* empty when the input has no ns form */
  bool has_docstring;
  scs_str docstring;
  scs_ns_metadata_list *ns_metadata;
  scs_ns_refer_clojure *refer_clojure;
  scs_str_list *refer_clojure_comments_above;
  scs_str refer_clojure_comment_after;
  scs_ns_require_list *require_macros;
  scs_str_list *require_macros_comments_above;
  scs_ns_require_list *
    requires;
  scs_str_list *require_comments_above;
  scs_str require_comment_after;
  scs_ns_import_list *imports;
  scs_str_list *import_comments_above;
  scs_str import_comment_after;
  scs_ns_gen_class *gen_class;
  scs_str comment_outside_ns_form; /* a comment on the line that closes the ns form */
} scs_ns;

/* -----------------------------------------------------------------------------
 * Sorting
 */

typedef int (*scs_compare_fn)(const void *a, const void *b);

/* Sorts an array in place, keeping the original order of items that compare
 * equal (a stable sort). Insertion sort: the arrays here are short. */
static void stable_sort(scs_ctx *ctx, void *base, int n, size_t item_size, scs_compare_fn cmp) {
  char *items = base;
  char *tmp = scs_alloc(ctx, item_size);
  for (int i = 1; i < n; i++) {
    memcpy(tmp, items + (size_t)i * item_size, item_size);
    int j = i - 1;
    while (j >= 0 && cmp(items + (size_t)j * item_size, tmp) > 0) {
      memcpy(items + (size_t)(j + 1) * item_size, items + (size_t)j * item_size, item_size);
      j--;
    }
    memcpy(items + (size_t)(j + 1) * item_size, tmp, item_size);
  }
}

/* orders by symbol, then by platform; an item without a platform compares
 * equal to any platform */
static int compare_symbol_then_platform(scs_str symbol_a, scs_str platform_a, scs_str symbol_b,
                                        scs_str platform_b) {
  int cmp = str_compare(symbol_a, symbol_b);
  if (cmp != 0) {
    return cmp;
  }
  if (platform_a.len == 0 || platform_b.len == 0) {
    return 0;
  }
  return str_compare(platform_a, platform_b);
}

static int compare_symbols_then_platform(const void *a, const void *b) {
  const scs_ns_symbol *itm_a = a;
  const scs_ns_symbol *itm_b = b;
  return compare_symbol_then_platform(itm_a->symbol, itm_a->platform, itm_b->symbol,
                                      itm_b->platform);
}

static int compare_requires(const void *a, const void *b) {
  const scs_ns_require *req_a = a;
  const scs_ns_require *req_b = b;
  return compare_symbol_then_platform(req_a->symbol, req_a->platform, req_b->symbol,
                                      req_b->platform);
}

static int compare_from_symbol(const void *a, const void *b) {
  const scs_ns_rename *itm_a = a;
  const scs_ns_rename *itm_b = b;
  return str_compare(itm_a->from_symbol, itm_b->from_symbol);
}

static int compare_imports(const void *a, const void *b) {
  const scs_ns_import *import_a = a;
  const scs_ns_import *import_b = b;
  return str_compare(import_a->package, import_b->package);
}

static int compare_strings(const void *a, const void *b) {
  return str_compare(*(const scs_str *)a, *(const scs_str *)b);
}

static void sort_symbols(scs_ctx *ctx, scs_ns_symbol_list *list) {
  if (list != NULL) {
    stable_sort(ctx, list->items, list->size, sizeof(scs_ns_symbol), compare_symbols_then_platform);
  }
}

static void sort_renames(scs_ctx *ctx, scs_ns_rename_list *list) {
  if (list != NULL) {
    stable_sort(ctx, list->items, list->size, sizeof(scs_ns_rename), compare_from_symbol);
  }
}

/* -----------------------------------------------------------------------------
 * Parse Namespace
 */

/* does a symbol look like a Java class name? (its first letter is upper case) */
static bool looks_like_a_java_classname(scs_ctx *ctx, scs_str s) {
  scs_str first_char = char_at(s, 0);
  return str_eq(ascii_to_upper_case(ctx, first_char), first_char);
}

typedef struct scs_java_package {
  scs_str package;
  scs_str class_name; /* empty when s is a package without a class */
} scs_java_package;

/* splits "java.util.Date" into its package and class name */
static scs_java_package parse_java_package_with_class(scs_ctx *ctx, scs_str s) {
  scs_str_list chunks = str_split(ctx, s, STR("."));
  scs_str last_itm = str_list_last(&chunks);

  scs_java_package result;
  if (looks_like_a_java_classname(ctx, last_itm)) {
    scs_str_list package_chunks = str_list_drop_last(ctx, &chunks);
    result.package = str_join(ctx, &package_chunks, STR("."));
    result.class_name = last_itm;
  } else {
    result.package = s;
    result.class_name = EMPTY_STR;
  }
  return result;
}

/* returns the next token node inside a :require list / vector, starting from
 * idx, or NULL if a closing paren is reached first */
static scs_node *find_next_token_inside_require_form(const scs_node_list *nodes, int idx) {
  for (; idx < nodes->size; idx++) {
    scs_node *node = nodes->items[idx];
    if (is_paren_closer(node)) {
      return NULL;
    }
    if (is_token_node(node) && node->text.len > 0) {
      return node;
    }
  }
  return NULL;
}

/* the form nodes directly inside a collection, skipping whitespace and comments */
static scs_node_list get_direct_form_nodes_from_collection(scs_ctx *ctx,
                                                           scs_node *collection_node) {
  scs_node_list form_nodes = {NULL, 0, 0};
  if (collection_node->children.size == 3 &&
      node_name_is(collection_node->children.items[1], ".body")) {
    scs_node_list *body_children = &collection_node->children.items[1]->children;
    for (int idx = 0; idx < body_children->size; idx++) {
      scs_node *child_node = body_children->items[idx];
      if (!is_whitespace_node(child_node) && !is_comment_node(child_node)) {
        node_list_push(ctx, &form_nodes, child_node);
      }
    }
  }
  return form_nodes;
}

/* the text of a :gen-class symbol, string, or metadata form; empty if the node
 * is none of those */
static scs_str get_gen_class_symbol_text(scs_ctx *ctx, scs_node *node) {
  if (node == NULL) {
    return EMPTY_STR;
  }
  if (is_token_node(node)) {
    return node->text;
  }
  if (is_string_node(node)) {
    return str_concat3(ctx, STR("\""), get_text_from_string_node(node), STR("\""));
  }
  if (node_name_is(node, "meta")) {
    return get_text_from_root_node(ctx, node);
  }
  return EMPTY_STR;
}

typedef struct scs_gen_class_metadata_form {
  scs_node *body_node;
  scs_str metadata; /* empty when the form has no metadata */
} scs_gen_class_metadata_form;

/* separates a form from the metadata attached to it */
static scs_gen_class_metadata_form split_gen_class_metadata_form(scs_ctx *ctx, scs_node *node) {
  scs_gen_class_metadata_form result = {node, EMPTY_STR};
  if (node == NULL || !node_name_is(node, "meta")) {
    return result;
  }

  scs_strbuf metadata = {NULL, 0, 0};
  result.body_node = NULL;
  for (int idx = 0; idx < node->children.size; idx++) {
    scs_node *child_node = node->children.items[idx];
    if (node_name_is(child_node, ".body")) {
      if (child_node->children.size == 1) {
        result.body_node = child_node->children.items[0];
      }
      break;
    }
    strbuf_append(ctx, &metadata, get_text_from_root_node(ctx, child_node));
  }
  result.metadata = rtrim(strbuf_to_str(&metadata));
  return result;
}

static scs_str_list parse_gen_class_type_vector(scs_ctx *ctx, scs_node *vector_node,
                                                const char *error_message) {
  scs_node_list type_nodes = get_direct_form_nodes_from_collection(ctx, vector_node);
  scs_str_list types = {NULL, 0, 0};
  for (int idx = 0; idx < type_nodes.size; idx++) {
    scs_str type_name = get_gen_class_symbol_text(ctx, type_nodes.items[idx]);
    if (type_name.len == 0) {
      scs_throw(ctx, error_message);
    }
    str_list_push(ctx, &types, type_name);
  }
  return types;
}

typedef struct scs_node_pair {
  scs_node *first;
  scs_node *second;
} scs_node_pair;

typedef struct scs_node_pair_list {
  scs_node_pair *items;
  int size;
  int capacity;
} scs_node_pair_list;

/* the key / value pairs of a map literal; throws error_message for an odd count */
static scs_node_pair_list get_gen_class_map_pairs(scs_ctx *ctx, scs_node *map_node,
                                                  const char *error_message) {
  scs_node_list form_nodes = get_direct_form_nodes_from_collection(ctx, map_node);
  if (form_nodes.size % 2 != 0) {
    scs_throw(ctx, error_message);
  }
  scs_node_pair_list pairs = {NULL, 0, 0};
  for (int idx = 0; idx < form_nodes.size; idx += 2) {
    scs_node_pair pair = {form_nodes.items[idx], form_nodes.items[idx + 1]};
    LIST_PUSH(ctx, &pairs, pair);
  }
  return pairs;
}

static scs_gen_class_constructor_list parse_gen_class_constructors(scs_ctx *ctx,
                                                                   scs_node *map_node) {
  const char *constructor_error_message =
      ":gen-class :constructors must be a map of parameter and super-parameter vectors.";
  scs_node_pair_list pairs = get_gen_class_map_pairs(
      ctx, map_node,
      ":gen-class :constructors must contain parameter and super-parameter vector pairs.");

  scs_gen_class_constructor_list constructors = {NULL, 0, 0};
  for (int pair_idx = 0; pair_idx < pairs.size; pair_idx++) {
    scs_gen_class_metadata_form parameter_types_form =
        split_gen_class_metadata_form(ctx, pairs.items[pair_idx].first);
    scs_node *parameter_types_node = parameter_types_form.body_node;
    scs_node *super_parameter_types_node = pairs.items[pair_idx].second;

    if (!is_vector_literal_node(parameter_types_node) ||
        !is_vector_literal_node(super_parameter_types_node)) {
      scs_throw(ctx, constructor_error_message);
    }

    scs_gen_class_constructor constructor;
    memset(&constructor, 0, sizeof(constructor));
    constructor.metadata = parameter_types_form.metadata;
    constructor.parameter_types =
        parse_gen_class_type_vector(ctx, parameter_types_node, constructor_error_message);
    constructor.super_parameter_types =
        parse_gen_class_type_vector(ctx, super_parameter_types_node, constructor_error_message);
    LIST_PUSH(ctx, &constructors, constructor);
  }
  return constructors;
}

static scs_str_list parse_gen_class_parameter_types(scs_ctx *ctx, scs_node *vector_node) {
  return parse_gen_class_type_vector(
      ctx, vector_node, ":gen-class :methods parameter types must be symbols inside a vector.");
}

static scs_gen_class_method_list parse_gen_class_methods(scs_ctx *ctx, scs_node *vector_node) {
  scs_node_list method_nodes = get_direct_form_nodes_from_collection(ctx, vector_node);
  scs_gen_class_method_list methods = {NULL, 0, 0};
  for (int method_idx = 0; method_idx < method_nodes.size; method_idx++) {
    scs_gen_class_metadata_form method_form =
        split_gen_class_metadata_form(ctx, method_nodes.items[method_idx]);
    scs_node *method_node = method_form.body_node;
    if (!is_vector_literal_node(method_node)) {
      scs_throw(ctx, ":gen-class :methods must be a vector of method vectors.");
    }

    scs_node_list method_form_nodes = get_direct_form_nodes_from_collection(ctx, method_node);
    if (method_form_nodes.size != 3) {
      scs_throw(ctx, ":gen-class :methods entries must contain a name, parameter-types vector, "
                     "and return type.");
    }

    scs_str name = get_gen_class_symbol_text(ctx, method_form_nodes.items[0]);
    scs_node *parameter_types_node = method_form_nodes.items[1];
    scs_str return_type = get_gen_class_symbol_text(ctx, method_form_nodes.items[2]);
    if (name.len == 0) {
      scs_throw(ctx, ":gen-class :methods method names must be symbols.");
    }
    if (!is_vector_literal_node(parameter_types_node)) {
      scs_throw(ctx, ":gen-class :methods parameter types must be provided in a vector.");
    }
    if (return_type.len == 0) {
      scs_throw(ctx, ":gen-class :methods return types must be symbols.");
    }

    scs_gen_class_method method;
    memset(&method, 0, sizeof(method));
    method.metadata = method_form.metadata;
    method.name = name;
    method.parameter_types = parse_gen_class_parameter_types(ctx, parameter_types_node);
    method.return_type = return_type;
    LIST_PUSH(ctx, &methods, method);
  }
  return methods;
}

static scs_gen_class_expose_list parse_gen_class_exposes(scs_ctx *ctx, scs_node *map_node) {
  scs_node_pair_list pairs = get_gen_class_map_pairs(
      ctx, map_node, ":gen-class :exposes must contain protected-field and accessor-map pairs.");
  scs_gen_class_expose_list exposes = {NULL, 0, 0};
  for (int pair_idx = 0; pair_idx < pairs.size; pair_idx++) {
    scs_str field_name = get_gen_class_symbol_text(ctx, pairs.items[pair_idx].first);
    scs_node *accessors_node = pairs.items[pair_idx].second;
    if (field_name.len == 0) {
      scs_throw(ctx, ":gen-class :exposes protected field names must be symbols.");
    }
    if (!is_map_literal_node(accessors_node)) {
      scs_throw(ctx, ":gen-class :exposes field values must be maps containing :get and/or :set.");
    }

    scs_node_pair_list accessor_pairs = get_gen_class_map_pairs(
        ctx, accessors_node,
        ":gen-class :exposes accessor maps must contain keyword and method-name pairs.");
    scs_gen_class_expose expose;
    memset(&expose, 0, sizeof(expose));
    expose.field_name = field_name;
    expose.getter = EMPTY_STR;
    expose.setter = EMPTY_STR;
    bool seen_get = false;
    bool seen_set = false;
    for (int accessor_idx = 0; accessor_idx < accessor_pairs.size; accessor_idx++) {
      scs_node *accessor_key_node = accessor_pairs.items[accessor_idx].first;
      scs_node *accessor_name_node = accessor_pairs.items[accessor_idx].second;
      bool is_get = is_token_node(accessor_key_node) && node_text_is(accessor_key_node, ":get");
      bool is_set = is_token_node(accessor_key_node) && node_text_is(accessor_key_node, ":set");
      scs_str accessor_name = get_gen_class_symbol_text(ctx, accessor_name_node);

      if (!is_get && !is_set) {
        scs_throw(ctx, ":gen-class :exposes accessor keys must be :get or :set.");
      }
      if ((is_get && seen_get) || (is_set && seen_set)) {
        scs_throw(ctx, ":gen-class :exposes accessor maps cannot repeat :get or :set.");
      }
      if (accessor_name.len == 0) {
        scs_throw(ctx, ":gen-class :exposes accessor method names must be symbols.");
      }

      if (is_get) {
        expose.getter = accessor_name;
        seen_get = true;
      } else {
        expose.setter = accessor_name;
        seen_set = true;
      }
    }
    LIST_PUSH(ctx, &exposes, expose);
  }
  return exposes;
}

static scs_gen_class_exposes_method_list parse_gen_class_exposes_methods(scs_ctx *ctx,
                                                                         scs_node *map_node) {
  scs_node_pair_list pairs = get_gen_class_map_pairs(
      ctx, map_node,
      ":gen-class :exposes-methods must contain superclass-method and exposed-name pairs.");
  scs_gen_class_exposes_method_list exposes_methods = {NULL, 0, 0};
  for (int pair_idx = 0; pair_idx < pairs.size; pair_idx++) {
    scs_gen_class_exposes_method itm;
    itm.super_method_name = get_gen_class_symbol_text(ctx, pairs.items[pair_idx].first);
    itm.exposed_name = get_gen_class_symbol_text(ctx, pairs.items[pair_idx].second);
    if (itm.super_method_name.len == 0 || itm.exposed_name.len == 0) {
      scs_throw(ctx, ":gen-class :exposes-methods names must be symbols.");
    }
    LIST_PUSH(ctx, &exposes_methods, itm);
  }
  return exposes_methods;
}

/* comments collected for a :require prefix list, attached to its first
 * require once parsing is complete */
typedef struct scs_prefix_list_comments {
  int prefix_list_id;
  scs_str_list *comments_above;
  scs_str comment_after;
  bool attached; /* already attached to a require */
} scs_prefix_list_comments;

typedef struct scs_prefix_list_comments_list {
  scs_prefix_list_comments *items;
  int size;
  int capacity;
} scs_prefix_list_comments_list;

/* finds or creates the comments entry for a prefix list */
static scs_prefix_list_comments *
prefix_list_comments_for(scs_ctx *ctx, scs_prefix_list_comments_list *list, int prefix_list_id) {
  for (int i = 0; i < list->size; i++) {
    if (list->items[i].prefix_list_id == prefix_list_id) {
      return &list->items[i];
    }
  }
  scs_prefix_list_comments entry;
  memset(&entry, 0, sizeof(entry));
  entry.prefix_list_id = prefix_list_id;
  entry.comment_after = EMPTY_STR;
  LIST_PUSH(ctx, list, entry);
  return &list->items[list->size - 1];
}

/* an empty list allocated in the arena */
static scs_str_list *new_str_list(scs_ctx *ctx) {
  return scs_alloc(ctx, sizeof(scs_str_list));
}

static scs_ns_symbol_list *new_symbol_list(scs_ctx *ctx) {
  return scs_alloc(ctx, sizeof(scs_ns_symbol_list));
}

static scs_ns_rename_list *new_rename_list(scs_ctx *ctx) {
  return scs_alloc(ctx, sizeof(scs_ns_rename_list));
}

static scs_ns_require_list *new_require_list(scs_ctx *ctx) {
  return scs_alloc(ctx, sizeof(scs_ns_require_list));
}

/* a symbol under the current reader conditional platform (if any) */
static scs_ns_symbol make_ns_symbol(scs_str symbol, scs_str platform) {
  scs_ns_symbol itm;
  itm.symbol = symbol;
  itm.platform = platform;
  return itm;
}

static scs_ns_rename make_ns_rename(scs_str from_symbol, scs_str to_symbol, scs_str platform) {
  scs_ns_rename itm;
  itm.from_symbol = from_symbol;
  itm.to_symbol = to_symbol;
  itm.platform = platform;
  return itm;
}

/* a new, empty require; the comments above it are attached if there are any */
static scs_ns_require make_require(scs_str symbol, scs_str_list **pending_metadata) {
  scs_ns_require req;
  memset(&req, 0, sizeof(req));
  req.symbol = symbol;
  req.as = EMPTY_STR;
  req.as_alias = EMPTY_STR;
  req.default_symbol = EMPTY_STR;
  req.comment_after = EMPTY_STR;
  req.platform = EMPTY_STR;
  if (*pending_metadata != NULL && (*pending_metadata)->size > 0) {
    req.metadata = *pending_metadata;
    *pending_metadata = NULL;
  }
  return req;
}

/* The require (or require-macro) at idx. Throws when there is no such require,
 * which happens when the ns form is malformed (for example an :as keyword
 * before any libspec). */
static scs_ns_require *require_at(scs_ctx *ctx, scs_ns_require_list *list, int idx) {
  if (list == NULL || idx < 0 || idx >= list->size) {
    scs_throw(ctx, "could not parse the ns form");
  }
  return &list->items[idx];
}

/* finds or creates the import for a package */
static scs_ns_import *import_for_package(scs_ctx *ctx, scs_ns *result, scs_str package_name) {
  if (result->imports == NULL) {
    result->imports = scs_alloc(ctx, sizeof(scs_ns_import_list));
  }
  for (int i = 0; i < result->imports->size; i++) {
    if (str_eq(result->imports->items[i].package, package_name)) {
      return &result->imports->items[i];
    }
  }
  scs_ns_import import;
  memset(&import, 0, sizeof(import));
  import.package = package_name;
  import.comment_after = EMPTY_STR;
  import.platform = EMPTY_STR;
  LIST_PUSH(ctx, result->imports, import);
  return &result->imports->items[result->imports->size - 1];
}

/* the import for the package being collected; throws if there is none */
static scs_ns_import *active_import(scs_ctx *ctx, scs_ns *result, scs_str package_name) {
  if (result->imports == NULL || package_name.len == 0) {
    scs_throw(ctx, "could not parse the ns form");
  }
  for (int i = 0; i < result->imports->size; i++) {
    if (str_eq(result->imports->items[i].package, package_name)) {
      return &result->imports->items[i];
    }
  }
  scs_throw(ctx, "could not parse the ns form");
}

/* the entry for the :gen-class key being collected; throws if there is none */
static scs_gen_class_entry *gen_class_entry(scs_ctx *ctx, scs_ns *result, int key) {
  if (result->gen_class == NULL || key < 0) {
    scs_throw(ctx, "could not parse the ns form");
  }
  return &result->gen_class->entries[key];
}

/* moves the collected single-line comments onto *target (if there are any) */
static void attach_comments_above(scs_str_list **target, scs_str_list **single_line_comments) {
  if ((*single_line_comments)->size > 0) {
    *target = *single_line_comments;
    *single_line_comments = NULL;
  }
}

/* the sections of the ns form that end-of-line comments can belong to */
typedef enum scs_ns_section {
  NS_SECTION_NONE,
  NS_SECTION_REFER_CLOJURE,
  NS_SECTION_REQUIRE,
  NS_SECTION_IMPORT,
  NS_SECTION_REQUIRE_MACROS,
  NS_SECTION_GEN_CLASS
} scs_ns_section;

/* sorts everything in the parsed ns form and attaches the prefix list comments */
static void sort_ns_result(scs_ctx *ctx, scs_ns *result,
                           scs_prefix_list_comments_list *prefix_list_comments) {
  if (result->refer_clojure != NULL) {
    sort_symbols(ctx, result->refer_clojure->exclude);
    sort_symbols(ctx, result->refer_clojure->only);
    sort_renames(ctx, result->refer_clojure->rename);
  }

  if (result->gen_class != NULL) {
    sort_symbols(ctx, &result->gen_class->implements);
  }

  if (result->require_macros != NULL) {
    stable_sort(ctx, result->require_macros->items, result->require_macros->size,
                sizeof(scs_ns_require), compare_requires);
    for (int rm_idx = 0; rm_idx < result->require_macros->size; rm_idx++) {
      sort_symbols(ctx, result->require_macros->items[rm_idx].refer);
    }
  }

  if (result->requires != NULL) {
    stable_sort(ctx, result->requires->items, result->requires->size, sizeof(scs_ns_require),
                compare_requires);

    for (int requires_idx = 0; requires_idx < result->requires->size; requires_idx++) {
      scs_ns_require *req = &result->
                               requires
                                ->items[requires_idx];

      /* attach prefix list comments to the first require with the same id */
      if (req->prefix_list_id != 0) {
        for (int i = 0; i < prefix_list_comments->size; i++) {
          scs_prefix_list_comments *entry = &prefix_list_comments->items[i];
          if (entry->prefix_list_id == req->prefix_list_id && !entry->attached) {
            if (entry->comments_above != NULL) {
              req->comments_above = entry->comments_above;
            }
            if (entry->comment_after.len > 0) {
              req->comment_after = entry->comment_after;
            }
            entry->attached = true;
          }
        }
        req->prefix_list_id = 0;
      }

      sort_symbols(ctx, req->refer);
      sort_symbols(ctx, req->exclude);
      sort_renames(ctx, req->rename);
    }
  }

  if (result->imports != NULL) {
    for (int i = 0; i < result->imports->size; i++) {
      scs_str_list *classes = &result->imports->items[i].classes;
      stable_sort(ctx, classes->items, classes->size, sizeof(scs_str), compare_strings);
    }
    stable_sort(ctx, result->imports->items, result->imports->size, sizeof(scs_ns_import),
                compare_imports);
  }

  /* merge duplicate ns metadata keys: each key is kept once, at the position
   * of its last occurrence, with the value of that last occurrence */
  if (result->ns_metadata != NULL && result->ns_metadata->size > 1) {
    scs_ns_metadata_list *metadata = result->ns_metadata;
    scs_ns_metadata_list reversed = {NULL, 0, 0};
    for (int idx = metadata->size - 1; idx >= 0; idx--) {
      bool already_kept = false;
      for (int j = 0; j < reversed.size; j++) {
        if (str_eq(reversed.items[j].key, metadata->items[idx].key)) {
          already_kept = true;
        }
      }
      if (!already_kept) {
        LIST_PUSH(ctx, &reversed, metadata->items[idx]);
      }
    }
    scs_ns_metadata_list *merged = scs_alloc(ctx, sizeof(scs_ns_metadata_list));
    for (int idx = reversed.size - 1; idx >= 0; idx--) {
      LIST_PUSH(ctx, merged, reversed.items[idx]);
    }
    result->ns_metadata = merged;
  }
}

/* Extracts the ns form from a flat list of nodes into a data structure that
 * format_ns() can print from scratch. Throws for an ns form that cannot be
 * formatted (for example one that uses :use). */
static scs_ns *parse_ns(scs_ctx *ctx, const scs_node_list *nodes_arr) {
  int idx = 0;
  int num_nodes = nodes_arr->size;
  scs_ns *result = scs_alloc(ctx, sizeof(scs_ns));
  result->ns_symbol = EMPTY_STR;
  result->docstring = EMPTY_STR;
  result->refer_clojure_comment_after = EMPTY_STR;
  result->require_comment_after = EMPTY_STR;
  result->import_comment_after = EMPTY_STR;
  result->comment_outside_ns_form = EMPTY_STR;

  bool continue_parsing_ns_form = true;
  int ns_form_ends_line_idx = -1;
  int paren_nesting_depth = 0;
  int line_no = 0;
  bool inside_ns_form = false;
  bool inside_refer_clojure_form = false;
  int refer_clojure_paren_nesting_depth = -1;
  bool inside_require_form = false;
  int require_form_paren_nesting_depth = -1;
  int require_form_line_no = -1;
  bool inside_import_form = false;
  int import_form_line_no = -1;
  bool next_text_node_is_ns_symbol = false;
  bool inside_import_package_list = false;
  bool collect_refer_clojure_exclude_symbols = false;
  bool collect_refer_clojure_only_symbols = false;
  bool collect_refer_clojure_rename_symbols = false;
  bool collect_require_exclude_symbols = false;
  int require_exclude_symbol_paren_depth = -1;
  scs_str_list renames_tmp = {NULL, 0, 0};
  scs_str import_package_list_first_token = EMPTY_STR;
  int ns_node_idx = -1;
  int ns_symbol_idx = -1;
  bool beyond_ns_metadata = false;
  bool inside_ns_metadata_hash_map = false;
  bool inside_ns_metadata_shorthand = false;
  bool next_token_node_is_metadata_true_key = false;
  bool next_text_node_is_metadata_key = false;
  int metadata_value_node_id = -1;
  scs_str tmp_metadata_key = EMPTY_STR;
  int refer_clojure_node_idx = -1;
  int require_node_idx = -1;
  int refer_idx = -1;
  int refer_paren_nesting_depth = -1;
  int import_node_idx = -1;
  int import_node_paren_nesting_depth = -1;
  int active_require_idx = -1;
  int require_symbol_idx = -1;
  bool next_token_is_as_symbol = false;
  scs_str_list *single_line_comments = new_str_list(ctx);
  scs_str active_import_package_name = EMPTY_STR;
  bool prev_node_is_newline = false;
  int line_of_last_comment_recording = -1;
  bool inside_prefix_list = false;
  int prefix_list_paren_nesting_depth = -1;
  scs_str prefix_list_prefix = EMPTY_STR;
  int prefix_list_line_no = -1;
  scs_prefix_list_comments_list prefix_list_comments = {NULL, 0, 0};
  int current_prefix_list_id = 0;
  bool inside_reader_conditional = false;
  scs_str current_reader_conditional_platform = EMPTY_STR;
  int reader_conditional_paren_nesting_depth = -1;
  bool inside_require_list = false;
  int require_list_paren_nesting_depth = -1;
  int refer_macros_idx = -1;
  int refer_macros_paren_nesting_depth = -1;
  bool inside_include_macros = false;
  int active_require_macros_idx = -1;
  bool inside_require_macros_form = false;
  int require_macros_node_idx = -1;
  int require_macros_line_no = -1;
  int require_macros_paren_nesting_depth = -1;
  int require_macros_refer_node_idx = -1;
  int require_macros_as_node_idx = -1;
  int require_macros_rename_idx = -1;
  int gen_class_node_idx = -1;
  bool inside_gen_class = false;
  int gen_class_line_no = -1;
  int gen_class_toggle = 0; /* 0: looking for a :gen-class key, 1: looking for its value */
  int gen_class_key = -1;
  int gen_class_value_line_no = -1;
  bool inside_reader_comment = false;
  int id_of_last_node_inside_reader_comment = -1;
  int rename_idx = -1;
  int rename_paren_nesting_depth = -1;
  int skip_nodes_until_we_reach_this_id = -1;
  scs_ns_section section_to_attach_eol_comments_to = NS_SECTION_NONE;
  bool next_token_is_require_default_symbol = false;
  int num_symbols_inside_list = 0;
  bool inside_gen_class_implements = false;
  int gen_class_implements_paren_depth = -1;
  int gen_class_value_last_node_id = -1;
  scs_str_list *pending_require_metadata = new_str_list(ctx);
  int pending_reader_comment_line_no = -1;

  while (continue_parsing_ns_form) {
    scs_node *node = nodes_arr->items[idx];
    bool current_node_is_newline = is_newline_node(node);
    bool is_token_node2 = is_token_node(node);
    bool is_text_node = node_contains_text(node);
    bool node_has_non_blank_text = is_node_with_non_blank_text(node);
    /* the platform to record on symbols collected from this node */
    scs_str platform = inside_reader_conditional ? current_reader_conditional_platform : EMPTY_STR;

    if (paren_nesting_depth >= 1 && is_token_node2 && node_has_non_blank_text) {
      num_symbols_inside_list++;
    }

    if (paren_nesting_depth == 1 && is_ns_node(node) && num_symbols_inside_list == 1) {
      inside_ns_form = true;
      next_text_node_is_ns_symbol = true;
      ns_node_idx = idx;
    } else if (inside_ns_form && is_refer_clojure_node(node)) {
      inside_refer_clojure_form = true;
      refer_clojure_paren_nesting_depth = paren_nesting_depth;
      section_to_attach_eol_comments_to = NS_SECTION_REFER_CLOJURE;
      refer_clojure_node_idx = idx;
      beyond_ns_metadata = true;
    } else if (inside_ns_form && is_require_node(node)) {
      inside_require_form = true;
      require_form_paren_nesting_depth = paren_nesting_depth;
      require_form_line_no = line_no;
      require_node_idx = idx;
      beyond_ns_metadata = true;
      section_to_attach_eol_comments_to = NS_SECTION_REQUIRE;
    } else if (inside_ns_form && is_import_node(node)) {
      inside_import_form = true;
      import_form_line_no = line_no;
      import_node_idx = idx;
      import_node_paren_nesting_depth = paren_nesting_depth;
      beyond_ns_metadata = true;
      section_to_attach_eol_comments_to = NS_SECTION_IMPORT;
    } else if (inside_ns_form && is_require_macros_keyword(node)) {
      inside_require_macros_form = true;
      require_macros_node_idx = idx;
      require_macros_line_no = line_no;
      require_macros_paren_nesting_depth = paren_nesting_depth;
      beyond_ns_metadata = true;
      section_to_attach_eol_comments_to = NS_SECTION_REQUIRE_MACROS;
    } else if (inside_ns_form && is_gen_class_node(node)) {
      inside_gen_class = true;
      gen_class_node_idx = idx;
      beyond_ns_metadata = true;
      section_to_attach_eol_comments_to = NS_SECTION_GEN_CLASS;
    }

    if (is_paren_opener(node)) {
      paren_nesting_depth++;
      num_symbols_inside_list = 0;

      if (inside_ns_form && is_reader_conditional_opener(node)) {
        inside_reader_conditional = true;
        current_reader_conditional_platform = EMPTY_STR;
        reader_conditional_paren_nesting_depth = paren_nesting_depth;
      } else if (inside_require_form && require_list_paren_nesting_depth == -1) {
        inside_require_list = true;
        require_list_paren_nesting_depth = paren_nesting_depth;
      } else if (inside_import_form && paren_nesting_depth > import_node_paren_nesting_depth) {
        inside_import_package_list = true;
      } else if (inside_gen_class && gen_class_toggle == 1 &&
                 gen_class_key == GEN_CLASS_IMPLEMENTS) {
        inside_gen_class_implements = true;
        gen_class_implements_paren_depth = paren_nesting_depth;
        scs_gen_class_entry *entry = gen_class_entry(ctx, result, GEN_CLASS_IMPLEMENTS);
        if (!entry->has_value) {
          /* the :implements value is a list of symbols; comments above the
           * key are not kept for it */
          entry->has_value = true;
          entry->comments_above = NULL;
          memset(&result->gen_class->implements, 0, sizeof(result->gen_class->implements));
        }
      }
    } else if (is_paren_closer(node)) {
      paren_nesting_depth--;

      /* there is only one ns form per file: stop once it is finished */
      if (inside_ns_form && paren_nesting_depth == 0) {
        inside_ns_form = false;
        ns_form_ends_line_idx = line_no;
      }

      if (inside_import_package_list) {
        inside_import_package_list = false;
        import_package_list_first_token = EMPTY_STR;
      }
      if (inside_require_form && paren_nesting_depth < require_form_paren_nesting_depth) {
        inside_require_form = false;
        require_form_paren_nesting_depth = -1;
      }
      if (inside_refer_clojure_form && paren_nesting_depth < refer_clojure_paren_nesting_depth) {
        inside_refer_clojure_form = false;
        refer_clojure_node_idx = -1;
      }

      if (inside_refer_clojure_form && paren_nesting_depth <= refer_clojure_paren_nesting_depth) {
        collect_refer_clojure_exclude_symbols = false;
        collect_refer_clojure_only_symbols = false;
        collect_refer_clojure_rename_symbols = false;
      }

      /* finished collecting :refer symbols */
      if (refer_idx > 0 && paren_nesting_depth <= refer_paren_nesting_depth) {
        refer_idx = -1;
        refer_paren_nesting_depth = -1;
      }

      /* finished collecting :rename symbols */
      if (rename_idx > 0 && paren_nesting_depth <= rename_paren_nesting_depth) {
        rename_idx = -1;
        rename_paren_nesting_depth = -1;
      }

      /* finished collecting require prefix list symbols */
      if (inside_require_list && paren_nesting_depth < require_list_paren_nesting_depth) {
        inside_require_list = false;
        require_list_paren_nesting_depth = -1;
        next_token_is_require_default_symbol = false;
      }

      if (inside_require_form && require_symbol_idx > 0) {
        require_symbol_idx = -1;
      }

      if (inside_require_form && inside_prefix_list && prefix_list_paren_nesting_depth != -1 &&
          paren_nesting_depth == prefix_list_paren_nesting_depth - 1) {
        inside_prefix_list = false;
        prefix_list_prefix = EMPTY_STR;
        prefix_list_paren_nesting_depth = -1;
      }

      if (inside_reader_conditional &&
          paren_nesting_depth == reader_conditional_paren_nesting_depth - 1) {
        inside_reader_conditional = false;
        current_reader_conditional_platform = EMPTY_STR;
        reader_conditional_paren_nesting_depth = -1;
      }
      if (idx > refer_macros_idx && paren_nesting_depth <= refer_macros_paren_nesting_depth) {
        refer_macros_idx = -1;
        refer_macros_paren_nesting_depth = -1;
      }
      if (inside_import_form && paren_nesting_depth < import_node_paren_nesting_depth) {
        inside_import_form = false;
        import_node_idx = -1;
        import_node_paren_nesting_depth = -1;
      }
      if (inside_require_macros_form && paren_nesting_depth < require_macros_paren_nesting_depth) {
        inside_require_macros_form = false;
        require_macros_paren_nesting_depth = -1;
        require_macros_node_idx = -1;
        require_macros_as_node_idx = -1;
      }
      if (collect_require_exclude_symbols &&
          paren_nesting_depth < require_exclude_symbol_paren_depth) {
        collect_require_exclude_symbols = false;
        require_exclude_symbol_paren_depth = -1;
      }
      if (inside_gen_class_implements && paren_nesting_depth < gen_class_implements_paren_depth) {
        inside_gen_class_implements = false;
        gen_class_implements_paren_depth = -1;
        gen_class_toggle = 0; /* back to looking for a :gen-class key */
        gen_class_value_line_no = line_no;
      }

      require_macros_refer_node_idx = -1;
      require_macros_rename_idx = -1;
    }

    bool is_comment_node2 = is_comment_node(node);
    bool is_reader_comment_node2 = is_reader_comment_node(node);

    /* An end-of-line comment may merge onto a reader comment collected earlier
     * on this same line, but only if no libspec has been parsed in between:
     * parsing a libspec flushes single_line_comments onto that require, which
     * closes the merge window (a comment after the libspec belongs to the
     * libspec as comment_after, not to the reader comment as comments_above). */
    bool reader_comment_merge_window_is_open = inside_require_form &&
                                               pending_reader_comment_line_no == line_no &&
                                               single_line_comments->size > 0;

    if (is_reader_comment_node2) {
      inside_reader_comment = true;
      scs_node *last_node_of_reader_comment = get_last_child_node_with_text(node);
      if (last_node_of_reader_comment == NULL) {
        scs_throw(ctx, "could not parse the ns form");
      }
      id_of_last_node_inside_reader_comment = last_node_of_reader_comment->id;
    }

    if (skip_nodes_until_we_reach_this_id > 0) {
      if (node->id == skip_nodes_until_we_reach_this_id) {
        skip_nodes_until_we_reach_this_id = -1;
        if (node->id == gen_class_value_last_node_id) {
          gen_class_value_line_no = line_no;
          gen_class_value_last_node_id = -1;
        }
      }

      /* collect metadata attached to the next :require libspec. The meta node
       * already groups stacked metadata and its wrapped body, so skip only the
       * metadata children and let the normal require parser consume the body. */
    } else if (inside_require_form && require_symbol_idx == -1 && node_name_is(node, "meta")) {
      scs_str_list metadata = get_metadata_strings_from_meta_node(ctx, node);
      if (pending_require_metadata == NULL) {
        pending_require_metadata = new_str_list(ctx);
      }
      for (int metadata_idx = 0; metadata_idx < metadata.size; metadata_idx++) {
        str_list_push(ctx, pending_require_metadata, metadata.items[metadata_idx]);
      }

      scs_node *body_node = get_body_node_from_meta_node(node);
      if (body_node != NULL) {
        skip_nodes_until_we_reach_this_id = body_node->id;
      }

      /* collect ns metadata shorthand */
    } else if (inside_ns_metadata_shorthand) {
      if (node_name_is(node, ".marker") && node_text_is(node, "^")) {
        next_token_node_is_metadata_true_key = true;
      } else if (next_token_node_is_metadata_true_key && is_token_node2) {
        if (result->ns_metadata == NULL) {
          result->ns_metadata = scs_alloc(ctx, sizeof(scs_ns_metadata_list));
        }
        scs_ns_metadata metadata_itm = {node->text, STR("true")};
        LIST_PUSH(ctx, result->ns_metadata, metadata_itm);

        next_token_node_is_metadata_true_key = false;
        inside_ns_metadata_shorthand = false;
      }

      /* collect ns metadata inside a hash map literal */
    } else if (inside_ns_metadata_hash_map) {
      if (next_text_node_is_metadata_key && node_name_is(node, ".close") &&
          node_text_is(node, "}")) {
        inside_ns_metadata_hash_map = false;
      } else if (!next_text_node_is_metadata_key && node_name_is(node, ".open") &&
                 node_text_is(node, "{")) {
        next_text_node_is_metadata_key = true;
      } else if (next_text_node_is_metadata_key && is_token_node2) {
        if (result->ns_metadata == NULL) {
          result->ns_metadata = scs_alloc(ctx, sizeof(scs_ns_metadata_list));
        }

        tmp_metadata_key = node->text;
        next_text_node_is_metadata_key = false;

        /* the next node should be a whitespace node, then the value for this key */
        scs_node *next_non_whitespace_node = find_next_non_whitespace_node(nodes_arr, idx + 1);
        if (next_non_whitespace_node == NULL) {
          scs_throw(ctx, "could not parse the ns form");
        }
        metadata_value_node_id = next_non_whitespace_node->id;
      } else if (node->id == metadata_value_node_id) {
        scs_ns_metadata metadata_itm = {tmp_metadata_key, get_text_from_root_node(ctx, node)};
        LIST_PUSH(ctx, result->ns_metadata, metadata_itm);

        tmp_metadata_key = EMPTY_STR;
        next_text_node_is_metadata_key = true;
        metadata_value_node_id = -1;

        /* skip the nodes whose text was just collected */
        scs_node *skip_candidate = node;
        while (skip_candidate != NULL) {
          if (skip_candidate->children.size > 0) {
            skip_candidate = node_list_last(&skip_candidate->children);
            skip_nodes_until_we_reach_this_id = skip_candidate->id;
          } else {
            skip_candidate = NULL;
          }
        }
      }

      /* collect ns metadata before we hit the ns symbol */
    } else if (!inside_ns_metadata_hash_map && !inside_ns_metadata_shorthand && inside_ns_form &&
               ns_symbol_idx < 0 && node_name_is(node, "meta")) {
      scs_node *marker_node = find_next_node_with_text(nodes_arr, idx + 1);

      if (node_text_is(marker_node, "^")) {
        scs_node *node_after_marker = find_next_node_with_text(nodes_arr, idx + 2);

        if (node_text_is(node_after_marker, "{")) {
          inside_ns_metadata_hash_map = true;
        } else if (node_after_marker != NULL && is_token_node(node_after_marker)) {
          inside_ns_metadata_shorthand = true;
        }
      }

      /* collect the metadata hash map after the ns symbol */
    } else if (inside_ns_form && idx > ns_node_idx && paren_nesting_depth >= 1 &&
               !beyond_ns_metadata && !inside_reader_comment && !inside_ns_metadata_shorthand &&
               !inside_ns_metadata_hash_map && node_name_is(node, ".open") &&
               node_text_is(node, "{")) {
      inside_ns_metadata_hash_map = true;
      next_text_node_is_metadata_key = true;

      /* collect the ns symbol */
    } else if (idx > ns_node_idx && next_text_node_is_ns_symbol && is_token_node2 && is_text_node) {
      result->ns_symbol = node->text;
      ns_symbol_idx = idx;
      next_text_node_is_ns_symbol = false;

      /* collect the reader conditional platform keyword */
    } else if (inside_reader_conditional &&
               paren_nesting_depth == reader_conditional_paren_nesting_depth &&
               is_keyword_node(node)) {
      current_reader_conditional_platform = node->text;

      /* collect single-line comments */
    } else if (inside_ns_form && idx > ns_node_idx && prev_node_is_newline && is_comment_node2) {
      str_list_push(ctx, single_line_comments, node->text);

      /* collect a reader comment that appears inline after the (:require keyword,
       * before any libspec (on the same line as the start of the :require form) */
    } else if (inside_require_form && active_require_idx < 0 && require_form_line_no == line_no &&
               require_symbol_idx == -1 && refer_idx == -1 && rename_idx == -1 &&
               is_reader_comment_node2) {
      str_list_push(ctx, single_line_comments, get_text_from_root_node(ctx, node));
      pending_reader_comment_line_no = line_no;

      /* merge this comment onto the pending reader comment so they travel together */
    } else if (reader_comment_merge_window_is_open && is_comment_node2) {
      int last_comment_idx = single_line_comments->size - 1;
      single_line_comments->items[last_comment_idx] =
          str_concat3(ctx, single_line_comments->items[last_comment_idx], STR(" "), node->text);
      /* a comment on a later line cannot merge onto this reader comment */
      pending_reader_comment_line_no = -1;

      /* collect a reader comment that starts its own line */
    } else if (inside_ns_form && idx > ns_node_idx && prev_node_is_newline &&
               is_reader_comment_node2) {
      str_list_push(ctx, single_line_comments, get_text_from_root_node(ctx, node));
      /* record the line number so that an end-of-line comment following this
       * reader comment can be merged onto it by the branch above */
      pending_reader_comment_line_no = line_no;

      /* collect comments at the end of a line */
    } else if (idx > ns_node_idx && !prev_node_is_newline &&
               (is_comment_node2 || is_reader_comment_node2)) {
      scs_str comment_at_end_of_line =
          is_comment_node2 ? node->text : get_text_from_root_node(ctx, node);

      if (prefix_list_line_no == line_no) {
        scs_prefix_list_comments *entry =
            prefix_list_comments_for(ctx, &prefix_list_comments, current_prefix_list_id);
        entry->comment_after = comment_at_end_of_line;
        line_of_last_comment_recording = line_no;
      } else if (require_form_line_no == line_no && active_require_idx < 0) {
        result->require_comment_after = comment_at_end_of_line;
        line_of_last_comment_recording = line_no;
      } else if (require_form_line_no == line_no && active_require_idx >= 0) {
        require_at(ctx, result->requires, active_require_idx)->comment_after =
            comment_at_end_of_line;
        line_of_last_comment_recording = line_no;
      } else if (section_to_attach_eol_comments_to == NS_SECTION_REFER_CLOJURE &&
                 result->refer_clojure != NULL) {
        result->refer_clojure_comment_after = comment_at_end_of_line;
        line_of_last_comment_recording = line_no;
      } else if (import_form_line_no == line_no && result->imports == NULL) {
        result->import_comment_after = comment_at_end_of_line;
        line_of_last_comment_recording = line_no;
      } else if (import_form_line_no == line_no) {
        active_import(ctx, result, active_import_package_name)->comment_after =
            comment_at_end_of_line;
        line_of_last_comment_recording = line_no;
      } else if (require_macros_line_no == line_no) {
        require_at(ctx, result->require_macros, active_require_macros_idx)->comment_after =
            comment_at_end_of_line;
        line_of_last_comment_recording = line_no;
      } else if (gen_class_line_no == line_no) {
        if (result->gen_class == NULL) {
          scs_throw(ctx, "could not parse the ns form");
        }
        result->gen_class->comment_after = comment_at_end_of_line;
        line_of_last_comment_recording = line_no;
      } else if (gen_class_value_line_no == line_no) {
        gen_class_entry(ctx, result, gen_class_key)->comment_after = comment_at_end_of_line;
        line_of_last_comment_recording = line_no;
      }

      if (!inside_ns_form && line_no == line_of_last_comment_recording) {
        result->comment_outside_ns_form = comment_at_end_of_line;
      }

      /* discard nodes that are inside a reader comment */
    } else if (inside_reader_comment) {
      if (node->id == id_of_last_node_inside_reader_comment) {
        inside_reader_comment = false;
        id_of_last_node_inside_reader_comment = -1;
      }

      /* attach comments to the :require form */
    } else if (inside_require_form && idx == require_node_idx && single_line_comments->size > 0) {
      result->require_comments_above = single_line_comments;
      single_line_comments = new_str_list(ctx);

      /* attach comments to the :import form */
    } else if (inside_import_form && idx == import_node_idx && single_line_comments->size > 0) {
      result->import_comments_above = single_line_comments;
      single_line_comments = new_str_list(ctx);

      /* attach comments to the :refer-clojure form */
    } else if (inside_refer_clojure_form && idx == refer_clojure_node_idx &&
               single_line_comments->size > 0) {
      result->refer_clojure_comments_above = single_line_comments;
      single_line_comments = new_str_list(ctx);

      /* collect the docstring */
    } else if (inside_ns_form && idx > ns_node_idx && paren_nesting_depth == 1 &&
               !beyond_ns_metadata && !inside_ns_metadata_shorthand &&
               !inside_ns_metadata_hash_map && is_string_node(node)) {
      result->has_docstring = true;
      result->docstring = get_text_from_string_node(node);

      /* collect :refer-clojure :exclude */
    } else if (inside_refer_clojure_form && idx > refer_clojure_node_idx &&
               is_exclude_keyword(node)) {
      if (result->refer_clojure == NULL) {
        result->refer_clojure = scs_alloc(ctx, sizeof(scs_ns_refer_clojure));
      }
      if (result->refer_clojure->exclude == NULL) {
        result->refer_clojure->exclude = new_symbol_list(ctx);
      }
      collect_refer_clojure_exclude_symbols = true;

      /* collect :refer-clojure :exclude symbols */
    } else if (idx > refer_clojure_node_idx + 1 && collect_refer_clojure_exclude_symbols &&
               paren_nesting_depth >= 3 && is_token_node2 && is_text_node &&
               result->refer_clojure != NULL && result->refer_clojure->exclude != NULL) {
      LIST_PUSH(ctx, result->refer_clojure->exclude, make_ns_symbol(node->text, platform));

      /* collect :refer-clojure :only */
    } else if (inside_refer_clojure_form && idx > refer_clojure_node_idx && is_only_keyword(node)) {
      if (result->refer_clojure == NULL) {
        result->refer_clojure = scs_alloc(ctx, sizeof(scs_ns_refer_clojure));
      }
      result->refer_clojure->only = new_symbol_list(ctx);
      collect_refer_clojure_only_symbols = true;

      /* collect :refer-clojure :only symbols */
    } else if (idx > refer_clojure_node_idx + 1 && collect_refer_clojure_only_symbols &&
               paren_nesting_depth >= 3 && is_token_node2 && is_text_node &&
               result->refer_clojure != NULL && result->refer_clojure->only != NULL) {
      LIST_PUSH(ctx, result->refer_clojure->only, make_ns_symbol(node->text, platform));

      /* collect :refer-clojure :rename */
    } else if (inside_refer_clojure_form && idx > refer_clojure_node_idx &&
               is_rename_keyword(node)) {
      if (result->refer_clojure == NULL) {
        result->refer_clojure = scs_alloc(ctx, sizeof(scs_ns_refer_clojure));
      }
      result->refer_clojure->rename = new_rename_list(ctx);
      collect_refer_clojure_rename_symbols = true;

      /* collect :refer-clojure :rename symbols */
    } else if (idx > refer_clojure_node_idx + 1 && collect_refer_clojure_rename_symbols &&
               paren_nesting_depth >= 3 && is_token_node2 && is_text_node &&
               result->refer_clojure != NULL && result->refer_clojure->rename != NULL) {
      str_list_push(ctx, &renames_tmp, node->text);
      if (renames_tmp.size == 2) {
        LIST_PUSH(ctx, result->refer_clojure->rename,
                  make_ns_rename(renames_tmp.items[0], renames_tmp.items[1], platform));
        renames_tmp.size = 0;
      }

      /* is this :require :as ? */
    } else if (idx > require_node_idx && inside_require_form && is_token_node2 &&
               is_as_keyword(node)) {
      next_token_is_as_symbol = true;

      /* collect the require :as symbol */
    } else if (idx > require_node_idx && inside_require_form && next_token_is_as_symbol &&
               is_token_node2 && is_text_node) {
      next_token_is_as_symbol = false;
      require_at(ctx, result->requires, active_require_idx)->as = node->text;

      /* collect :require-macros :refer symbols */
    } else if (inside_require_macros_form && require_macros_refer_node_idx != -1 &&
               idx > require_macros_refer_node_idx && is_token_node2 && is_text_node) {
      scs_ns_require *rm = require_at(ctx, result->require_macros, active_require_macros_idx);
      if (rm->refer == NULL) {
        rm->refer = new_symbol_list(ctx);
      }
      LIST_PUSH(ctx, rm->refer, make_ns_symbol(node->text, platform));

      /* collect :require-macros :as symbol */
    } else if (inside_require_macros_form && require_macros_as_node_idx != -1 &&
               idx > require_macros_as_node_idx && is_token_node2 && is_text_node) {
      require_at(ctx, result->require_macros, active_require_macros_idx)->as = node->text;
      require_macros_as_node_idx = -1;

      /* collect :require-macros :rename */
    } else if (inside_require_macros_form && require_macros_rename_idx != -1 &&
               idx > require_macros_rename_idx && is_token_node2 && is_text_node) {
      scs_ns_require *rm = require_at(ctx, result->require_macros, active_require_macros_idx);
      if (rm->rename == NULL) {
        rm->rename = new_rename_list(ctx);
      }
      str_list_push(ctx, &renames_tmp, node->text);
      if (renames_tmp.size == 2) {
        LIST_PUSH(ctx, rm->rename,
                  make_ns_rename(renames_tmp.items[0], renames_tmp.items[1], platform));
        renames_tmp.size = 0;
      }

      /* :require-macros :refer */
    } else if (inside_require_macros_form && idx > require_macros_node_idx &&
               is_refer_keyword(node)) {
      require_macros_refer_node_idx = idx;

      /* :require-macros :as */
    } else if (inside_require_macros_form && idx > require_macros_node_idx && is_as_keyword(node)) {
      require_macros_as_node_idx = idx;

      /* :require-macros :rename */
    } else if (inside_require_macros_form && idx > require_macros_node_idx &&
               is_rename_keyword(node)) {
      require_macros_rename_idx = idx;
      renames_tmp.size = 0;

      /* collect :require-macros symbol */
    } else if (inside_require_macros_form && idx > require_macros_node_idx && is_token_node2 &&
               is_text_node) {
      if (result->require_macros == NULL) {
        result->require_macros = new_require_list(ctx);
        /* add comments above the :require-macros form if possible */
        attach_comments_above(&result->require_macros_comments_above, &single_line_comments);
        if (single_line_comments == NULL) {
          single_line_comments = new_str_list(ctx);
        }
      }

      scs_str_list *no_metadata = NULL;
      scs_ns_require req_obj = make_require(node->text, &no_metadata);
      req_obj.platform = platform;
      LIST_PUSH(ctx, result->require_macros, req_obj);
      active_require_macros_idx++;
      require_macros_line_no = line_no;

      /* store the comments above this line */
      attach_comments_above(
          &require_at(ctx, result->require_macros, active_require_macros_idx)->comments_above,
          &single_line_comments);
      if (single_line_comments == NULL) {
        single_line_comments = new_str_list(ctx);
      }

      /* is this :include-macros ? */
    } else if (idx > require_node_idx && inside_require_form && is_token_node2 &&
               is_include_macros_node(node)) {
      inside_include_macros = true;

      /* collect :include-macros boolean */
    } else if (inside_include_macros && is_token_node2 && is_boolean_node(node)) {
      scs_ns_require *req = require_at(ctx, result->requires, active_require_idx);
      req->has_include_macros = true;
      req->include_macros = node_text_is(node, "true");
      inside_include_macros = false;

      /* is this :refer-macros ? */
    } else if (idx > require_node_idx && inside_require_form && is_token_node2 &&
               is_refer_macros_keyword(node)) {
      refer_macros_idx = idx;
      refer_macros_paren_nesting_depth = paren_nesting_depth;

      /* collect :refer-macros symbols */
    } else if (idx > refer_macros_idx && inside_require_form &&
               paren_nesting_depth == refer_macros_paren_nesting_depth + 1 && is_token_node2 &&
               is_text_node) {
      scs_ns_require *req = require_at(ctx, result->requires, active_require_idx);
      if (req->refer_macros == NULL) {
        req->refer_macros = new_str_list(ctx);
      }
      str_list_push(ctx, req->refer_macros, node->text);

      /* is this :require :refer ? */
    } else if (idx > require_node_idx && inside_require_form && is_token_node2 &&
               is_refer_keyword(node)) {
      refer_idx = idx;
      refer_paren_nesting_depth = paren_nesting_depth;

      /* is this :require :default ? */
    } else if (idx > require_node_idx && inside_require_form && is_token_node2 &&
               is_default_keyword(node)) {
      next_token_is_require_default_symbol = true;

      /* collect :require :exclude symbols */
    } else if (idx > require_node_idx && inside_require_form && is_token_node2 &&
               collect_require_exclude_symbols &&
               paren_nesting_depth > require_exclude_symbol_paren_depth) {
      scs_ns_require *req = require_at(ctx, result->requires, active_require_idx);
      if (req->exclude == NULL) {
        scs_throw(ctx, "could not parse the ns form");
      }
      LIST_PUSH(ctx, req->exclude, make_ns_symbol(node->text, EMPTY_STR));

      /* is this :require :exclude ? */
    } else if (idx > require_node_idx && inside_require_form && is_token_node2 &&
               is_exclude_keyword(node)) {
      require_at(ctx, result->requires, active_require_idx)->exclude = new_symbol_list(ctx);
      collect_require_exclude_symbols = true;
      require_exclude_symbol_paren_depth = paren_nesting_depth;

      /* :require :as-alias */
    } else if (idx > require_node_idx && inside_require_form && is_token_node2 &&
               is_as_alias_keyword(node)) {
      scs_node *next_symbol = find_next_token_inside_require_form(nodes_arr, idx + 1);
      if (next_symbol == NULL) {
        scs_throw(ctx, "could not parse the ns form");
      }
      require_at(ctx, result->requires, active_require_idx)->as_alias = next_symbol->text;

      /* collect :refer :all */
    } else if (idx > refer_idx && inside_require_form && is_token_node2 && is_all_node(node)) {
      require_at(ctx, result->requires, active_require_idx)->refer_all = true;

      /* collect :refer :default symbol */
    } else if (idx > refer_idx && inside_require_form && is_token_node2 &&
               next_token_is_require_default_symbol) {
      require_at(ctx, result->requires, active_require_idx)->default_symbol = node->text;
      next_token_is_require_default_symbol = false;

      /* :rename keyword inside a :require list */
    } else if (inside_require_form && inside_require_list && rename_idx == -1 &&
               is_rename_keyword(node)) {
      rename_idx = idx;
      rename_paren_nesting_depth = paren_nesting_depth;
      renames_tmp.size = 0;

      /* collect :require renames */
    } else if (inside_require_form && inside_require_list && rename_idx > 0 && idx > rename_idx &&
               paren_nesting_depth > rename_paren_nesting_depth && is_token_node2 && is_text_node) {
      str_list_push(ctx, &renames_tmp, node->text);
      if (renames_tmp.size == 2) {
        scs_ns_require *req = require_at(ctx, result->requires, active_require_idx);
        if (req->rename == NULL) {
          req->rename = new_rename_list(ctx);
        }
        LIST_PUSH(ctx, req->rename,
                  make_ns_rename(renames_tmp.items[0], renames_tmp.items[1], platform));
        renames_tmp.size = 0;
      }

      /* collect :require :refer symbols */
    } else if (idx > refer_idx && inside_require_form && refer_paren_nesting_depth != -1 &&
               paren_nesting_depth > refer_paren_nesting_depth && is_token_node2 && is_text_node) {
      scs_ns_require *req = require_at(ctx, result->requires, active_require_idx);
      if (req->refer == NULL) {
        req->refer = new_symbol_list(ctx);
      }
      LIST_PUSH(ctx, req->refer, make_ns_symbol(node->text, EMPTY_STR));

      /* collect :require symbol not inside of a list / vector */
    } else if (inside_require_form && !inside_require_list && idx > require_node_idx &&
               is_token_node2 && is_text_node && require_symbol_idx == -1 &&
               !is_keyword_node(node)) {
      if (result->requires == NULL) {
        result->
          requires
        = new_require_list(ctx);
      }

      scs_ns_require require_obj = make_require(node->text, &pending_require_metadata);
      require_obj.platform = platform;
      LIST_PUSH(ctx, result->requires, require_obj);
      active_require_idx++;
      require_form_line_no = line_no;

      /* attach comments from the lines above this require */
      attach_comments_above(&require_at(ctx, result->requires, active_require_idx)->comments_above,
                            &single_line_comments);
      if (single_line_comments == NULL) {
        single_line_comments = new_str_list(ctx);
      }

      /* collect symbols inside of a prefix list */
    } else if (inside_prefix_list && is_token_node2 && is_text_node) {
      if (result->requires == NULL) {
        result->
          requires
        = new_require_list(ctx);
      }

      scs_str namespace = str_concat3(ctx, prefix_list_prefix, STR("."), node->text);
      scs_ns_require require_obj = make_require(namespace, &pending_require_metadata);
      require_obj.prefix_list_id = current_prefix_list_id;
      LIST_PUSH(ctx, result->requires, require_obj);
      active_require_idx++;
      require_symbol_idx = idx;
      require_form_line_no = line_no;

      /* collect :require symbol inside of a list / vector */
    } else if (inside_require_form && inside_require_list && idx > require_node_idx &&
               refer_idx == -1 && rename_idx == -1 && is_token_node2 && is_text_node &&
               require_symbol_idx == -1 && !is_keyword_node(node)) {
      if (result->requires == NULL) {
        result->
          requires
        = new_require_list(ctx);
      }

      /* five possibilities for a :require import:
       * - require symbol not inside of list / vector
       * - require symbol inside a list / vector, followed by nothing
       * - require symbol for a prefix list (the following symbols tell us this)
       * - require symbol followed by :as
       * - require symbol followed by :refer */
      scs_node *next_token_inside_require_form =
          find_next_token_inside_require_form(nodes_arr, idx + 1);
      bool is_prefix_list = next_token_inside_require_form != NULL &&
                            !is_keyword_node(next_token_inside_require_form);

      if (is_prefix_list) {
        int prefix_list_id = create_id(ctx);
        inside_prefix_list = true;
        prefix_list_paren_nesting_depth = paren_nesting_depth;
        prefix_list_line_no = line_no;
        prefix_list_prefix = node->text;
        current_prefix_list_id = prefix_list_id;

        /* store the comments above this line; they are attached to the first
         * ns imported by this prefix list later */
        if (single_line_comments->size > 0) {
          scs_prefix_list_comments *entry =
              prefix_list_comments_for(ctx, &prefix_list_comments, prefix_list_id);
          entry->comments_above = single_line_comments;
          single_line_comments = new_str_list(ctx);
        }
      } else {
        scs_ns_require require_obj = make_require(node->text, &pending_require_metadata);
        require_obj.platform = platform;
        LIST_PUSH(ctx, result->requires, require_obj);
        active_require_idx++;
        require_symbol_idx = idx;
        require_form_line_no = line_no;
        inside_prefix_list = false;
        prefix_list_line_no = -1;

        /* attach comments from the lines above this require */
        attach_comments_above(
            &require_at(ctx, result->requires, active_require_idx)->comments_above,
            &single_line_comments);
        if (single_line_comments == NULL) {
          single_line_comments = new_str_list(ctx);
        }
      }

      /* collect require Strings in ClojureScript */
    } else if (inside_require_form && inside_require_list && idx > require_node_idx &&
               is_string_node(node)) {
      if (result->requires == NULL) {
        result->
          requires
        = new_require_list(ctx);
      }

      scs_str symbol = str_concat3(ctx, STR("\""), get_text_from_string_node(node), STR("\""));
      scs_ns_require require_obj = make_require(symbol, &pending_require_metadata);
      require_obj.symbol_is_string = true;
      require_obj.platform = platform;
      LIST_PUSH(ctx, result->requires, require_obj);
      active_require_idx++;
      require_form_line_no = line_no;

      /* attach comments from the lines above this require */
      attach_comments_above(&require_at(ctx, result->requires, active_require_idx)->comments_above,
                            &single_line_comments);
      if (single_line_comments == NULL) {
        single_line_comments = new_str_list(ctx);
      }

      /* collect :import packages not inside of a list or vector */
    } else if (inside_import_form && idx > import_node_idx && !inside_import_package_list &&
               is_token_node2 && is_text_node) {
      scs_java_package package_parsed = parse_java_package_with_class(ctx, node->text);
      scs_ns_import *import = import_for_package(ctx, result, package_parsed.package);

      /* a package without a class (not valid Clojure) records no class */
      if (package_parsed.class_name.len > 0) {
        str_list_push(ctx, &import->classes, package_parsed.class_name);
      }
      active_import_package_name = package_parsed.package;
      import_form_line_no = line_no;

      attach_comments_above(&import->comments_above, &single_line_comments);
      if (single_line_comments == NULL) {
        single_line_comments = new_str_list(ctx);
      }
      if (platform.len > 0) {
        import->platform = platform;
      }

      /* collect :import classes inside of a list or vector */
    } else if (inside_import_package_list && is_token_node2 && is_text_node) {
      if (import_package_list_first_token.len == 0) {
        scs_str package_name = node->text;
        import_package_list_first_token = package_name;
        active_import_package_name = package_name;
        import_form_line_no = line_no;

        scs_ns_import *import = import_for_package(ctx, result, package_name);
        attach_comments_above(&import->comments_above, &single_line_comments);
        if (single_line_comments == NULL) {
          single_line_comments = new_str_list(ctx);
        }
        if (platform.len > 0) {
          import->platform = platform;
        }
      } else {
        scs_ns_import *import = active_import(ctx, result, import_package_list_first_token);
        str_list_push(ctx, &import->classes, node->text);
      }

      /* collect gen-class :implements symbols */
    } else if (inside_gen_class && inside_gen_class_implements && idx > gen_class_node_idx &&
               gen_class_toggle == 1 && gen_class_key == GEN_CLASS_IMPLEMENTS &&
               (is_token_node2 || is_string_node(node) || node_name_is(node, "meta"))) {
      scs_str symbol = get_gen_class_symbol_text(ctx, node);
      if (symbol.len == 0) {
        scs_throw(ctx, ":gen-class :implements values must be symbols or strings.");
      }
      if (result->gen_class == NULL) {
        scs_throw(ctx, "could not parse the ns form");
      }
      LIST_PUSH(ctx, &result->gen_class->implements, make_ns_symbol(symbol, platform));

      if (!is_token_node2) {
        scs_node *last_node = get_last_child_node_with_text(node);
        if (last_node != NULL) {
          skip_nodes_until_we_reach_this_id = last_node->id;
        }
      }

      /* we are on the :gen-class node */
    } else if (inside_gen_class && idx == gen_class_node_idx) {
      result->gen_class = scs_alloc(ctx, sizeof(scs_ns_gen_class));
      result->gen_class->is_empty = true;
      result->gen_class->platform = platform;
      result->gen_class->comment_after = EMPTY_STR;
      for (int key = 0; key < GEN_CLASS_NUM_KEYS; key++) {
        result->gen_class->entries[key].value = EMPTY_STR;
        result->gen_class->entries[key].metadata = EMPTY_STR;
        result->gen_class->entries[key].comment_after = EMPTY_STR;
      }

      attach_comments_above(&result->gen_class->comments_above, &single_line_comments);
      if (single_line_comments == NULL) {
        single_line_comments = new_str_list(ctx);
      }
      gen_class_line_no = line_no;

      /* a :gen-class key like :main, :name, :state, :init, etc */
    } else if (inside_gen_class && idx > gen_class_node_idx && is_text_node &&
               gen_class_toggle == 0 && is_gen_class_keyword(node)) {
      if (result->gen_class == NULL) {
        scs_throw(ctx, "could not parse the ns form");
      }
      result->gen_class->is_empty = false;

      gen_class_key = gen_class_key_from_keyword(node->text);
      scs_gen_class_entry *entry = &result->gen_class->entries[gen_class_key];
      memset(entry, 0, sizeof(*entry));
      entry->present = true;
      entry->value = EMPTY_STR;
      entry->metadata = EMPTY_STR;
      entry->comment_after = EMPTY_STR;

      attach_comments_above(&entry->comments_above, &single_line_comments);
      if (single_line_comments == NULL) {
        single_line_comments = new_str_list(ctx);
      }

      gen_class_toggle = 1;

      /* :gen-class :prefix value */
    } else if (inside_gen_class && idx > gen_class_node_idx && gen_class_toggle == 1 &&
               gen_class_key == GEN_CLASS_PREFIX && is_string_node(node)) {
      scs_gen_class_entry *entry = gen_class_entry(ctx, result, gen_class_key);
      entry->has_value = true;
      entry->value = get_gen_class_symbol_text(ctx, node);
      gen_class_toggle = 0;

      scs_node *last_node = get_last_child_node_with_text(node);
      if (last_node != NULL) {
        skip_nodes_until_we_reach_this_id = last_node->id;
        gen_class_value_last_node_id = last_node->id;
      }

      /* :gen-class name-like value with metadata
       * (:name is the only key where metadata is meaningful to gen-class, as
       * class annotations, but metadata is legal reader syntax on any of these
       * values and must be preserved, not dropped) */
    } else if (inside_gen_class && idx > gen_class_node_idx && gen_class_toggle == 1 &&
               is_gen_class_name_key(gen_class_key) && node_name_is(node, "meta")) {
      scs_gen_class_metadata_form value_form = split_gen_class_metadata_form(ctx, node);
      scs_str value = get_gen_class_symbol_text(ctx, value_form.body_node);
      if (value.len == 0) {
        scs_str msg =
            str_concat3(ctx, STR(":gen-class :"), str_from_cstr(gen_class_key_names[gen_class_key]),
                        STR(" must be a symbol or string."));
        scs_throw(ctx, msg.data);
      }

      scs_gen_class_entry *entry = gen_class_entry(ctx, result, gen_class_key);
      entry->metadata = value_form.metadata;
      entry->has_value = true;
      entry->value = value;
      gen_class_toggle = 0;

      scs_node *last_node = get_last_child_node_with_text(node);
      if (last_node != NULL) {
        skip_nodes_until_we_reach_this_id = last_node->id;
        gen_class_value_last_node_id = last_node->id;
      }

      /* other :gen-class name-like values */
    } else if (inside_gen_class && idx > gen_class_node_idx && gen_class_toggle == 1 &&
               is_gen_class_name_key(gen_class_key) && (is_token_node2 || is_string_node(node))) {
      scs_gen_class_entry *entry = gen_class_entry(ctx, result, gen_class_key);
      entry->has_value = true;
      entry->value = get_gen_class_symbol_text(ctx, node);
      gen_class_toggle = 0;

      if (is_string_node(node)) {
        scs_node *last_node = get_last_child_node_with_text(node);
        if (last_node != NULL) {
          skip_nodes_until_we_reach_this_id = last_node->id;
          gen_class_value_last_node_id = last_node->id;
        }
      } else {
        gen_class_value_line_no = line_no;
      }

      /* :gen-class :constructors value */
    } else if (inside_gen_class && idx > gen_class_node_idx && gen_class_toggle == 1 &&
               gen_class_key == GEN_CLASS_CONSTRUCTORS && is_map_literal_node(node)) {
      scs_gen_class_entry *entry = gen_class_entry(ctx, result, gen_class_key);
      result->gen_class->constructors = parse_gen_class_constructors(ctx, node);
      entry->has_value = true;
      gen_class_toggle = 0;

      scs_node *last_node = get_last_child_node_with_text(node);
      if (last_node != NULL) {
        skip_nodes_until_we_reach_this_id = last_node->id;
        gen_class_value_last_node_id = last_node->id;
      }

      /* :gen-class :methods value */
    } else if (inside_gen_class && idx > gen_class_node_idx && gen_class_toggle == 1 &&
               gen_class_key == GEN_CLASS_METHODS && is_vector_literal_node(node)) {
      scs_gen_class_entry *entry = gen_class_entry(ctx, result, gen_class_key);
      result->gen_class->methods = parse_gen_class_methods(ctx, node);
      entry->has_value = true;
      gen_class_toggle = 0;

      scs_node *last_node = get_last_child_node_with_text(node);
      if (last_node != NULL) {
        skip_nodes_until_we_reach_this_id = last_node->id;
        gen_class_value_last_node_id = last_node->id;
      }

      /* invalid :gen-class :methods value */
    } else if (inside_gen_class && idx > gen_class_node_idx && gen_class_toggle == 1 &&
               gen_class_key == GEN_CLASS_METHODS && node_contains_text_and_not_whitespace(node)) {
      scs_throw(ctx, ":gen-class :methods must be a vector of method vectors.");

      /* :gen-class :exposes value */
    } else if (inside_gen_class && idx > gen_class_node_idx && gen_class_toggle == 1 &&
               gen_class_key == GEN_CLASS_EXPOSES && is_map_literal_node(node)) {
      scs_gen_class_entry *entry = gen_class_entry(ctx, result, gen_class_key);
      result->gen_class->exposes = parse_gen_class_exposes(ctx, node);
      entry->has_value = true;
      gen_class_toggle = 0;

      scs_node *last_node = get_last_child_node_with_text(node);
      if (last_node != NULL) {
        skip_nodes_until_we_reach_this_id = last_node->id;
        gen_class_value_last_node_id = last_node->id;
      }

      /* invalid :gen-class :exposes value */
    } else if (inside_gen_class && idx > gen_class_node_idx && gen_class_toggle == 1 &&
               gen_class_key == GEN_CLASS_EXPOSES && node_contains_text_and_not_whitespace(node)) {
      scs_throw(ctx,
                ":gen-class :exposes must be a map of protected-field names to accessor maps.");

      /* :gen-class :exposes-methods value */
    } else if (inside_gen_class && idx > gen_class_node_idx && gen_class_toggle == 1 &&
               gen_class_key == GEN_CLASS_EXPOSES_METHODS && is_map_literal_node(node)) {
      scs_gen_class_entry *entry = gen_class_entry(ctx, result, gen_class_key);
      result->gen_class->exposes_methods = parse_gen_class_exposes_methods(ctx, node);
      entry->has_value = true;
      gen_class_toggle = 0;

      scs_node *last_node = get_last_child_node_with_text(node);
      if (last_node != NULL) {
        skip_nodes_until_we_reach_this_id = last_node->id;
        gen_class_value_last_node_id = last_node->id;
      }

      /* invalid :gen-class :exposes-methods value */
    } else if (inside_gen_class && idx > gen_class_node_idx && gen_class_toggle == 1 &&
               gen_class_key == GEN_CLASS_EXPOSES_METHODS &&
               node_contains_text_and_not_whitespace(node)) {
      scs_throw(ctx, ":gen-class :exposes-methods must be a map of superclass-method names to "
                     "exposed names.");

      /* :main, :load-impl-ns */
    } else if (inside_gen_class && idx > gen_class_node_idx && is_text_node && is_token_node2 &&
               gen_class_toggle == 1 && is_gen_class_boolean_key(gen_class_key)) {
      if (node_text_is(node, "true") || node_text_is(node, "false")) {
        scs_gen_class_entry *entry = gen_class_entry(ctx, result, gen_class_key);
        entry->has_value = true;
        entry->bool_value = node_text_is(node, "true");
        gen_class_toggle = 0;
        gen_class_value_line_no = line_no;
      }
      /* any other value is almost certainly an error in the source; it is left alone */

      /* :use is not supported */
    } else if (inside_ns_form && is_token_node2 && paren_nesting_depth >= 1 && is_use_node(node)) {
      scs_throw(ctx, "Standard Clojure Style does not support :use inside of the ns form. Please "
                     "refactor with :require as appropriate.");
    }

    /* increment the line_no for the next node if we are on a newline node
     * (this does not account for newlines inside of multi-line strings, which
     * does not matter for the purposes of ns parsing) */
    if (current_node_is_newline) {
      line_no++;
    }
    prev_node_is_newline = current_node_is_newline;

    idx++;

    /* exit if we are at the end of the nodes */
    if (idx >= num_nodes) {
      continue_parsing_ns_form = false;
      /* exit if we have finished parsing the ns form */
    } else if (ns_node_idx > 0 && !inside_ns_form && line_no >= ns_form_ends_line_idx + 2) {
      continue_parsing_ns_form = false;
    }
  }

  sort_ns_result(ctx, result, &prefix_list_comments);
  return result;
}

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

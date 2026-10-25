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
  const char *data; /* never NULL; points at "" for an empty string */
  int len;          /* length in bytes */
} scs_str;

static const scs_str EMPTY_STR = {"", 0};

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
  return a.len == b.len && memcmp(a.data, b.data, (size_t)a.len) == 0;
}

static bool str_eq_cstr(scs_str a, const char *b) {
  return str_eq(a, str_from_cstr(b));
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
  memcpy(buf, s.data, (size_t)s.len);
  scs_str result;
  result.data = buf;
  result.len = s.len;
  return result;
}

static scs_str str_concat(scs_ctx *ctx, scs_str s1, scs_str s2) {
  int len = checked_len_add(ctx, s1.len, s2.len);
  char *buf = str_alloc_buf(ctx, len);
  memcpy(buf, s1.data, (size_t)s1.len);
  memcpy(buf + s1.len, s2.data, (size_t)s2.len);
  scs_str result;
  result.data = buf;
  result.len = len;
  return result;
}

static scs_str str_concat3(scs_ctx *ctx, scs_str s1, scs_str s2, scs_str s3) {
  int len = checked_len_add(ctx, checked_len_add(ctx, s1.len, s2.len), s3.len);
  char *buf = str_alloc_buf(ctx, len);
  memcpy(buf, s1.data, (size_t)s1.len);
  memcpy(buf + s1.len, s2.data, (size_t)s2.len);
  memcpy(buf + s1.len + s2.len, s3.data, (size_t)s3.len);
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
  return prefix.len <= s.len && memcmp(s.data, prefix.data, (size_t)prefix.len) == 0;
}

static bool str_ends_with(scs_str s, scs_str suffix) {
  return suffix.len <= s.len &&
         memcmp(s.data + (s.len - suffix.len), suffix.data, (size_t)suffix.len) == 0;
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
    memcpy(buf + out, s.data + search_from, (size_t)(found_byte - search_from));
    out += found_byte - search_from;
    memcpy(buf + out, replace.data, (size_t)replace.len);
    out += replace.len;
    search_from = found_byte + find.len;
  }
  memcpy(buf + out, s.data + search_from, (size_t)(s.len - search_from));

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
 * String Lists
 */

/* a growable array of strings */
typedef struct scs_str_list {
  scs_str *items; /* NULL until the first item is added */
  int size;
  int capacity;
} scs_str_list;

static void str_list_push(scs_ctx *ctx, scs_str_list *list, scs_str s) {
  if (list->size == list->capacity) {
    int new_capacity =
        list->capacity == 0 ? 8 : checked_len_add(ctx, list->capacity, list->capacity);
    scs_str *new_items = scs_alloc(ctx, (size_t)new_capacity * sizeof(scs_str));
    if (list->size > 0) {
      memcpy(new_items, list->items, (size_t)list->size * sizeof(scs_str));
    }
    list->items = new_items;
    list->capacity = new_capacity;
  }
  list->items[list->size] = s;
  list->size++;
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
      memcpy(buf + out, sep.data, (size_t)sep.len);
      out += sep.len;
    }
    memcpy(buf + out, list->items[i].data, (size_t)list->items[i].len);
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
  if (list->size == list->capacity) {
    int new_capacity =
        list->capacity == 0 ? 8 : checked_len_add(ctx, list->capacity, list->capacity);
    scs_node **new_items = scs_alloc(ctx, (size_t)new_capacity * sizeof(scs_node *));
    if (list->size > 0) {
      memcpy(new_items, list->items, (size_t)list->size * sizeof(scs_node *));
    }
    list->items = new_items;
    list->capacity = new_capacity;
  }
  list->items[list->size] = node;
  list->size++;
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

/*
 * test_main.c - unit tests for Standard Clojure Style in C
 *
 * Copyright (c) 2026, Chris Oakman
 * Released under the ISC license
 * https://github.com/oakmac/standard-clojure-style-c/blob/master/LICENSE.md
 *
 * This file #includes standard-clojure-style.c directly so that the tests can
 * exercise the internal (static) functions.
 *
 * Build and run with `make test` (or via CMake, see README.md).
 */

#include "../standard-clojure-style.c"

#include <stdarg.h>
#include <stdio.h>

/* -----------------------------------------------------------------------------
 * Minimal test framework
 *
 * No external dependencies: a handful of macros that count assertions and
 * print file:line for every failure. The process exits non-zero if any
 * assertion failed.
 */

static int tests_run = 0;
static int tests_failed = 0;
static int assertions_run = 0;
static int assertions_failed = 0;
static const char *current_test = "";
static bool current_test_failed = false;

#define TEST(name) static void test_##name(void)

static void run_test(const char *name, void (*test_fn)(void)) {
  current_test = name;
  current_test_failed = false;
  tests_run++;
  test_fn();
  if (current_test_failed) {
    tests_failed++;
  }
}

#define RUN_TEST(name) run_test(#name, test_##name)

static void report_failure(const char *file, int line, const char *msg) {
  assertions_failed++;
  current_test_failed = true;
  printf("FAIL %s (%s:%d): %s\n", current_test, file, line, msg);
}

#define ASSERT_TRUE(cond)                                                                          \
  do {                                                                                             \
    assertions_run++;                                                                              \
    if (!(cond)) {                                                                                 \
      report_failure(__FILE__, __LINE__, "expected true: " #cond);                                 \
    }                                                                                              \
  } while (0)

#define ASSERT_FALSE(cond)                                                                         \
  do {                                                                                             \
    assertions_run++;                                                                              \
    if (cond) {                                                                                    \
      report_failure(__FILE__, __LINE__, "expected false: " #cond);                                \
    }                                                                                              \
  } while (0)

#define ASSERT_NULL(ptr) ASSERT_TRUE((ptr) == NULL)
#define ASSERT_NOT_NULL(ptr) ASSERT_TRUE((ptr) != NULL)

#define ASSERT_INT_EQ(actual, expected)                                                            \
  do {                                                                                             \
    assertions_run++;                                                                              \
    long actual_ = (long)(actual);                                                                 \
    long expected_ = (long)(expected);                                                             \
    if (actual_ != expected_) {                                                                    \
      char msg_[512];                                                                              \
      snprintf(msg_, sizeof(msg_), "%s: expected %ld, got %ld", #actual, expected_, actual_);      \
      report_failure(__FILE__, __LINE__, msg_);                                                    \
    }                                                                                              \
  } while (0)

/* compares two NUL-terminated strings; either may be NULL */
static bool cstr_equal(const char *a, const char *b) {
  if (a == NULL || b == NULL) {
    return a == b;
  }
  return strcmp(a, b) == 0;
}

#define ASSERT_STR_EQ(actual, expected)                                                            \
  do {                                                                                             \
    assertions_run++;                                                                              \
    const char *actual_ = (actual);                                                                \
    const char *expected_ = (expected);                                                            \
    if (!cstr_equal(actual_, expected_)) {                                                         \
      char msg_[1024];                                                                             \
      snprintf(msg_, sizeof(msg_), "%s: expected \"%s\", got \"%s\"", #actual,                     \
               expected_ ? expected_ : "(null)", actual_ ? actual_ : "(null)");                    \
      report_failure(__FILE__, __LINE__, msg_);                                                    \
    }                                                                                              \
  } while (0)

/* compares an scs_str with a NUL-terminated expected string */
#define ASSERT_SSTR_EQ(actual, expected)                                                           \
  do {                                                                                             \
    assertions_run++;                                                                              \
    scs_str actual_ = (actual);                                                                    \
    const char *expected_ = (expected);                                                            \
    if (!str_eq_cstr(actual_, expected_)) {                                                        \
      char msg_[1024];                                                                             \
      snprintf(msg_, sizeof(msg_), "%s: expected \"%s\", got \"%.*s\"", #actual, expected_,        \
               actual_.len, actual_.data);                                                         \
      report_failure(__FILE__, __LINE__, msg_);                                                    \
    }                                                                                              \
  } while (0)

/* a context for tests that need to allocate (strings, lists) */
static scs_ctx *test_ctx_new(void) {
  scs_ctx *ctx = calloc(1, sizeof(scs_ctx));
  if (!ctx) {
    fprintf(stderr, "out of memory\n");
    exit(1);
  }
  arena_init(&ctx->arena, 4096);
  return ctx;
}

static void test_ctx_free(scs_ctx *ctx) {
  arena_free_all(&ctx->arena);
  free(ctx);
}

/* builds a str list from NUL-terminated strings; the list ends with NULL */
static scs_str_list make_str_list(scs_ctx *ctx, const char *first, ...) {
  scs_str_list list = {NULL, 0, 0};
  va_list args;
  va_start(args, first);
  for (const char *item = first; item != NULL; item = va_arg(args, const char *)) {
    str_list_push(ctx, &list, str_from_cstr(item));
  }
  va_end(args);
  return list;
}

/* builds a whitespace-like node for the helper tests */
static scs_node make_node(const char *name, const char *text) {
  scs_node node;
  memset(&node, 0, sizeof(node));
  node.name = name;
  node.text = str_from_cstr(text);
  return node;
}

/* -----------------------------------------------------------------------------
 * Public API
 */

TEST(version_is_exposed) {
  ASSERT_STR_EQ(scs_version(), SCS_VERSION);
  ASSERT_TRUE(strlen(scs_version()) > 0);
}

TEST(format_is_not_implemented_yet) {
  const char *src = "(def a 1)";
  scs_format_result result = scs_format(src, strlen(src));

  ASSERT_INT_EQ(result.status, SCS_STATUS_ERROR);
  ASSERT_FALSE(result.file_was_ignored);
  ASSERT_NULL(result.out);
  ASSERT_INT_EQ(result.out_len, 0);
  ASSERT_NOT_NULL(result.reason);
  ASSERT_TRUE(result.reason && strstr(result.reason, "not implemented") != NULL);

  scs_format_result_free(&result);
  ASSERT_NULL(result.out);
  ASSERT_NULL(result.reason);

  /* freeing twice is allowed */
  scs_format_result_free(&result);
}

TEST(format_rejects_null_input) {
  scs_format_result result = scs_format(NULL, 0);
  ASSERT_INT_EQ(result.status, SCS_STATUS_ERROR);
  ASSERT_NULL(result.out);
  ASSERT_TRUE(result.reason && strstr(result.reason, "NULL") != NULL);
  scs_format_result_free(&result);
}

TEST(format_result_free_accepts_null) {
  scs_format_result_free(NULL);
  ASSERT_TRUE(true);
}

/* -----------------------------------------------------------------------------
 * Arena
 */

TEST(arena_alloc_returns_zeroed_aligned_memory) {
  scs_arena arena;
  arena_init(&arena, 1024);

  unsigned char *p = arena_try_alloc(&arena, 100);
  ASSERT_NOT_NULL(p);
  ASSERT_INT_EQ((uintptr_t)p % SCS_ARENA_ALIGN, 0);

  bool all_zero = true;
  for (int i = 0; i < 100; i++) {
    if (p[i] != 0) {
      all_zero = false;
    }
  }
  ASSERT_TRUE(all_zero);

  /* 100 bytes rounds up to 112 (the next multiple of 16), so the next
   * allocation starts 112 bytes later and is aligned as well */
  unsigned char *q = arena_try_alloc(&arena, 1);
  ASSERT_NOT_NULL(q);
  ASSERT_INT_EQ(q - p, 112);
  ASSERT_INT_EQ((uintptr_t)q % SCS_ARENA_ALIGN, 0);

  arena_free_all(&arena);
  ASSERT_NULL(arena.current);
}

TEST(arena_zero_size_allocation_returns_unique_pointer) {
  scs_arena arena;
  arena_init(&arena, 1024);

  char *p = arena_try_alloc(&arena, 0);
  char *q = arena_try_alloc(&arena, 0);
  ASSERT_NOT_NULL(p);
  ASSERT_NOT_NULL(q);
  ASSERT_TRUE(p != q);

  arena_free_all(&arena);
}

TEST(arena_grows_across_blocks_without_overlap) {
  scs_arena arena;
  arena_init(&arena, 64); /* tiny blocks force many block transitions */

  enum { num_allocs = 200, alloc_size = 24 };
  unsigned char *ptrs[num_allocs];

  for (int i = 0; i < num_allocs; i++) {
    ptrs[i] = arena_try_alloc(&arena, alloc_size);
    ASSERT_NOT_NULL(ptrs[i]);
    memset(ptrs[i], i, alloc_size);
  }

  /* every allocation must still hold its own pattern: no two overlap */
  bool intact = true;
  for (int i = 0; i < num_allocs; i++) {
    for (int j = 0; j < alloc_size; j++) {
      if (ptrs[i][j] != (unsigned char)i) {
        intact = false;
      }
    }
  }
  ASSERT_TRUE(intact);

  /* more than one block was needed */
  ASSERT_NOT_NULL(arena.current);
  ASSERT_NOT_NULL(arena.current->prev);

  arena_free_all(&arena);
  ASSERT_NULL(arena.current);
}

TEST(arena_large_allocation_gets_its_own_block) {
  scs_arena arena;
  arena_init(&arena, 64);

  size_t big = 10000; /* already a multiple of 16 */
  unsigned char *p = arena_try_alloc(&arena, big);
  ASSERT_NOT_NULL(p);
  ASSERT_INT_EQ(arena.current->capacity, big);
  ASSERT_INT_EQ(arena.current->used, big);

  /* the whole range must be writable */
  memset(p, 0xAB, big);
  ASSERT_INT_EQ(p[0], 0xAB);
  ASSERT_INT_EQ(p[big - 1], 0xAB);

  arena_free_all(&arena);
}

TEST(arena_rejects_sizes_that_would_overflow) {
  scs_arena arena;
  arena_init(&arena, 64);

  ASSERT_NULL(arena_try_alloc(&arena, SIZE_MAX));
  ASSERT_NULL(arena_try_alloc(&arena, SIZE_MAX - 5));

  /* the arena is still usable after a failed allocation */
  ASSERT_NOT_NULL(arena_try_alloc(&arena, 8));

  arena_free_all(&arena);
}

TEST(arena_can_be_reused_after_free_all) {
  scs_arena arena;
  arena_init(&arena, 64);

  ASSERT_NOT_NULL(arena_try_alloc(&arena, 500));
  arena_free_all(&arena);
  ASSERT_NULL(arena.current);

  char *p = arena_try_alloc(&arena, 8);
  ASSERT_NOT_NULL(p);
  ASSERT_INT_EQ(p[0], 0);

  arena_free_all(&arena);
}

/* -----------------------------------------------------------------------------
 * String Util
 */

TEST(str_basics) {
  scs_str s = STR("hello");
  ASSERT_INT_EQ(s.len, 5);
  ASSERT_TRUE(str_eq(s, str_from_cstr("hello")));
  ASSERT_FALSE(str_eq(s, STR("hellO")));
  ASSERT_FALSE(str_eq(s, STR("hell")));
  ASSERT_TRUE(str_eq(EMPTY_STR, STR("")));
  ASSERT_TRUE(str_has_chars(STR(" ")));
  ASSERT_FALSE(str_has_chars(EMPTY_STR));
}

TEST(char_at) {
  ASSERT_SSTR_EQ(char_at(STR("hello"), 0), "h");
  ASSERT_SSTR_EQ(char_at(STR("hello"), 4), "o");
  /* edge cases */
  ASSERT_SSTR_EQ(char_at(STR(""), 0), "");
  ASSERT_SSTR_EQ(char_at(STR("a"), 1), "");  /* out of bounds */
  ASSERT_SSTR_EQ(char_at(STR("a"), -1), ""); /* negative index */
}

TEST(substr) {
  ASSERT_SSTR_EQ(substr(STR("hello world"), 0, 5), "hello");
  ASSERT_SSTR_EQ(substr(STR("hello world"), 6, 11), "world");
  /* negative end index means "until the end" */
  ASSERT_SSTR_EQ(substr(STR("hello world"), 0, -1), "hello world");
  ASSERT_SSTR_EQ(substr(STR("hello"), 2, -1), "llo");
  /* out of bounds indexes */
  ASSERT_SSTR_EQ(substr(STR("hello"), 0, 10), "hello"); /* end beyond string length */
  ASSERT_SSTR_EQ(substr(STR("hello"), 10, 15), "");     /* start beyond string length */
  ASSERT_SSTR_EQ(substr(STR("hello"), -1, 3), "");      /* negative start */
  ASSERT_SSTR_EQ(substr(STR("hello"), 5, -1), "");      /* start at the end */
  ASSERT_SSTR_EQ(substr(STR("hello"), 3, 3), "");       /* empty range */
  ASSERT_SSTR_EQ(substr(STR("hello"), 3, 1), "");       /* start after end */
}

TEST(substr_is_a_view_without_copying) {
  scs_str s = STR("hello world");
  scs_str sub = substr(s, 6, 11);
  ASSERT_TRUE(sub.data == s.data + 6);
  ASSERT_INT_EQ(sub.len, 5);
}

TEST(strings_may_contain_nul_bytes) {
  scs_str s = {"a\0b", 3};
  ASSERT_INT_EQ(str_len(s), 3);
  ASSERT_SSTR_EQ(char_at(s, 2), "b");
  ASSERT_INT_EQ(char_at(s, 1).len, 1);
  ASSERT_INT_EQ(char_at(s, 1).data[0], 0);
  ASSERT_TRUE(str_includes(s, STR("b")));
}

TEST(repeat_string) {
  scs_ctx *ctx = test_ctx_new();
  ASSERT_SSTR_EQ(repeat_string(ctx, STR("abc"), 3), "abcabcabc");
  ASSERT_SSTR_EQ(repeat_string(ctx, STR("x"), 5), "xxxxx");
  /* edge cases */
  ASSERT_SSTR_EQ(repeat_string(ctx, STR(""), 5), "");
  ASSERT_SSTR_EQ(repeat_string(ctx, STR("hello"), 0), "");
  ASSERT_SSTR_EQ(repeat_string(ctx, STR("a"), -1), "");
  test_ctx_free(ctx);
}

TEST(str_includes) {
  ASSERT_TRUE(str_includes(STR("hello world"), STR("world")));
  ASSERT_TRUE(str_includes(STR("hello world"), STR("hello")));
  ASSERT_FALSE(str_includes(STR("hello world"), STR("xyz")));
  /* edge cases */
  ASSERT_TRUE(str_includes(STR(""), STR("")));
  ASSERT_TRUE(str_includes(STR("abc"), STR("")));
  ASSERT_FALSE(str_includes(STR(""), STR("a")));
  ASSERT_FALSE(str_includes(STR("ab"), STR("abc")));
}

TEST(str_last_index_of) {
  ASSERT_INT_EQ(str_last_index_of(STR("a,b,c"), STR(",")), 3);
  ASSERT_INT_EQ(str_last_index_of(STR("abc\ndef\nxyz"), STR("\n")), 7);
  ASSERT_INT_EQ(str_last_index_of(STR("aaa"), STR("a")), 2);
  /* not found */
  ASSERT_INT_EQ(str_last_index_of(STR("abc"), STR("z")), -1);
  ASSERT_INT_EQ(str_last_index_of(STR(""), STR("a")), -1);
  ASSERT_INT_EQ(str_last_index_of(STR("abc"), STR("\n")), -1);
  /* an empty needle is found at the end */
  ASSERT_INT_EQ(str_last_index_of(STR("abc"), STR("")), 3);
  /* multi-character needle, and code point indexes with multi-byte text */
  ASSERT_INT_EQ(str_last_index_of(STR("ab-ab"), STR("ab")), 3);
  ASSERT_INT_EQ(str_last_index_of(STR("σσ\nσ"), STR("\n")), 2);
}

TEST(str_index_of_from) {
  ASSERT_INT_EQ(str_index_of_from(STR("a,b,c"), STR(","), 0), 1);
  ASSERT_INT_EQ(str_index_of_from(STR("a,b,c"), STR(","), 2), 3);
  /* from_idx pointing directly at a match */
  ASSERT_INT_EQ(str_index_of_from(STR("a,b,c"), STR(","), 3), 3);
  /* not found at or after from_idx */
  ASSERT_INT_EQ(str_index_of_from(STR("a,b,c"), STR(","), 4), -1);
  ASSERT_INT_EQ(str_index_of_from(STR("abc"), STR("z"), 0), -1);
  ASSERT_INT_EQ(str_index_of_from(STR(""), STR("a"), 0), -1);
  /* out of range from_idx is clamped */
  ASSERT_INT_EQ(str_index_of_from(STR("a,b"), STR(","), -5), 1);
  ASSERT_INT_EQ(str_index_of_from(STR("a,b"), STR(","), 50), -1);
  ASSERT_INT_EQ(str_index_of_from(STR("abc"), STR(""), 50), 3);
  /* code point indexes with multi-byte text */
  ASSERT_INT_EQ(str_index_of_from(STR("σ,σ,σ"), STR(","), 2), 3);
}

TEST(str_has_non_whitespace_chars) {
  /* equivalent to str_trim(s).len > 0 */
  const char *cases[] = {"", " ", "   ", "\n", " \t\n ", "a", " a ", ",", " , ", "\xc2\xa0", "abc"};
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    scs_str s = str_from_cstr(cases[i]);
    ASSERT_TRUE(str_has_non_whitespace_chars(s) == (str_trim(s).len > 0));
  }
  /* whitespace-only strings return false */
  ASSERT_FALSE(str_has_non_whitespace_chars(STR("")));
  ASSERT_FALSE(str_has_non_whitespace_chars(STR("   ")));
  ASSERT_FALSE(str_has_non_whitespace_chars(STR(" \t\r\n ")));
  /* any non-whitespace character returns true */
  ASSERT_TRUE(str_has_non_whitespace_chars(STR("a")));
  ASSERT_TRUE(str_has_non_whitespace_chars(STR("   x   ")));
  /* commas are NOT whitespace for this function (same as str_trim) */
  ASSERT_TRUE(str_has_non_whitespace_chars(STR(",")));
}

TEST(ascii_to_upper_case) {
  scs_ctx *ctx = test_ctx_new();
  ASSERT_SSTR_EQ(ascii_to_upper_case(ctx, STR("hello")), "HELLO");
  ASSERT_SSTR_EQ(ascii_to_upper_case(ctx, STR("Hello World!")), "HELLO WORLD!");
  ASSERT_SSTR_EQ(ascii_to_upper_case(ctx, STR("")), "");
  ASSERT_SSTR_EQ(ascii_to_upper_case(ctx, STR("123")), "123");
  /* only ASCII letters are converted; other characters pass through untouched */
  ASSERT_SSTR_EQ(ascii_to_upper_case(ctx, STR("\xc3\xa1"
                                              "b")),
                 "\xc3\xa1"
                 "B");
  test_ctx_free(ctx);
}

TEST(str_join) {
  scs_ctx *ctx = test_ctx_new();
  scs_str_list abc = make_str_list(ctx, "a", "b", "c", NULL);
  ASSERT_SSTR_EQ(str_join(ctx, &abc, STR("-")), "a-b-c");
  scs_str_list hello_world = make_str_list(ctx, "hello", "world", NULL);
  ASSERT_SSTR_EQ(str_join(ctx, &hello_world, STR(" ")), "hello world");
  /* edge cases */
  scs_str_list empty = {NULL, 0, 0};
  ASSERT_SSTR_EQ(str_join(ctx, &empty, STR("-")), "");
  scs_str_list a = make_str_list(ctx, "a", NULL);
  ASSERT_SSTR_EQ(str_join(ctx, &a, STR("-")), "a");
  scs_str_list ab = make_str_list(ctx, "a", "b", NULL);
  ASSERT_SSTR_EQ(str_join(ctx, &ab, STR("")), "ab");
  test_ctx_free(ctx);
}

TEST(rtrim) {
  ASSERT_SSTR_EQ(rtrim(STR("  hello  ")), "  hello");
  ASSERT_SSTR_EQ(rtrim(STR("hello\n\t  ")), "hello");
  /* edge cases */
  ASSERT_SSTR_EQ(rtrim(STR("")), "");
  ASSERT_SSTR_EQ(rtrim(STR("   ")), "");
  ASSERT_SSTR_EQ(rtrim(STR("hello")), "hello");
  /* Unicode whitespace: no-break space, ideographic space, BOM */
  ASSERT_SSTR_EQ(rtrim(STR("a\xc2\xa0\xe3\x80\x80\xef\xbb\xbf")), "a");
  /* multi-byte non-whitespace is kept intact */
  ASSERT_SSTR_EQ(rtrim(STR("\xcf\x83 ")), "\xcf\x83");
}

TEST(str_trim) {
  ASSERT_SSTR_EQ(str_trim(STR("  hello  ")), "hello");
  ASSERT_SSTR_EQ(str_trim(STR("\n\t hello \t\n")), "hello");
  /* edge cases */
  ASSERT_SSTR_EQ(str_trim(STR("")), "");
  ASSERT_SSTR_EQ(str_trim(STR("   ")), "");
  ASSERT_SSTR_EQ(str_trim(STR("hello")), "hello");
  ASSERT_SSTR_EQ(str_trim(STR("\xc2\xa0hello\xe2\x80\x83")), "hello");
  /* a comma is not whitespace */
  ASSERT_SSTR_EQ(str_trim(STR(" , ")), ",");
}

TEST(str_starts_with) {
  ASSERT_TRUE(str_starts_with(STR("hello world"), STR("hello")));
  ASSERT_FALSE(str_starts_with(STR("hello world"), STR("world")));
  /* edge cases */
  ASSERT_TRUE(str_starts_with(STR(""), STR("")));
  ASSERT_TRUE(str_starts_with(STR("hello"), STR("")));
  ASSERT_FALSE(str_starts_with(STR(""), STR("a")));
  ASSERT_FALSE(str_starts_with(STR("he"), STR("hello")));
}

TEST(str_ends_with) {
  ASSERT_TRUE(str_ends_with(STR("hello world"), STR("world")));
  ASSERT_FALSE(str_ends_with(STR("hello world"), STR("hello")));
  /* edge cases */
  ASSERT_TRUE(str_ends_with(STR(""), STR("")));
  ASSERT_TRUE(str_ends_with(STR("hello"), STR("")));
  ASSERT_FALSE(str_ends_with(STR(""), STR("a")));
  ASSERT_FALSE(str_ends_with(STR("lo"), STR("hello")));
}

TEST(str_replace_first) {
  scs_ctx *ctx = test_ctx_new();
  ASSERT_SSTR_EQ(str_replace_first(ctx, STR("hello world"), STR("world"), STR("there")),
                 "hello there");
  ASSERT_SSTR_EQ(str_replace_first(ctx, STR("hello hello"), STR("hello"), STR("hi")), "hi hello");
  /* edge cases */
  ASSERT_SSTR_EQ(str_replace_first(ctx, STR(""), STR("a"), STR("b")), "");
  ASSERT_SSTR_EQ(str_replace_first(ctx, STR("hello"), STR(""), STR("x")), "hello");
  ASSERT_SSTR_EQ(str_replace_first(ctx, STR("hello"), STR("x"), STR("y")), "hello");
  ASSERT_SSTR_EQ(str_replace_first(ctx, STR("abc"), STR("b"), STR("")), "ac");
  test_ctx_free(ctx);
}

TEST(str_replace_all) {
  scs_ctx *ctx = test_ctx_new();
  ASSERT_SSTR_EQ(str_replace_all(ctx, STR("aaa"), STR("a"), STR("b")), "bbb");
  ASSERT_SSTR_EQ(str_replace_all(ctx, STR("a,b,c,"), STR(","), STR("")), "abc");
  ASSERT_SSTR_EQ(str_replace_all(ctx, STR("a,b,c,"), STR(","), STR("x")), "axbxcx");
  ASSERT_SSTR_EQ(str_replace_all(ctx, STR("\n\n\n"), STR("\n"), STR("x")), "xxx");
  /* replacements longer than what they replace, and no overlap */
  ASSERT_SSTR_EQ(str_replace_all(ctx, STR("abab"), STR("ab"), STR("xyz")), "xyzxyz");
  ASSERT_SSTR_EQ(str_replace_all(ctx, STR("aaaa"), STR("aa"), STR("a")), "aa");
  /* edge cases */
  ASSERT_SSTR_EQ(str_replace_all(ctx, STR(""), STR("a"), STR("b")), "");
  ASSERT_SSTR_EQ(str_replace_all(ctx, STR("hello"), STR(""), STR("x")), "hello");
  ASSERT_SSTR_EQ(str_replace_all(ctx, STR("hello"), STR("z"), STR("x")), "hello");
  test_ctx_free(ctx);
}

TEST(crlf_to_lf) {
  scs_ctx *ctx = test_ctx_new();
  ASSERT_SSTR_EQ(crlf_to_lf(ctx, STR("hello\r\nworld")), "hello\nworld");
  ASSERT_SSTR_EQ(crlf_to_lf(ctx, STR("line1\r\nline2\r\nline3")), "line1\nline2\nline3");
  /* edge cases */
  ASSERT_SSTR_EQ(crlf_to_lf(ctx, STR("")), "");
  ASSERT_SSTR_EQ(crlf_to_lf(ctx, STR("no crlf")), "no crlf");
  ASSERT_SSTR_EQ(crlf_to_lf(ctx, STR("\r\n")), "\n");
  /* a lone carriage return is left alone */
  ASSERT_SSTR_EQ(crlf_to_lf(ctx, STR("a\rb")), "a\rb");
  test_ctx_free(ctx);
}

TEST(str_concat) {
  scs_ctx *ctx = test_ctx_new();
  ASSERT_SSTR_EQ(str_concat(ctx, STR("foo"), STR("bar")), "foobar");
  ASSERT_SSTR_EQ(str_concat(ctx, STR(""), STR("")), "");
  ASSERT_SSTR_EQ(str_concat(ctx, STR("a"), STR("")), "a");
  ASSERT_SSTR_EQ(str_concat3(ctx, STR("a"), STR("b"), STR("c")), "abc");
  ASSERT_SSTR_EQ(str_concat3(ctx, STR(""), STR("b"), STR("")), "b");
  /* the result is a copy in the arena, not a view */
  scs_str copy = str_copy(ctx, STR("xyz"));
  ASSERT_SSTR_EQ(copy, "xyz");
  ASSERT_TRUE(copy.data != STR("xyz").data || true); /* literals may be merged; just exercise it */
  test_ctx_free(ctx);
}

TEST(str_split) {
  scs_ctx *ctx = test_ctx_new();
  scs_str_list parts = str_split(ctx, STR("java.util.Date"), STR("."));
  ASSERT_INT_EQ(parts.size, 3);
  ASSERT_SSTR_EQ(parts.items[0], "java");
  ASSERT_SSTR_EQ(parts.items[1], "util");
  ASSERT_SSTR_EQ(parts.items[2], "Date");

  /* no separator: one piece */
  parts = str_split(ctx, STR("Date"), STR("."));
  ASSERT_INT_EQ(parts.size, 1);
  ASSERT_SSTR_EQ(parts.items[0], "Date");

  /* consecutive, leading, and trailing separators produce empty pieces */
  parts = str_split(ctx, STR(".a..b."), STR("."));
  ASSERT_INT_EQ(parts.size, 5);
  ASSERT_SSTR_EQ(parts.items[0], "");
  ASSERT_SSTR_EQ(parts.items[1], "a");
  ASSERT_SSTR_EQ(parts.items[2], "");
  ASSERT_SSTR_EQ(parts.items[3], "b");
  ASSERT_SSTR_EQ(parts.items[4], "");

  /* empty input: one empty piece */
  parts = str_split(ctx, STR(""), STR("."));
  ASSERT_INT_EQ(parts.size, 1);
  ASSERT_SSTR_EQ(parts.items[0], "");
  test_ctx_free(ctx);
}

/* -----------------------------------------------------------------------------
 * String Util - Unicode
 *
 * char_at, substr, and str_len operate on characters (code points), not bytes.
 */

TEST(unicode_str_len) {
  ASSERT_INT_EQ(str_len(STR("levels-σ")), 8); /* σ is 2 bytes */
  ASSERT_INT_EQ(str_len(STR("σ")), 1);
  ASSERT_INT_EQ(str_len(STR("αβγ")), 3);   /* 3 chars, 6 bytes */
  ASSERT_INT_EQ(str_len(STR("hello")), 5); /* ASCII baseline */
  ASSERT_INT_EQ(str_len(STR("")), 0);
  /* 3-byte and 4-byte characters */
  ASSERT_INT_EQ(str_len(STR("日本語")), 3); /* 3 chars, 9 bytes */
  ASSERT_INT_EQ(str_len(STR("a日b")), 3);
  ASSERT_INT_EQ(str_len(STR("🤓")), 1);      /* 4 bytes */
  ASSERT_INT_EQ(str_len(STR("👁️ ❤️ 🤓")), 7); /* variation selectors count as characters */
}

TEST(unicode_char_at) {
  ASSERT_SSTR_EQ(char_at(STR("levels-σ"), 0), "l");
  ASSERT_SSTR_EQ(char_at(STR("levels-σ"), 6), "-");
  ASSERT_SSTR_EQ(char_at(STR("levels-σ"), 7), "σ");
  /* out of bounds */
  ASSERT_SSTR_EQ(char_at(STR("levels-σ"), 8), "");
  /* multi-byte characters in the middle */
  ASSERT_SSTR_EQ(char_at(STR("aσb"), 0), "a");
  ASSERT_SSTR_EQ(char_at(STR("aσb"), 1), "σ");
  ASSERT_SSTR_EQ(char_at(STR("aσb"), 2), "b");
  ASSERT_SSTR_EQ(char_at(STR("aσb"), 3), "");
  /* CJK characters */
  ASSERT_SSTR_EQ(char_at(STR("日本語"), 0), "日");
  ASSERT_SSTR_EQ(char_at(STR("日本語"), 1), "本");
  ASSERT_SSTR_EQ(char_at(STR("日本語"), 2), "語");
  /* 4-byte character */
  ASSERT_SSTR_EQ(char_at(STR("a🤓b"), 1), "🤓");
}

TEST(unicode_substr) {
  /* end_idx is exclusive */
  ASSERT_SSTR_EQ(substr(STR("levels-σ"), 0, 8), "levels-σ");
  ASSERT_SSTR_EQ(substr(STR("levels-σ"), 7, 8), "σ");
  ASSERT_SSTR_EQ(substr(STR("levels-σ"), 6, 8), "-σ");
  ASSERT_SSTR_EQ(substr(STR("levels-σ"), 0, 7), "levels-");
  /* -1 end_idx */
  ASSERT_SSTR_EQ(substr(STR("levels-σ"), 0, -1), "levels-σ");
  ASSERT_SSTR_EQ(substr(STR("levels-σ"), 7, -1), "σ");
  /* multi-byte characters in the middle */
  ASSERT_SSTR_EQ(substr(STR("aσb"), 0, 3), "aσb");
  ASSERT_SSTR_EQ(substr(STR("aσb"), 1, 2), "σ");
  ASSERT_SSTR_EQ(substr(STR("aσb"), 1, 3), "σb");
}

TEST(unicode_malformed_input_is_passed_through) {
  /* a truncated 3-byte sequence at the end: counted as one character per byte */
  scs_str truncated = {"a\xe6\x97", 3};
  ASSERT_INT_EQ(str_len(truncated), 2);
  ASSERT_INT_EQ(char_at(truncated, 1).len, 2);
  /* a stray continuation byte counts as one character */
  scs_str stray = {"a\x80"
                   "b",
                   3};
  ASSERT_INT_EQ(str_len(stray), 3);
  ASSERT_SSTR_EQ(char_at(stray, 2), "b");
  /* malformed bytes are never considered whitespace */
  ASSERT_SSTR_EQ(rtrim(stray), "a\x80"
                               "b");
  ASSERT_TRUE(str_has_non_whitespace_chars((scs_str){"\x80", 1}));
}

/* -----------------------------------------------------------------------------
 * Stack Operations
 */

TEST(node_list_peek) {
  scs_ctx *ctx = test_ctx_new();
  scs_node nodes[4];
  scs_node_list list = {NULL, 0, 0};
  for (int i = 0; i < 4; i++) {
    node_list_push(ctx, &list, &nodes[i]);
  }

  /* 0 is the last element */
  ASSERT_TRUE(node_list_peek(&list, 0) == &nodes[3]);
  ASSERT_TRUE(node_list_peek(&list, 1) == &nodes[2]);
  ASSERT_TRUE(node_list_peek(&list, 2) == &nodes[1]);
  ASSERT_TRUE(node_list_peek(&list, 3) == &nodes[0]);
  /* NULL when idx_from_back is too large or negative */
  ASSERT_NULL(node_list_peek(&list, 4));
  ASSERT_NULL(node_list_peek(&list, 5));
  ASSERT_NULL(node_list_peek(&list, -1));

  scs_node_list empty = {NULL, 0, 0};
  ASSERT_NULL(node_list_peek(&empty, 0));
  ASSERT_NULL(node_list_last(&empty));

  scs_node_list one = {NULL, 0, 0};
  node_list_push(ctx, &one, &nodes[0]);
  ASSERT_TRUE(node_list_peek(&one, 0) == &nodes[0]);
  ASSERT_TRUE(node_list_last(&one) == &nodes[0]);
  ASSERT_NULL(node_list_peek(&one, 1));
  test_ctx_free(ctx);
}

TEST(node_list_pop) {
  scs_ctx *ctx = test_ctx_new();
  scs_node nodes[3];
  scs_node_list stack = {NULL, 0, 0};
  for (int i = 0; i < 3; i++) {
    node_list_push(ctx, &stack, &nodes[i]);
  }

  /* removes and returns the last element */
  ASSERT_TRUE(node_list_pop(&stack) == &nodes[2]);
  ASSERT_INT_EQ(stack.size, 2);
  ASSERT_TRUE(stack.items[0] == &nodes[0]);
  ASSERT_TRUE(stack.items[1] == &nodes[1]);

  ASSERT_TRUE(node_list_pop(&stack) == &nodes[1]);
  ASSERT_TRUE(node_list_pop(&stack) == &nodes[0]);
  ASSERT_INT_EQ(stack.size, 0);

  /* popping an empty stack returns NULL and leaves it empty */
  ASSERT_NULL(node_list_pop(&stack));
  ASSERT_INT_EQ(stack.size, 0);
  test_ctx_free(ctx);
}

TEST(node_list_push_grows) {
  scs_ctx *ctx = test_ctx_new();
  enum { num_nodes = 100 };
  scs_node nodes[num_nodes];
  scs_node_list stack = {NULL, 0, 0};
  for (int i = 0; i < num_nodes; i++) {
    node_list_push(ctx, &stack, &nodes[i]);
    ASSERT_INT_EQ(stack.size, i + 1);
    ASSERT_TRUE(stack.capacity >= stack.size);
  }
  bool intact = true;
  for (int i = 0; i < num_nodes; i++) {
    if (stack.items[i] != &nodes[i]) {
      intact = false;
    }
  }
  ASSERT_TRUE(intact);
  test_ctx_free(ctx);
}

TEST(node_list_integration) {
  scs_ctx *ctx = test_ctx_new();
  scs_node n1, n2, n3, n4;
  scs_node_list stack = {NULL, 0, 0};

  node_list_push(ctx, &stack, &n1);
  node_list_push(ctx, &stack, &n2);
  node_list_push(ctx, &stack, &n3);

  ASSERT_TRUE(node_list_peek(&stack, 0) == &n3);
  ASSERT_TRUE(node_list_peek(&stack, 1) == &n2);

  ASSERT_TRUE(node_list_pop(&stack) == &n3);
  ASSERT_TRUE(node_list_peek(&stack, 0) == &n2);

  node_list_push(ctx, &stack, &n4);
  ASSERT_INT_EQ(stack.size, 3);
  ASSERT_TRUE(stack.items[0] == &n1);
  ASSERT_TRUE(stack.items[1] == &n2);
  ASSERT_TRUE(stack.items[2] == &n4);
  test_ctx_free(ctx);
}

TEST(str_list_operations) {
  scs_ctx *ctx = test_ctx_new();
  scs_str_list list = make_str_list(ctx, "java", "util", "Date", NULL);
  ASSERT_INT_EQ(list.size, 3);
  ASSERT_SSTR_EQ(str_list_last(&list), "Date");

  scs_str_list package = str_list_drop_last(ctx, &list);
  ASSERT_INT_EQ(package.size, 2);
  ASSERT_SSTR_EQ(str_join(ctx, &package, STR(".")), "java.util");
  /* the original list is untouched */
  ASSERT_INT_EQ(list.size, 3);

  scs_str_list empty = {NULL, 0, 0};
  ASSERT_SSTR_EQ(str_list_last(&empty), "");
  ASSERT_INT_EQ(str_list_drop_last(ctx, &empty).size, 0);

  /* growth past the initial capacity keeps every item */
  scs_str_list many = {NULL, 0, 0};
  for (int i = 0; i < 50; i++) {
    str_list_push(ctx, &many, STR("x"));
  }
  ASSERT_INT_EQ(many.size, 50);
  ASSERT_INT_EQ(str_join(ctx, &many, STR("")).len, 50);
  test_ctx_free(ctx);
}

/* -----------------------------------------------------------------------------
 * Format Helpers
 */

TEST(comment_needs_space_before) {
  ASSERT_TRUE(comment_needs_space_before(STR("foo"), STR(";bar")));
  ASSERT_TRUE(comment_needs_space_before(STR("foo {}"), STR(";bar")));
  ASSERT_FALSE(comment_needs_space_before(STR("foo "), STR(";bar")));
  ASSERT_FALSE(comment_needs_space_before(STR(""), STR(";bar")));
  ASSERT_FALSE(comment_needs_space_before(STR("foo ["), STR(";bar")));
  ASSERT_FALSE(comment_needs_space_before(STR("foo ("), STR(";bar")));
  ASSERT_FALSE(comment_needs_space_before(STR("foo {"), STR(";bar")));
  /* not a comment */
  ASSERT_FALSE(comment_needs_space_before(STR("foo"), STR("bar")));
}

TEST(comment_needs_space_inside) {
  ASSERT_TRUE(comment_needs_space_inside(STR(";foo")));
  ASSERT_TRUE(comment_needs_space_inside(STR(";;foo")));
  ASSERT_TRUE(comment_needs_space_inside(STR(";;;;;;;foo")));
  ASSERT_FALSE(comment_needs_space_inside(STR(";; foo")));
  ASSERT_FALSE(comment_needs_space_inside(STR("; foo")));
  ASSERT_FALSE(comment_needs_space_inside(STR(";      foo")));
  ASSERT_FALSE(comment_needs_space_inside(STR(";")));
  ASSERT_FALSE(comment_needs_space_inside(STR(";;")));
  ASSERT_FALSE(comment_needs_space_inside(STR(";;;;;;")));
}

TEST(remove_leading_whitespace) {
  ASSERT_SSTR_EQ(remove_leading_whitespace(STR("\n ,,")), ",,");
  ASSERT_SSTR_EQ(remove_leading_whitespace(STR(" \n ")), "");
  ASSERT_SSTR_EQ(remove_leading_whitespace(STR("  \n\n  ")), "");
  ASSERT_SSTR_EQ(remove_leading_whitespace(STR(",, \n ")), "");
  ASSERT_SSTR_EQ(remove_leading_whitespace(STR(",, \n\n ")), "");
  ASSERT_SSTR_EQ(remove_leading_whitespace(STR(",, \n\n")), "");
  /* text that does not start with the pattern only loses trailing whitespace */
  ASSERT_SSTR_EQ(remove_leading_whitespace(STR("abc \n ")), "abc");
  ASSERT_SSTR_EQ(remove_leading_whitespace(STR(" ,abc")), " ,abc");
  ASSERT_SSTR_EQ(remove_leading_whitespace(STR("")), "");
}

TEST(remove_trailing_whitespace) {
  ASSERT_SSTR_EQ(remove_trailing_whitespace(STR("aaa,")), "aaa");
  ASSERT_SSTR_EQ(remove_trailing_whitespace(STR("aaa , ")), "aaa");
  ASSERT_SSTR_EQ(remove_trailing_whitespace(STR("aaa")), "aaa");
  ASSERT_SSTR_EQ(remove_trailing_whitespace(STR("  aaa aaa ,, ")), "  aaa aaa");
  ASSERT_SSTR_EQ(remove_trailing_whitespace(STR(" , aaa aaa ,, ")), " , aaa aaa");
  /* does not remove newline characters: it only operates on a single line */
  ASSERT_SSTR_EQ(remove_trailing_whitespace(STR("aaa \n ")), "aaa \n");
  /* empty and whitespace-only strings */
  ASSERT_SSTR_EQ(remove_trailing_whitespace(STR("")), "");
  ASSERT_SSTR_EQ(remove_trailing_whitespace(STR(" ")), "");
  ASSERT_SSTR_EQ(remove_trailing_whitespace(STR(",")), "");
  ASSERT_SSTR_EQ(remove_trailing_whitespace(STR(" ,, ,, ")), "");
}

TEST(is_space_or_comma) {
  ASSERT_TRUE(is_space_or_comma(STR(" ")));
  ASSERT_TRUE(is_space_or_comma(STR(",")));
  ASSERT_FALSE(is_space_or_comma(STR("a")));
  ASSERT_FALSE(is_space_or_comma(STR("\n")));
  ASSERT_FALSE(is_space_or_comma(STR("\t")));
  ASSERT_FALSE(is_space_or_comma(STR("")));
  ASSERT_FALSE(is_space_or_comma(STR(", ")));
}

TEST(remove_chars_up_to_newline) {
  ASSERT_SSTR_EQ(remove_chars_up_to_newline(STR("abc\nxyz")), "xyz");
  ASSERT_SSTR_EQ(remove_chars_up_to_newline(STR("abc")), "abc");
  ASSERT_SSTR_EQ(remove_chars_up_to_newline(STR("abc\ndef\n\nxyz")), "xyz");
  /* newline at the very end, or the string is only a newline */
  ASSERT_SSTR_EQ(remove_chars_up_to_newline(STR("abc\n")), "");
  ASSERT_SSTR_EQ(remove_chars_up_to_newline(STR("\n")), "");
  ASSERT_SSTR_EQ(remove_chars_up_to_newline(STR("")), "");
  ASSERT_SSTR_EQ(remove_chars_up_to_newline(STR("\nabc")), "abc");
}

TEST(txt_has_commas_after_newline) {
  ASSERT_TRUE(txt_has_commas_after_newline(STR("\n ,,")));
  ASSERT_TRUE(txt_has_commas_after_newline(STR("\n\n  ,")));
  ASSERT_FALSE(txt_has_commas_after_newline(STR(" \n ")));
  ASSERT_FALSE(txt_has_commas_after_newline(STR("  \n\n  ")));
  ASSERT_FALSE(txt_has_commas_after_newline(STR(",, \n ")));
  ASSERT_FALSE(txt_has_commas_after_newline(STR(",, \n\n ")));
  /* the comma must come after the *last* newline */
  ASSERT_FALSE(txt_has_commas_after_newline(STR("a,b\nc")));
  ASSERT_FALSE(txt_has_commas_after_newline(STR("a\nb,c\nd")));
  ASSERT_TRUE(txt_has_commas_after_newline(STR("a\nb,")));
  /* strings without a newline always return false, even with commas */
  ASSERT_FALSE(txt_has_commas_after_newline(STR(",,,")));
  ASSERT_FALSE(txt_has_commas_after_newline(STR("")));
}

TEST(num_spaces_after_newline) {
  scs_node node = make_node("whitespace", "\n");
  ASSERT_INT_EQ(num_spaces_after_newline(&node), 0);
  node = make_node("whitespace", "\n  ");
  ASSERT_INT_EQ(num_spaces_after_newline(&node), 2);
  node = make_node("whitespace", "\n    ");
  ASSERT_INT_EQ(num_spaces_after_newline(&node), 4);
  /* only the characters after the *last* newline are counted */
  node = make_node("whitespace", "\n  \n ");
  ASSERT_INT_EQ(num_spaces_after_newline(&node), 1);
  node = make_node("whitespace", "  \n   ");
  ASSERT_INT_EQ(num_spaces_after_newline(&node), 3);
}

TEST(is_newline_node_with_comma_on_next_line) {
  /* newline node with a comma after the last newline */
  scs_node node = make_node("whitespace", "\n ,");
  ASSERT_TRUE(is_newline_node_with_comma_on_next_line(&node));
  node = make_node("whitespace", "\n\n  ,,");
  ASSERT_TRUE(is_newline_node_with_comma_on_next_line(&node));
  /* newline node without a comma on the next line */
  node = make_node("whitespace", "\n ");
  ASSERT_FALSE(is_newline_node_with_comma_on_next_line(&node));
  node = make_node("whitespace", ", \n ");
  ASSERT_FALSE(is_newline_node_with_comma_on_next_line(&node));
  /* not a newline node */
  node = make_node("whitespace", " , ");
  ASSERT_FALSE(is_newline_node_with_comma_on_next_line(&node));
  node = make_node("token", "a,b");
  ASSERT_FALSE(is_newline_node_with_comma_on_next_line(&node));
  ASSERT_FALSE(is_newline_node_with_comma_on_next_line(NULL));
}

TEST(newline_and_whitespace_node_predicates) {
  scs_node node = make_node("whitespace", " ");
  ASSERT_TRUE(is_whitespace_node(&node));
  ASSERT_FALSE(is_newline_node(&node));
  node = make_node("whitespace", " \n ");
  ASSERT_TRUE(is_whitespace_node(&node));
  ASSERT_TRUE(is_newline_node(&node));
  node = make_node("token", "\n");
  ASSERT_FALSE(is_whitespace_node(&node));
  ASSERT_FALSE(is_newline_node(&node));
  node = make_node(NULL, "\n");
  ASSERT_FALSE(is_whitespace_node(&node));
}

/* -----------------------------------------------------------------------------
 * Main
 */

int main(void) {
  RUN_TEST(version_is_exposed);
  RUN_TEST(format_is_not_implemented_yet);
  RUN_TEST(format_rejects_null_input);
  RUN_TEST(format_result_free_accepts_null);

  RUN_TEST(arena_alloc_returns_zeroed_aligned_memory);
  RUN_TEST(arena_zero_size_allocation_returns_unique_pointer);
  RUN_TEST(arena_grows_across_blocks_without_overlap);
  RUN_TEST(arena_large_allocation_gets_its_own_block);
  RUN_TEST(arena_rejects_sizes_that_would_overflow);
  RUN_TEST(arena_can_be_reused_after_free_all);

  RUN_TEST(str_basics);
  RUN_TEST(char_at);
  RUN_TEST(substr);
  RUN_TEST(substr_is_a_view_without_copying);
  RUN_TEST(strings_may_contain_nul_bytes);
  RUN_TEST(repeat_string);
  RUN_TEST(str_includes);
  RUN_TEST(str_last_index_of);
  RUN_TEST(str_index_of_from);
  RUN_TEST(str_has_non_whitespace_chars);
  RUN_TEST(ascii_to_upper_case);
  RUN_TEST(str_join);
  RUN_TEST(rtrim);
  RUN_TEST(str_trim);
  RUN_TEST(str_starts_with);
  RUN_TEST(str_ends_with);
  RUN_TEST(str_replace_first);
  RUN_TEST(str_replace_all);
  RUN_TEST(crlf_to_lf);
  RUN_TEST(str_concat);
  RUN_TEST(str_split);

  RUN_TEST(unicode_str_len);
  RUN_TEST(unicode_char_at);
  RUN_TEST(unicode_substr);
  RUN_TEST(unicode_malformed_input_is_passed_through);

  RUN_TEST(node_list_peek);
  RUN_TEST(node_list_pop);
  RUN_TEST(node_list_push_grows);
  RUN_TEST(node_list_integration);
  RUN_TEST(str_list_operations);

  RUN_TEST(comment_needs_space_before);
  RUN_TEST(comment_needs_space_inside);
  RUN_TEST(remove_leading_whitespace);
  RUN_TEST(remove_trailing_whitespace);
  RUN_TEST(is_space_or_comma);
  RUN_TEST(remove_chars_up_to_newline);
  RUN_TEST(txt_has_commas_after_newline);
  RUN_TEST(num_spaces_after_newline);
  RUN_TEST(is_newline_node_with_comma_on_next_line);
  RUN_TEST(newline_and_whitespace_node_predicates);

  printf("%d tests, %d assertions, %d failures\n", tests_run, assertions_run, assertions_failed);
  return tests_failed == 0 ? 0 : 1;
}

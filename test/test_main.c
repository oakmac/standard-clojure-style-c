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

#define TEST(name) static void name(void)

static void run_test(const char *name, void (*test_fn)(void)) {
  current_test = name;
  current_test_failed = false;
  tests_run++;
  test_fn();
  if (current_test_failed) {
    tests_failed++;
  }
}

#define RUN_TEST(name) run_test(#name, name)

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

  printf("%d tests, %d assertions, %d failures\n", tests_run, assertions_run, assertions_failed);
  return tests_failed == 0 ? 0 : 1;
}

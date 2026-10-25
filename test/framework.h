/*
 * framework.h - the minimal test framework shared by the test programs
 *
 * Copyright (c) 2026, Chris Oakman
 * Released under the ISC license
 * https://github.com/oakmac/standard-clojure-style-c/blob/master/LICENSE.md
 *
 * No external dependencies: a handful of macros that count assertions and
 * print file:line for every failure. A test program includes this header,
 * defines tests with TEST(name), runs them with RUN_TEST(name), and finishes
 * with TEST_SUMMARY(), which prints the totals and returns the exit code.
 * Tests whose name does not contain the optional command line filter are
 * skipped.
 */

#ifndef SCS_TEST_FRAMEWORK_H
#define SCS_TEST_FRAMEWORK_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int tests_run = 0;
static int tests_failed = 0;
static int assertions_run = 0;
static int assertions_failed = 0;
static const char *current_test = "";
static bool current_test_failed = false;

/* when set (from the command line), only tests whose name contains it are run */
static const char *test_filter = NULL;

static bool should_run_test(const char *name) {
  return test_filter == NULL || strstr(name, test_filter) != NULL;
}

static void start_test(const char *name) {
  current_test = name;
  current_test_failed = false;
  tests_run++;
}

static void finish_test(void) {
  if (current_test_failed) {
    tests_failed++;
  }
}

#define TEST(name) static void test_##name(void)

static void run_test(const char *name, void (*test_fn)(void)) {
  if (!should_run_test(name)) {
    return;
  }
  start_test(name);
  test_fn();
  finish_test();
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

/* prints the totals; returns the process exit code */
static int test_summary(void) {
  if (tests_run == 0) {
    printf("no tests matched \"%s\"\n", test_filter ? test_filter : "");
    return 1;
  }
  printf("%d tests, %d assertions, %d failures\n", tests_run, assertions_run, assertions_failed);
  return tests_failed == 0 ? 0 : 1;
}

#define TEST_SUMMARY() test_summary()

#endif /* SCS_TEST_FRAMEWORK_H */

/*
 * framework.h - the minimal test framework shared by the test programs
 *
 * Copyright (c) 2026, Chris Oakman
 * Released under the ISC license
 * https://github.com/oakmac/standard-clojure-style-c/blob/master/LICENSE.md
 *
 * No external dependencies: a handful of macros that count assertions and
 * report failures. The output is modeled on the verbose reporter of jest, the
 * test runner of Standard Clojure Style JavaScript: a header per suite, one
 * line per test with a check mark or a cross and its duration, the failure
 * details under the failing test, and the totals at the end.
 *
 * A test program includes this header, defines tests with TEST(name), calls
 * TEST_BEGIN(argc, argv) in main(), groups tests with SUITE("name"), runs them
 * with RUN_TEST(name), and finishes with TEST_SUMMARY(), which prints the
 * totals and returns the exit code.
 *
 * Command line:
 *
 *   test_program [-q | --quiet] [filter]
 *
 * With a filter, only the tests whose name or suite name contains it are run.
 * With --quiet, nothing is printed unless a test fails; the failures and the
 * totals are then printed as usual. The exit code is 0 when every test
 * passed and 1 otherwise, in both modes.
 *
 * Colors follow the same rules as the CLI: FORCE_COLOR=0 or NO_COLOR disable
 * them, any other FORCE_COLOR value enables them, and otherwise they are used
 * when stdout is a terminal.
 */

#ifndef SCS_TEST_FRAMEWORK_H
#define SCS_TEST_FRAMEWORK_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <io.h>
#define test_stdout_is_tty() (_isatty(_fileno(stdout)) != 0)
#else
#include <unistd.h>
#define test_stdout_is_tty() (isatty(fileno(stdout)) != 0)
#endif

static int tests_run = 0;
static int tests_failed = 0;
static int assertions_run = 0;
static int assertions_failed = 0;
static int suites_run = 0;
static int suites_failed = 0;

static const char *test_program = "";
static clock_t test_program_start = 0;
static bool test_use_colors = false;

static const char *current_test = "";
static bool current_test_failed = false;
static clock_t current_test_start = 0;

/* The suite the next test belongs to. It counts as run once one of its tests
 * starts, and its header is printed at that point (or, in quiet mode, when one
 * of its tests fails), so a suite whose tests are all filtered out prints
 * nothing. */
static const char *current_suite = "";
static bool current_suite_started = false;
static bool current_suite_printed = false;
static bool current_suite_failed = false;

/* the names of the failed tests, repeated in the summary */
#define MAX_FAILED_NAMES 64
static char failed_names[MAX_FAILED_NAMES][256];
static int num_failed_names = 0;

/* when set (from the command line), only the tests whose name or suite name
 * contains it are run */
static const char *test_filter = NULL;

/* --quiet: print nothing but the failures and, when there are any, the totals */
static bool test_quiet = false;

/* UTF-8 */
#define TEST_CHECK_MARK "\xE2\x9C\x93"
#define TEST_CROSS "\xE2\x9C\x95"
#define TEST_BULLET "\xE2\x97\x8F"
#define TEST_CHEVRON "\xE2\x80\xBA"

/* ANSI styles, as opening and closing sequences */
#define TEST_BOLD "\x1b[1m", "\x1b[22m"
#define TEST_DIM "\x1b[2m", "\x1b[22m"
#define TEST_RED "\x1b[31m", "\x1b[39m"
#define TEST_GREEN "\x1b[32m", "\x1b[39m"
#define TEST_BOLD_RED "\x1b[1m\x1b[31m", "\x1b[39m\x1b[22m"
#define TEST_BOLD_GREEN "\x1b[1m\x1b[32m", "\x1b[39m\x1b[22m"
#define TEST_BADGE_RED "\x1b[7m\x1b[1m\x1b[31m", "\x1b[39m\x1b[22m\x1b[27m"
#define TEST_BADGE_GREEN "\x1b[7m\x1b[1m\x1b[32m", "\x1b[39m\x1b[22m\x1b[27m"

static void print_styled(const char *open, const char *close, const char *text) {
  if (test_use_colors) {
    printf("%s%s%s", open, text, close);
  } else {
    fputs(text, stdout);
  }
}

static bool should_run_test(const char *name) {
  return test_filter == NULL || strstr(name, test_filter) != NULL ||
         strstr(current_suite, test_filter) != NULL;
}

static void print_usage(FILE *out, const char *program) {
  fprintf(out, "Usage: %s [-q | --quiet] [filter]\n\n", program);
  fprintf(out, "  filter        run only the tests whose name or suite name contains it\n");
  fprintf(out, "  -q, --quiet   print nothing unless a test fails\n");
  fprintf(out, "  -h, --help    print this help\n");
}

/* program is the name shown in the header and the PASS / FAIL line */
static void test_begin(const char *program, int argc, char **argv) {
  test_program = program;
  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    if (strcmp(arg, "-q") == 0 || strcmp(arg, "--quiet") == 0) {
      test_quiet = true;
    } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
      print_usage(stdout, argv[0]);
      exit(0);
    } else if (arg[0] == '-' && arg[1] != '\0') {
      fprintf(stderr, "unknown option: %s\n\n", arg);
      print_usage(stderr, argv[0]);
      exit(2);
    } else if (test_filter != NULL) {
      fprintf(stderr, "only one filter is allowed\n\n");
      print_usage(stderr, argv[0]);
      exit(2);
    } else {
      test_filter = arg;
    }
  }
  const char *force = getenv("FORCE_COLOR");
  if (force != NULL) {
    test_use_colors = strcmp(force, "0") != 0;
  } else if (getenv("NO_COLOR") != NULL) {
    test_use_colors = false;
  } else {
    test_use_colors = test_stdout_is_tty();
  }
  test_program_start = clock();
  if (!test_quiet) {
    print_styled(TEST_BOLD, program);
    printf("\n\n");
  }
}

#define TEST_BEGIN(argc, argv) test_begin(__FILE__, argc, argv)

static void finish_suite(void) {
  if (current_suite_started && current_suite[0] != '\0') {
    suites_run++;
    if (current_suite_failed) {
      suites_failed++;
    }
  }
  current_suite_started = false;
  current_suite_printed = false;
  current_suite_failed = false;
}

static void test_suite(const char *name) {
  finish_suite();
  current_suite = name;
}

#define SUITE(name) test_suite(name)

static const char *test_indent(void) {
  return current_suite[0] != '\0' ? "    " : "  ";
}

static void print_suite_header(void) {
  if (!current_suite_printed) {
    if (current_suite[0] != '\0') {
      printf("  %s\n", current_suite);
    }
    current_suite_printed = true;
  }
}

static void start_test(const char *name) {
  current_suite_started = true;
  if (!test_quiet) {
    print_suite_header();
  }
  current_test = name;
  current_test_failed = false;
  current_test_start = clock();
  tests_run++;
  fflush(stdout);
}

static double elapsed_ms(clock_t since) {
  return (double)(clock() - since) * 1000.0 / (double)CLOCKS_PER_SEC;
}

static void finish_test(void) {
  double ms = elapsed_ms(current_test_start);
  if (current_test_failed) {
    tests_failed++;
    current_suite_failed = true;
    if (num_failed_names < MAX_FAILED_NAMES) {
      if (current_suite[0] != '\0') {
        snprintf(failed_names[num_failed_names], sizeof(failed_names[0]), "%s " TEST_CHEVRON " %s",
                 current_suite, current_test);
      } else {
        snprintf(failed_names[num_failed_names], sizeof(failed_names[0]), "%s", current_test);
      }
    }
    num_failed_names++;
    return;
  }
  if (test_quiet) {
    return;
  }
  fputs(test_indent(), stdout);
  print_styled(TEST_GREEN, TEST_CHECK_MARK);
  printf(" %s", current_test);
  if (ms >= 0.5) {
    char duration[32];
    snprintf(duration, sizeof(duration), " (%.0f ms)", ms);
    print_styled(TEST_DIM, duration);
  }
  printf("\n");
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
  if (!current_test_failed) {
    current_test_failed = true;
    print_suite_header();
    fputs(test_indent(), stdout);
    print_styled(TEST_RED, TEST_CROSS);
    printf(" %s\n", current_test);
  }
  printf("%s  %s:%d: %s\n", test_indent(), file, line, msg);
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

/* "1 failed, 2 passed, 3 total" in the colors of jest */
static void print_counts(int failed, int passed, int total) {
  char text[64];
  if (failed > 0) {
    snprintf(text, sizeof(text), "%d failed", failed);
    print_styled(TEST_BOLD_RED, text);
    printf(", ");
  }
  if (passed > 0) {
    snprintf(text, sizeof(text), "%d passed", passed);
    print_styled(TEST_BOLD_GREEN, text);
    printf(", ");
  }
  printf("%d total\n", total);
}

/* prints the totals; returns the process exit code */
static int test_summary(void) {
  finish_suite();
  if (tests_run == 0) {
    printf("no tests matched \"%s\"\n", test_filter ? test_filter : "");
    return 1;
  }
  if (test_quiet && tests_failed == 0) {
    return 0;
  }
  double seconds = elapsed_ms(test_program_start) / 1000.0;

  printf("\n");
  if (tests_failed > 0) {
    for (int i = 0; i < num_failed_names && i < MAX_FAILED_NAMES; i++) {
      printf("  ");
      print_styled(TEST_BOLD_RED, TEST_BULLET);
      printf(" %s\n", failed_names[i]);
    }
    if (num_failed_names > MAX_FAILED_NAMES) {
      printf("  ... and %d more\n", num_failed_names - MAX_FAILED_NAMES);
    }
    printf("\n");
  }

  if (tests_failed > 0) {
    print_styled(TEST_BADGE_RED, " FAIL ");
  } else {
    print_styled(TEST_BADGE_GREEN, " PASS ");
  }
  printf(" %s\n\n", test_program);

  print_styled(TEST_BOLD, "Test Suites: ");
  print_counts(suites_failed, suites_run - suites_failed, suites_run);
  print_styled(TEST_BOLD, "Tests:       ");
  print_counts(tests_failed, tests_run - tests_failed, tests_run);
  print_styled(TEST_BOLD, "Assertions:  ");
  print_counts(assertions_failed, assertions_run - assertions_failed, assertions_run);
  print_styled(TEST_BOLD, "Time:        ");
  printf("%.3f s\n", seconds);
  char ran[320];
  if (test_filter != NULL) {
    snprintf(ran, sizeof(ran), "Ran all tests matching \"%s\".", test_filter);
  } else {
    snprintf(ran, sizeof(ran), "Ran all tests.");
  }
  print_styled(TEST_DIM, ran);
  printf("\n\n");
  return tests_failed == 0 ? 0 : 1;
}

#define TEST_SUMMARY() test_summary()

#endif /* SCS_TEST_FRAMEWORK_H */

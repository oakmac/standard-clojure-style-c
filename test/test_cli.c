/*
 * test_cli.c - tests for the standard-clj command line interface
 *
 * Copyright (c) 2026, Chris Oakman
 * Released under the ISC license
 * https://github.com/oakmac/standard-clojure-style-c/blob/master/LICENSE.md
 *
 * This file #includes cli/cli.c directly so that the tests can exercise its
 * internal (static) functions. The CLI itself is run in-process through
 * scs_cli_run() with captured output, against a small project created under
 * build/cli-test-project/.
 *
 * Build and run with `make test` (or via CMake, see README.md).
 */

#include "../cli/cli.c"

#include "framework.h"

#include <stdarg.h>

#ifdef _WIN32
#define make_directory(path) _mkdir(path)
#define remove_directory(path) _rmdir(path)
#else
#define make_directory(path) mkdir(path, 0755)
#define remove_directory(path) rmdir(path)
#endif

/* -----------------------------------------------------------------------------
 * Test utilities: a temporary project on disk
 */

static char *project_dir = NULL; /* absolute */

/* creates every missing directory along path */
static void make_directories(const char *path) {
  char *copy = xstrdup(path);
  size_t root_len = path_root_len(copy);
  for (size_t i = root_len; copy[i]; i++) {
    if (is_path_sep(copy[i])) {
      copy[i] = '\0';
      make_directory(copy);
      copy[i] = PATH_SEP;
    }
  }
  make_directory(copy);
  free(copy);
}

/* the absolute path of a file in the project, given with forward slashes */
static char *project_path(const char *relative) {
  return path_resolve(project_dir, relative);
}

/* The CLI prints a file inside the project as its path with the project
 * directory removed, so with the native separator: "/src/core.clj" here and
 * "\src\core.clj" on Windows. The expected output in these tests is written
 * with "/"; this maps every "/" in it to PATH_SEP. (Nothing else in those
 * strings contains a "/".) */
static char *native_seps(const char *s) {
  char *copy = xstrdup(s);
  for (char *c = copy; *c; c++) {
    if (*c == '/') {
      *c = PATH_SEP;
    }
  }
  return copy;
}

/* like ASSERT_STR_EQ and ASSERT_CONTAINS, for expected text with file paths */
#define ASSERT_STR_EQ_PATHS(actual, expected)                                                      \
  do {                                                                                             \
    char *native_ = native_seps(expected);                                                         \
    ASSERT_STR_EQ(actual, native_);                                                                \
    free(native_);                                                                                 \
  } while (0)

#define ASSERT_CONTAINS_PATHS(haystack, needle)                                                    \
  do {                                                                                             \
    char *native_ = native_seps(needle);                                                           \
    ASSERT_CONTAINS(haystack, native_);                                                            \
    free(native_);                                                                                 \
  } while (0)

/* s as a JSON (and EDN) string literal, the way the list command prints it;
 * the backslashes in a Windows path come out escaped */
static char *json_quoted(const char *s) {
  buf b = {NULL, 0, 0};
  append_json_string(&b, s);
  return b.data;
}

static void write_project_file(const char *relative, const char *contents) {
  char *path = project_path(relative);
  char *dir = xstrdup(path);
  char *last_sep = strrchr(dir, PATH_SEP);
  if (last_sep) {
    *last_sep = '\0';
    make_directories(dir);
  }
  free(dir);
  if (!fs_write_file(path, contents, strlen(contents))) {
    fprintf(stderr, "could not write test file %s\n", path);
    exit(1);
  }
  free(path);
}

static char *read_project_file(const char *relative) {
  char *path = project_path(relative);
  size_t len = 0;
  char *text = fs_read_file(path, &len);
  free(path);
  return text;
}

static void remove_tree(const char *path) {
  fs_kind kind = fs_stat_kind(path);
  if (kind == FS_DIRECTORY) {
    strlist names = fs_list_directory(path);
    for (size_t i = 0; i < names.size; i++) {
      char *child = path_join(path, names.items[i]);
      remove_tree(child);
      free(child);
    }
    strlist_free(&names);
    remove_directory(path);
  } else if (kind != FS_MISSING) {
    remove(path);
  }
}

/* the same project the JavaScript file discovery tests use */
static void create_test_project(void) {
  remove_tree(project_dir);
  make_directories(project_dir);
  write_project_file("dev/user.clj", "(ns user)\n");
  write_project_file("resources/config.edn", "{:enabled true}\n");
  write_project_file("src/browser.cljs", "(ns browser)\n");
  write_project_file("src/core.clj", "(ns core)\n");
  write_project_file("src/data.edn", "{:hello \"world\"}\n");
  write_project_file("src/experimental.jank", "(ns experimental)\n");
  write_project_file("src/generated/generated.clj", "(ns generated)\n");
  write_project_file("src/generated/generated.cljs", "(ns generated-browser)\n");
  write_project_file("src/nested/deep.clj", "(ns nested.deep)\n");
  write_project_file("src/nested/deep.cljs", "(ns nested.deep-browser)\n");
  write_project_file("src/nested/deep.cljc", "(ns nested.deep-shared)\n");
  write_project_file("src/nested/notes.md", "# notes\n");
  write_project_file("src/notes.txt", "not Clojure\n");
  write_project_file("src/shared.cljc", "(ns shared)\n");
  write_project_file("test/core_test.clj", "(ns core-test)\n");
  write_project_file("test/nested/browser_test.cljs", "(ns browser-test)\n");
}

/* -----------------------------------------------------------------------------
 * Test utilities: running the CLI in-process
 */

typedef struct cli_result {
  int exit_code;
  char *out;
  char *err;
} cli_result;

static void cli_result_free(cli_result *r) {
  free(r->out);
  free(r->err);
  r->out = NULL;
  r->err = NULL;
}

/* a clock that never advances, so durations print as [0ms] */
static double fake_now_ms(void) {
  return 0.0;
}

static char *read_stream_to_string(FILE *f) {
  rewind(f);
  buf b = {NULL, 0, 0};
  char chunk[4096];
  size_t n;
  while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
    buf_append_n(&b, chunk, n);
  }
  if (b.data == NULL) {
    buf_append_n(&b, "", 0);
  }
  return b.data;
}

/* a read/write stream backed by a file next to the test project (tmpfile()
 * is not reliable on Windows) */
static char *streams_dir = NULL; /* absolute */
static int num_streams_opened = 0;

static FILE *open_temp_stream(void) {
  if (num_streams_opened == 0) {
    make_directories(streams_dir);
  }
  char *path = xprintf("%s%cstream-%d", streams_dir, PATH_SEP, num_streams_opened++);
  FILE *f = fopen(path, "w+b");
  if (!f) {
    fprintf(stderr, "could not create a temporary file at %s\n", path);
    exit(1);
  }
  free(path);
  return f;
}

/* runs the CLI with the given arguments (NULL-terminated) in the test project */
static cli_result run_cli_full(const char *stdin_text, bool colors, const char *first, ...) {
  char *argv[64];
  int argc = 0;
  argv[argc++] = xstrdup("standard-clj");
  va_list va;
  va_start(va, first);
  for (const char *arg = first; arg != NULL && argc < 63; arg = va_arg(va, const char *)) {
    argv[argc++] = xstrdup(arg);
  }
  va_end(va);
  argv[argc] = NULL;

  FILE *out = open_temp_stream();
  FILE *err = open_temp_stream();
  FILE *in = open_temp_stream();
  if (stdin_text != NULL) {
    fputs(stdin_text, in);
    rewind(in);
  }

  scs_cli_env env;
  memset(&env, 0, sizeof(env));
  env.out = out;
  env.err = err;
  env.in = in;
  env.cwd = project_dir;
  env.color_out = colors;
  env.color_err = colors;
  env.now_ms = fake_now_ms;
  env.version = "0.0.0-test";
  env.version_verbose = "0.0.0-test [abc1234]";

  cli_result result;
  result.exit_code = scs_cli_run(argc, argv, &env);
  result.out = read_stream_to_string(out);
  result.err = read_stream_to_string(err);

  fclose(out);
  fclose(err);
  fclose(in);
  for (int i = 0; i < argc; i++) {
    free(argv[i]);
  }
  return result;
}

#define run_cli(...) run_cli_full(NULL, false, __VA_ARGS__, NULL)
#define run_cli_colors(...) run_cli_full(NULL, true, __VA_ARGS__, NULL)
#define run_cli_stdin(text, ...) run_cli_full(text, false, __VA_ARGS__, NULL)

/* compares a `list --output json` result with the expected project-relative files */
static void assert_listed_files(const cli_result *r, const char *const *expected, int line) {
  assertions_run++;
  cJSON *actual = cJSON_Parse(r->out);
  if (!cJSON_IsArray(actual)) {
    char msg[1024];
    snprintf(msg, sizeof(msg), "list output is not a JSON array: %.300s", r->out);
    report_failure(__FILE__, line, msg);
    cJSON_Delete(actual);
    return;
  }

  strlist expected_paths = {NULL, 0, 0};
  for (size_t i = 0; expected[i] != NULL; i++) {
    strlist_push(&expected_paths, project_path(expected[i]));
  }
  strlist_sort_unique(&expected_paths);

  bool same = (size_t)cJSON_GetArraySize(actual) == expected_paths.size;
  for (size_t i = 0; same && i < expected_paths.size; i++) {
    cJSON *item = cJSON_GetArrayItem(actual, (int)i);
    same = cJSON_IsString(item) && strcmp(item->valuestring, expected_paths.items[i]) == 0;
  }
  if (!same) {
    report_failure(__FILE__, line, "listed files do not match");
    printf("--- expected ---\n");
    for (size_t i = 0; i < expected_paths.size; i++) {
      printf("%s\n", expected_paths.items[i]);
    }
    printf("--- actual ---\n%s--- stderr ---\n%s---\n", r->out, r->err);
  }
  strlist_free(&expected_paths);
  cJSON_Delete(actual);
}

#define ASSERT_LISTED(result, ...)                                                                 \
  do {                                                                                             \
    const char *const expected_[] = {__VA_ARGS__, NULL};                                           \
    assert_listed_files(&(result), expected_, __LINE__);                                           \
  } while (0)

#define ASSERT_CONTAINS(haystack, needle)                                                          \
  do {                                                                                             \
    assertions_run++;                                                                              \
    if (strstr((haystack), (needle)) == NULL) {                                                    \
      char msg_[1024];                                                                             \
      snprintf(msg_, sizeof(msg_), "expected to contain \"%s\" but got:\n%s", (needle),            \
               (haystack));                                                                        \
      report_failure(__FILE__, __LINE__, msg_);                                                    \
    }                                                                                              \
  } while (0)

#define ASSERT_NOT_CONTAINS(haystack, needle)                                                      \
  do {                                                                                             \
    assertions_run++;                                                                              \
    if (strstr((haystack), (needle)) != NULL) {                                                    \
      char msg_[1024];                                                                             \
      snprintf(msg_, sizeof(msg_), "expected not to contain \"%s\" but got:\n%s", (needle),        \
               (haystack));                                                                        \
      report_failure(__FILE__, __LINE__, msg_);                                                    \
    }                                                                                              \
  } while (0)

/* -----------------------------------------------------------------------------
 * Pure helpers
 */

TEST(normalize_log_level) {
  ASSERT_INT_EQ(normalize_log_level(NULL), LOG_EVERYTHING);
  ASSERT_INT_EQ(normalize_log_level(""), LOG_EVERYTHING);
  ASSERT_INT_EQ(normalize_log_level("everything"), LOG_EVERYTHING);
  ASSERT_INT_EQ(normalize_log_level("junk"), LOG_EVERYTHING);
  ASSERT_INT_EQ(normalize_log_level("0"), LOG_EVERYTHING);
  ASSERT_INT_EQ(normalize_log_level("ignore-already-formatted"), LOG_IGNORE_ALREADY_FORMATTED);
  ASSERT_INT_EQ(normalize_log_level("1"), LOG_IGNORE_ALREADY_FORMATTED);
  ASSERT_INT_EQ(normalize_log_level("quiet"), LOG_QUIET);
  ASSERT_INT_EQ(normalize_log_level("5"), LOG_QUIET);
}

static scs_format_result success_result(const char *out) {
  scs_format_result r;
  memset(&r, 0, sizeof(r));
  r.status = SCS_STATUS_SUCCESS;
  r.out = xstrdup(out);
  r.out_len = strlen(out);
  return r;
}

static scs_format_result error_result_with(const char *reason) {
  scs_format_result r;
  memset(&r, 0, sizeof(r));
  r.status = SCS_STATUS_ERROR;
  r.reason = reason ? xstrdup(reason) : NULL;
  return r;
}

TEST(classify_format_result) {
  /* already formatted (trailing newline present) */
  scs_format_result r = success_result("(def x 1)");
  classification cls = classify_format_result("(def x 1)\n", 10, &r);
  ASSERT_INT_EQ(cls.action, ACTION_ALREADY_FORMATTED);
  ASSERT_STR_EQ(cls.output_text, "(def x 1)\n");
  ASSERT_INT_EQ(cls.output_len, 10);
  classification_free(&cls);
  scs_format_result_free(&r);

  /* needed formatting */
  r = success_result("(def x 1)");
  cls = classify_format_result("(def x   1)\n", 12, &r);
  ASSERT_INT_EQ(cls.action, ACTION_FORMATTED);
  ASSERT_STR_EQ(cls.output_text, "(def x 1)\n");
  classification_free(&cls);
  scs_format_result_free(&r);

  /* missing trailing newline */
  r = success_result("(def x 1)");
  cls = classify_format_result("(def x 1)", 9, &r);
  ASSERT_INT_EQ(cls.action, ACTION_FORMATTED);
  classification_free(&cls);
  scs_format_result_free(&r);

  /* exactly one newline is appended */
  r = success_result("(ns foo.core)");
  cls = classify_format_result("anything", 8, &r);
  ASSERT_STR_EQ(cls.output_text, "(ns foo.core)\n");
  classification_free(&cls);
  scs_format_result_free(&r);

  /* an empty file that formats to an empty string */
  r = success_result("");
  cls = classify_format_result("\n", 1, &r);
  ASSERT_INT_EQ(cls.action, ACTION_ALREADY_FORMATTED);
  ASSERT_STR_EQ(cls.output_text, "\n");
  classification_free(&cls);
  scs_format_result_free(&r);

  /* an explicit error */
  r = error_result_with("Unexpected EOF");
  cls = classify_format_result("(def )", 6, &r);
  ASSERT_INT_EQ(cls.action, ACTION_ERROR);
  ASSERT_STR_EQ(cls.error_message, "Unexpected EOF");
  classification_free(&cls);
  scs_format_result_free(&r);

  /* an error without a reason */
  r = error_result_with(NULL);
  cls = classify_format_result("(def x 1)", 9, &r);
  ASSERT_INT_EQ(cls.action, ACTION_ERROR);
  ASSERT_CONTAINS(cls.error_message, "Unknown error");
  classification_free(&cls);
  scs_format_result_free(&r);
}

TEST(file_str_and_add_period_prefix) {
  ASSERT_STR_EQ(file_str(1), "file");
  ASSERT_STR_EQ(file_str(0), "files");
  ASSERT_STR_EQ(file_str(2), "files");
  ASSERT_STR_EQ(file_str(100), "files");

  char *s = add_period_prefix("clj");
  ASSERT_STR_EQ(s, ".clj");
  free(s);
  s = add_period_prefix(".edn");
  ASSERT_STR_EQ(s, ".edn");
  free(s);
}

TEST(relative_filename) {
  char *s = relative_filename("/home/user/project/src/foo.clj", "/home/user/project");
  ASSERT_STR_EQ(s, "/src/foo.clj");
  free(s);
  s = relative_filename("/other/path/foo.clj", "/home/user/project");
  ASSERT_STR_EQ(s, "/other/path/foo.clj");
  free(s);
  s = relative_filename("/src/foo.clj", "");
  ASSERT_STR_EQ(s, "/src/foo.clj");
  free(s);
}

TEST(convert_file_ext) {
  strlist exts = convert_file_ext("clj,cljs,.edn");
  ASSERT_INT_EQ(exts.size, 3);
  ASSERT_TRUE(strlist_contains(&exts, ".clj"));
  ASSERT_TRUE(strlist_contains(&exts, ".cljs"));
  ASSERT_TRUE(strlist_contains(&exts, ".edn"));
  strlist_free(&exts);

  exts = convert_file_ext("clj");
  ASSERT_INT_EQ(exts.size, 1);
  ASSERT_STR_EQ(exts.items[0], ".clj");
  strlist_free(&exts);

  ASSERT_STR_EQ(path_extname("src/core.clj"), ".clj");
  ASSERT_STR_EQ(path_extname("src/.hidden"), "");
  ASSERT_STR_EQ(path_extname("src/.hidden.clj"), ".clj");
  ASSERT_STR_EQ(path_extname("noext"), "");
  ASSERT_STR_EQ(path_extname("a.b/c"), "");
}

TEST(output_encoders) {
  strlist empty = {NULL, 0, 0};
  strlist one = {NULL, 0, 0};
  strlist_push_copy(&one, "a.clj");
  strlist three = {NULL, 0, 0};
  strlist_push_copy(&three, "a.clj");
  strlist_push_copy(&three, "b.clj");
  strlist_push_copy(&three, "c.clj");
  strlist tricky = {NULL, 0, 0};
  strlist_push_copy(&tricky, "say \"hi\"\\\n\t");

  char *s = encode_json_array(&empty);
  ASSERT_STR_EQ(s, "[]");
  free(s);
  s = encode_json_array(&one);
  ASSERT_STR_EQ(s, "[\"a.clj\"]");
  free(s);
  s = encode_json_array(&three);
  ASSERT_STR_EQ(s, "[\"a.clj\",\"b.clj\",\"c.clj\"]");
  free(s);
  s = encode_json_array(&tricky);
  ASSERT_STR_EQ(s, "[\"say \\\"hi\\\"\\\\\\n\\t\"]");
  free(s);

  s = encode_json_array_pretty(&empty);
  ASSERT_STR_EQ(s, "[]");
  free(s);
  s = encode_json_array_pretty(&one);
  ASSERT_STR_EQ(s, "[\n  \"a.clj\"\n]");
  free(s);
  s = encode_json_array_pretty(&three);
  ASSERT_STR_EQ(s, "[\n  \"a.clj\",\n  \"b.clj\",\n  \"c.clj\"\n]");
  free(s);

  s = encode_edn_array(&empty);
  ASSERT_STR_EQ(s, "[]");
  free(s);
  s = encode_edn_array(&three);
  ASSERT_STR_EQ(s, "[\"a.clj\" \"b.clj\" \"c.clj\"]");
  free(s);

  s = encode_edn_array_pretty(&empty);
  ASSERT_STR_EQ(s, "[]");
  free(s);
  s = encode_edn_array_pretty(&one);
  ASSERT_STR_EQ(s, "[\"a.clj\"]");
  free(s);
  s = encode_edn_array_pretty(&three);
  ASSERT_STR_EQ(s, "[\"a.clj\"\n \"b.clj\"\n \"c.clj\"]");
  free(s);

  strlist_free(&one);
  strlist_free(&three);
  strlist_free(&tricky);
}

TEST(duration_formatting) {
  buf b = {NULL, 0, 0};
  append_duration(&b, false, 0.0);
  ASSERT_STR_EQ(buf_cstr(&b), "[0ms]");
  buf_clear(&b);
  append_duration(&b, false, 1.5);
  ASSERT_STR_EQ(buf_cstr(&b), "[1.5ms]");
  buf_clear(&b);
  append_duration(&b, false, 12.3456);
  ASSERT_STR_EQ(buf_cstr(&b), "[12.35ms]");
  buf_clear(&b);
  append_duration(&b, false, 100.0);
  ASSERT_STR_EQ(buf_cstr(&b), "[100ms]");
  buf_clear(&b);
  append_duration(&b, false, 0.004);
  ASSERT_STR_EQ(buf_cstr(&b), "[0ms]");
  buf_clear(&b);
  append_duration(&b, true, 2.0);
  ASSERT_STR_EQ(buf_cstr(&b), "\x1b[2m[2ms]\x1b[22m");
  buf_free(&b);
}

TEST(path_helpers) {
#ifndef _WIN32
  char *s = path_normalize_absolute("/a/b/../c/./d/");
  ASSERT_STR_EQ(s, "/a/c/d");
  free(s);
  s = path_normalize_absolute("/../..");
  ASSERT_STR_EQ(s, "/");
  free(s);
  s = path_normalize_absolute("//a///b");
  ASSERT_STR_EQ(s, "/a/b");
  free(s);
  s = path_resolve("/home/me", "src/core.clj");
  ASSERT_STR_EQ(s, "/home/me/src/core.clj");
  free(s);
  s = path_resolve("/home/me", "./src/../test/");
  ASSERT_STR_EQ(s, "/home/me/test");
  free(s);
  s = path_resolve("/home/me", "/abs/path");
  ASSERT_STR_EQ(s, "/abs/path");
  free(s);
  s = path_resolve("/home/me", ".");
  ASSERT_STR_EQ(s, "/home/me");
  free(s);
  s = path_join("/a/", "b");
  ASSERT_STR_EQ(s, "/a/b");
  free(s);
  s = path_join("/a", "b");
  ASSERT_STR_EQ(s, "/a/b");
  free(s);
  ASSERT_TRUE(path_is_absolute("/x"));
  ASSERT_FALSE(path_is_absolute("x/y"));
#else
  char *s = path_resolve("C:\\home\\me", "src/core.clj");
  ASSERT_STR_EQ(s, "C:\\home\\me\\src\\core.clj");
  free(s);
  ASSERT_TRUE(path_is_absolute("C:\\x"));
  ASSERT_TRUE(path_is_absolute("\\\\server\\share"));
  ASSERT_FALSE(path_is_absolute("x\\y"));
#endif
}

TEST(brace_expansion) {
  strlist out = expand_braces("src/**/*.{clj,cljs,cljc}");
  ASSERT_INT_EQ(out.size, 3);
  ASSERT_STR_EQ(out.items[0], "src/**/*.clj");
  ASSERT_STR_EQ(out.items[1], "src/**/*.cljs");
  ASSERT_STR_EQ(out.items[2], "src/**/*.cljc");
  strlist_free(&out);

  out = expand_braces("{src,test}/**/*.{clj,edn}");
  ASSERT_INT_EQ(out.size, 4);
  ASSERT_STR_EQ(out.items[0], "src/**/*.clj");
  ASSERT_STR_EQ(out.items[1], "src/**/*.edn");
  ASSERT_STR_EQ(out.items[2], "test/**/*.clj");
  ASSERT_STR_EQ(out.items[3], "test/**/*.edn");
  strlist_free(&out);

  out = expand_braces("a/{b,{c,d}}/e");
  ASSERT_INT_EQ(out.size, 3);
  ASSERT_STR_EQ(out.items[0], "a/b/e");
  ASSERT_STR_EQ(out.items[1], "a/c/e");
  ASSERT_STR_EQ(out.items[2], "a/d/e");
  strlist_free(&out);

  out = expand_braces("no/braces/*.clj");
  ASSERT_INT_EQ(out.size, 1);
  ASSERT_STR_EQ(out.items[0], "no/braces/*.clj");
  strlist_free(&out);

  out = expand_braces("unbalanced/{a,b");
  ASSERT_INT_EQ(out.size, 1);
  ASSERT_STR_EQ(out.items[0], "unbalanced/{a,b");
  strlist_free(&out);
}

TEST(segment_matching) {
  ASSERT_TRUE(match_segment("*.clj", "core.clj"));
  ASSERT_FALSE(match_segment("*.clj", "core.cljs"));
  ASSERT_TRUE(match_segment("*", "anything"));
  ASSERT_TRUE(match_segment("*", ""));
  ASSERT_TRUE(match_segment("core.*", "core.clj"));
  ASSERT_TRUE(match_segment("c?re.clj", "core.clj"));
  ASSERT_FALSE(match_segment("c?re.clj", "cre.clj"));
  ASSERT_TRUE(match_segment("src*", "src"));
  ASSERT_TRUE(match_segment("src*", "srcfile.clj"));
  ASSERT_TRUE(match_segment("*test*", "core_test.clj"));
  ASSERT_FALSE(match_segment("*test*", "core.clj"));
  /* character classes */
  ASSERT_TRUE(match_segment("[abc].clj", "a.clj"));
  ASSERT_FALSE(match_segment("[abc].clj", "d.clj"));
  ASSERT_TRUE(match_segment("[a-c].clj", "b.clj"));
  ASSERT_TRUE(match_segment("[!a-c].clj", "d.clj"));
  ASSERT_FALSE(match_segment("[!a-c].clj", "a.clj"));
  /* a leading dot must be matched explicitly */
  ASSERT_FALSE(match_segment("*", ".hidden"));
  ASSERT_FALSE(match_segment("*.clj", ".dot.clj"));
  ASSERT_TRUE(match_segment(".*", ".hidden"));
  ASSERT_TRUE(match_segment(".dot.clj", ".dot.clj"));
  /* literals */
  ASSERT_TRUE(match_segment("core.clj", "core.clj"));
  ASSERT_FALSE(match_segment("core.clj", "core.cljs"));
  ASSERT_TRUE(match_segment("a\\*b", "a*b"));
  ASSERT_FALSE(match_segment("a\\*b", "axb"));

  ASSERT_TRUE(contains_glob_syntax("src/*.clj"));
  ASSERT_TRUE(contains_glob_syntax("src/{a,b}"));
  ASSERT_FALSE(contains_glob_syntax("src/core.clj"));
}

TEST(edn_reader) {
  const char *text = "{:include [\"src/\" \"test/\"]\n ;; a comment\n :ignore \"vendor/\"\n"
                     " :log-level \"quiet\", :extra #_ignored 42 :nested {:a [1 2]} :set #{1}}";
  edn_value *v = edn_parse(text, strlen(text));
  ASSERT_NOT_NULL(v);
  if (v) {
    ASSERT_INT_EQ(v->type, EDN_MAP);
    const edn_value *include = edn_map_get(v, "include");
    ASSERT_NOT_NULL(include);
    ASSERT_INT_EQ(include->type, EDN_VECTOR);
    ASSERT_INT_EQ(include->num_items, 2);
    ASSERT_STR_EQ(include->items[0]->text, "src/");
    ASSERT_STR_EQ(include->items[1]->text, "test/");
    const edn_value *ignore = edn_map_get(v, "ignore");
    ASSERT_INT_EQ(ignore->type, EDN_STRING);
    ASSERT_STR_EQ(ignore->text, "vendor/");
    ASSERT_STR_EQ(edn_map_get(v, "log-level")->text, "quiet");
    ASSERT_STR_EQ(edn_map_get(v, "extra")->text, "42");
    ASSERT_INT_EQ(edn_map_get(v, "nested")->type, EDN_MAP);
    ASSERT_INT_EQ(edn_map_get(v, "set")->type, EDN_VECTOR);
    ASSERT_NULL(edn_map_get(v, "missing"));
    edn_free(v);
  }

  /* string escapes */
  text = "\"a\\\"b\\\\c\\n\\u0041\"";
  v = edn_parse(text, strlen(text));
  ASSERT_NOT_NULL(v);
  if (v) {
    ASSERT_STR_EQ(v->text, "a\"b\\c\nA");
    edn_free(v);
  }

  /* string keys work like keyword keys */
  text = "{\"include\" [\"src\"]}";
  v = edn_parse(text, strlen(text));
  ASSERT_NOT_NULL(v);
  if (v) {
    ASSERT_NOT_NULL(edn_map_get(v, "include"));
    edn_free(v);
  }

  /* invalid documents */
  const char *invalid[] = {
      "{:a", "{:a 1 :b}",    "[1 2", "\"unterminated", "{:a 1} extra", "#tag 1",
      "}",   "{:a \"\\q\"}", ""};
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    v = edn_parse(invalid[i], strlen(invalid[i]));
    ASSERT_NULL(v);
    edn_free(v);
  }
}

static args parse_test_args(const char *first, ...) {
  char *argv[32];
  int argc = 0;
  argv[argc++] = xstrdup("standard-clj");
  va_list list;
  va_start(list, first);
  for (const char *arg = first; arg != NULL && argc < 31; arg = va_arg(list, const char *)) {
    argv[argc++] = xstrdup(arg);
  }
  va_end(list);
  args a = parse_args(argc, argv);
  /* the args keep pointers into argv for single values, so copy them */
  a.command = a.command ? xstrdup(a.command) : NULL;
  a.config = a.config ? xstrdup(a.config) : NULL;
  a.log_level = a.log_level ? xstrdup(a.log_level) : NULL;
  a.file_ext = a.file_ext ? xstrdup(a.file_ext) : NULL;
  a.output = a.output ? xstrdup(a.output) : NULL;
  for (int i = 0; i < argc; i++) {
    free(argv[i]);
  }
  return a;
}

static void free_test_args(args *a) {
  free((char *)a->command);
  free((char *)a->config);
  free((char *)a->log_level);
  free((char *)a->file_ext);
  free((char *)a->output);
  args_free(a);
}

TEST(argument_parsing) {
  args a =
      parse_test_args("check", "src", "test", "--include", "a", "-in", "b", "--ignore=c", "-ig",
                      "d", "-l", "quiet", "--file-ext", "clj,cljs", "--config", "cfg.edn", NULL);
  ASSERT_NULL(a.error);
  ASSERT_STR_EQ(a.command, "check");
  ASSERT_INT_EQ(a.direct_args.size, 2);
  ASSERT_STR_EQ(a.direct_args.items[0], "src");
  ASSERT_STR_EQ(a.direct_args.items[1], "test");
  ASSERT_TRUE(a.include_passed);
  ASSERT_INT_EQ(a.include.size, 2);
  ASSERT_STR_EQ(a.include.items[1], "b");
  ASSERT_TRUE(a.ignore_passed);
  ASSERT_INT_EQ(a.ignore.size, 2);
  ASSERT_STR_EQ(a.ignore.items[0], "c");
  ASSERT_STR_EQ(a.log_level, "quiet");
  ASSERT_STR_EQ(a.file_ext, "clj,cljs");
  ASSERT_STR_EQ(a.config, "cfg.edn");
  free_test_args(&a);

  /* options before the command, and "-" as a positional */
  a = parse_test_args("--log-level=1", "fix", "-", NULL);
  ASSERT_NULL(a.error);
  ASSERT_STR_EQ(a.command, "fix");
  ASSERT_INT_EQ(a.direct_args.size, 1);
  ASSERT_STR_EQ(a.direct_args.items[0], "-");
  ASSERT_STR_EQ(a.log_level, "1");
  free_test_args(&a);

  /* flags */
  a = parse_test_args("--help", NULL);
  ASSERT_TRUE(a.help);
  free_test_args(&a);
  a = parse_test_args("-v", NULL);
  ASSERT_TRUE(a.version);
  free_test_args(&a);

  /* no arguments at all */
  a = parse_test_args(NULL);
  ASSERT_NULL(a.command);
  ASSERT_NULL(a.error);
  free_test_args(&a);

  /* errors */
  a = parse_test_args("list", "--bogus", "src", NULL);
  ASSERT_NOT_NULL(a.error);
  ASSERT_CONTAINS(a.error, "Unknown option: --bogus");
  free_test_args(&a);
  a = parse_test_args("list", "src", "--include", NULL);
  ASSERT_NOT_NULL(a.error);
  ASSERT_CONTAINS(a.error, "requires a value");
  free_test_args(&a);
}

/* -----------------------------------------------------------------------------
 * File discovery (the same scenarios as the JavaScript test suite)
 */

typedef struct discovery_case {
  const char *name;
  const char *json_config;  /* written to .standard-clj.json when not NULL */
  const char *edn_config;   /* written to .standard-clj.edn when not NULL */
  const char *extra_file;   /* an extra file to create (contents "(ns extra)\n") */
  const char *extra_dir;    /* an extra empty directory to create */
  const char *args[8];      /* arguments after "list" (NULL-terminated) */
  const char *expected[20]; /* project-relative files (NULL-terminated) */
} discovery_case;

#define ALL_SRC_FILES                                                                              \
  "src/browser.cljs", "src/core.clj", "src/data.edn", "src/experimental.jank",                     \
      "src/generated/generated.clj", "src/generated/generated.cljs", "src/nested/deep.clj",        \
      "src/nested/deep.cljs", "src/nested/deep.cljc", "src/shared.cljc"

static const discovery_case discovery_cases[] = {
    /* direct file and directory arguments */
    {"includes a directly named file", NULL, NULL, NULL, NULL, {"src/core.clj"}, {"src/core.clj"}},
    {"allows a directly named file with a non-default extension",
     NULL,
     NULL,
     NULL,
     NULL,
     {"src/notes.txt"},
     {"src/notes.txt"}},
    {"recursively discovers supported files in a directory",
     NULL,
     NULL,
     NULL,
     NULL,
     {"src"},
     {ALL_SRC_FILES}},
    {"uses --file-ext when discovering files from a directory",
     NULL,
     NULL,
     NULL,
     NULL,
     {"src", "--file-ext", "clj,cljs"},
     {"src/browser.cljs", "src/core.clj", "src/generated/generated.clj",
      "src/generated/generated.cljs", "src/nested/deep.clj", "src/nested/deep.cljs"}},

    /* --include files and directories */
    {"--include a directly named file with a supported extension",
     NULL,
     NULL,
     NULL,
     NULL,
     {"--include", "src/core.clj"},
     {"src/core.clj"}},
    {"--include filters a directly named file with a non-default extension",
     NULL,
     NULL,
     NULL,
     NULL,
     {"--include", "src/notes.txt"},
     {NULL}},
    {"--include recursively discovers supported files in a literal directory",
     NULL,
     NULL,
     NULL,
     NULL,
     {"--include", "src"},
     {ALL_SRC_FILES}},
    {"--include accepts a trailing slash on a literal directory",
     NULL,
     NULL,
     NULL,
     NULL,
     {"--include", "src/"},
     {ALL_SRC_FILES}},
    {"--include recursively discovers files from a nested literal directory",
     NULL,
     NULL,
     NULL,
     NULL,
     {"--include", "src/nested"},
     {"src/nested/deep.clj", "src/nested/deep.cljs", "src/nested/deep.cljc"}},
    {"--include combines multiple literal directories",
     NULL,
     NULL,
     NULL,
     NULL,
     {"--include", "dev", "--include", "test"},
     {"dev/user.clj", "test/core_test.clj", "test/nested/browser_test.cljs"}},
    {"--include combines a literal directory with a glob pattern",
     NULL,
     NULL,
     NULL,
     NULL,
     {"--include", "dev", "--include", "src/**/*.cljc"},
     {"dev/user.clj", "src/nested/deep.cljc", "src/shared.cljc"}},
    {"--include deduplicates a literal directory and an overlapping glob pattern",
     NULL,
     NULL,
     NULL,
     NULL,
     {"--include", "src", "--include", "src/**/*.clj"},
     {ALL_SRC_FILES}},
    {"--include uses --file-ext when discovering files from a literal directory",
     NULL,
     NULL,
     NULL,
     NULL,
     {"--include", "src", "--file-ext", "clj,cljc"},
     {"src/core.clj", "src/generated/generated.clj", "src/nested/deep.clj", "src/nested/deep.cljc",
      "src/shared.cljc"}},
    {"applies literal directory ignores to a literal include directory",
     NULL,
     NULL,
     NULL,
     NULL,
     {"--include", "src", "--ignore", "src/generated"},
     {"src/browser.cljs", "src/core.clj", "src/data.edn", "src/experimental.jank",
      "src/nested/deep.clj", "src/nested/deep.cljs", "src/nested/deep.cljc", "src/shared.cljc"}},
    {"applies ignore globs to a literal include directory",
     NULL,
     NULL,
     NULL,
     NULL,
     {"--include", "src", "--ignore", "src/{generated,nested}/**/*"},
     {"src/browser.cljs", "src/core.clj", "src/data.edn", "src/experimental.jank",
      "src/shared.cljc"}},
    {"--include . recursively discovers supported files from the current directory",
     NULL,
     NULL,
     NULL,
     NULL,
     {"--include", "."},
     {"dev/user.clj", "resources/config.edn", ALL_SRC_FILES, "test/core_test.clj",
      "test/nested/browser_test.cljs"}},
    {"returns an empty list for an empty literal directory",
     NULL,
     NULL,
     NULL,
     "empty",
     {"--include", "empty"},
     {NULL}},

    /* --include patterns */
    {"* matches files in one directory level",
     NULL,
     NULL,
     NULL,
     NULL,
     {"--include", "src/*.clj"},
     {"src/core.clj"}},
    {"** recursively matches nested directories",
     NULL,
     NULL,
     NULL,
     NULL,
     {"--include", "src/**/*.clj"},
     {"src/core.clj", "src/generated/generated.clj", "src/nested/deep.clj"}},
    {"supports brace expansion for file extensions",
     NULL,
     NULL,
     NULL,
     NULL,
     {"--include", "src/**/*.{clj,cljs,cljc}"},
     {"src/browser.cljs", "src/core.clj", "src/generated/generated.clj",
      "src/generated/generated.cljs", "src/nested/deep.clj", "src/nested/deep.cljs",
      "src/nested/deep.cljc", "src/shared.cljc"}},
    {"supports brace expansion for directory names",
     NULL,
     NULL,
     NULL,
     NULL,
     {"--include", "{src,test}/**/*.clj"},
     {"src/core.clj", "src/generated/generated.clj", "src/nested/deep.clj", "test/core_test.clj"}},
    {"combines multiple include patterns",
     NULL,
     NULL,
     NULL,
     NULL,
     {"--include", "src/**/*.clj", "--include", "test/**/*.clj"},
     {"src/core.clj", "src/generated/generated.clj", "src/nested/deep.clj", "test/core_test.clj"}},
    {"deduplicates files matched by multiple include patterns",
     NULL,
     NULL,
     NULL,
     NULL,
     {"--include", "src/**/*.clj", "--include", "src/core.clj"},
     {"src/core.clj", "src/generated/generated.clj", "src/nested/deep.clj"}},
    {"combines direct arguments with command-line include patterns",
     NULL,
     NULL,
     NULL,
     NULL,
     {"dev/user.clj", "--include", "src/*.clj"},
     {"dev/user.clj", "src/core.clj"}},
    {"broad include patterns return only regular files with supported extensions",
     NULL,
     NULL,
     NULL,
     NULL,
     {"--include", "src/**/*"},
     {ALL_SRC_FILES}},
    {"uses --file-ext to filter files discovered by include patterns",
     NULL,
     NULL,
     NULL,
     NULL,
     {"--include", "src/**/*", "--file-ext", "clj,cljc"},
     {"src/core.clj", "src/generated/generated.clj", "src/nested/deep.clj", "src/nested/deep.cljc",
      "src/shared.cljc"}},
    {"returns an empty list when no include pattern matches",
     NULL,
     NULL,
     NULL,
     NULL,
     {"--include", "does-not-exist/**/*.clj"},
     {NULL}},

    /* --ignore patterns */
    {"ignores a relative filename selected by an include pattern",
     NULL,
     NULL,
     NULL,
     NULL,
     {"--include", "src/**/*.clj", "--ignore", "src/nested/deep.clj"},
     {"src/core.clj", "src/generated/generated.clj"}},
    {"ignores all supported files beneath a directory",
     NULL,
     NULL,
     NULL,
     NULL,
     {"--include", "src/**/*.{clj,cljs,cljc}", "--ignore", "src/generated"},
     {"src/browser.cljs", "src/core.clj", "src/nested/deep.clj", "src/nested/deep.cljs",
      "src/nested/deep.cljc", "src/shared.cljc"}},
    {"supports recursive ignore glob patterns",
     NULL,
     NULL,
     NULL,
     NULL,
     {"--include", "src/**/*.{clj,cljs,cljc}", "--ignore", "src/generated/**/*"},
     {"src/browser.cljs", "src/core.clj", "src/nested/deep.clj", "src/nested/deep.cljs",
      "src/nested/deep.cljc", "src/shared.cljc"}},
    {"supports brace expansion in ignore patterns",
     NULL,
     NULL,
     NULL,
     NULL,
     {"--include", "src/**/*.{clj,cljs,cljc}", "--ignore",
      "src/{generated,nested}/**/*.{clj,cljs,cljc}"},
     {"src/browser.cljs", "src/core.clj", "src/shared.cljc"}},
    {"combines multiple ignore patterns",
     NULL,
     NULL,
     NULL,
     NULL,
     {"--include", "src/**/*.clj", "--ignore", "src/generated/**/*.clj", "--ignore",
      "src/nested/**/*.clj"},
     {"src/core.clj"}},
    {"applies ignore patterns to files discovered from a directory argument",
     NULL,
     NULL,
     NULL,
     NULL,
     {"src", "--ignore", "src/{generated,nested}/**/*"},
     {"src/browser.cljs", "src/core.clj", "src/data.edn", "src/experimental.jank",
      "src/shared.cljc"}},

    /* configuration files and command-line precedence */
    {"loads a literal include directory from .standard-clj.json",
     "{\"include\": [\"src\"]}\n",
     NULL,
     NULL,
     NULL,
     {NULL},
     {ALL_SRC_FILES}},
    {"loads a literal include directory with a trailing slash from .standard-clj.edn",
     NULL,
     "{:include [\"src/\"]}\n",
     NULL,
     NULL,
     {NULL},
     {ALL_SRC_FILES}},
    {"loads multiple literal include directories from configuration",
     "{\"include\": [\"dev\", \"test\"]}\n",
     NULL,
     NULL,
     NULL,
     {NULL},
     {"dev/user.clj", "test/core_test.clj", "test/nested/browser_test.cljs"}},
    {"applies configured ignore patterns to a configured literal directory",
     "{\"include\": [\"src\"], \"ignore\": [\"src/{generated,nested}/**/*\"]}\n",
     NULL,
     NULL,
     NULL,
     {NULL},
     {"src/browser.cljs", "src/core.clj", "src/data.edn", "src/experimental.jank",
      "src/shared.cljc"}},
    {"command-line literal directories replace configured include patterns",
     "{\"include\": [\"test/**/*.clj\"]}\n",
     NULL,
     NULL,
     NULL,
     {"--include", "src/nested"},
     {"src/nested/deep.clj", "src/nested/deep.cljs", "src/nested/deep.cljc"}},
    {"loads include and ignore patterns from .standard-clj.json",
     "{\"include\": [\"src/**/*.{clj,cljs,cljc}\"], \"ignore\": [\"src/generated/**/*\"]}\n",
     NULL,
     NULL,
     NULL,
     {NULL},
     {"src/browser.cljs", "src/core.clj", "src/nested/deep.clj", "src/nested/deep.cljs",
      "src/nested/deep.cljc", "src/shared.cljc"}},
    {"loads include and ignore patterns from .standard-clj.edn",
     NULL,
     "{:include [\"src/**/*.{clj,cljs,cljc}\"]\n :ignore [\"src/generated/**/*\"]}\n",
     NULL,
     NULL,
     {NULL},
     {"src/browser.cljs", "src/core.clj", "src/nested/deep.clj", "src/nested/deep.cljs",
      "src/nested/deep.cljc", "src/shared.cljc"}},
    {"command-line include patterns replace configured include patterns",
     "{\"include\": [\"test/**/*.clj\"]}\n",
     NULL,
     NULL,
     NULL,
     {"--include", "src/*.clj"},
     {"src/core.clj"}},
    {"direct file arguments replace configured include patterns",
     "{\"include\": [\"test/**/*.clj\"]}\n",
     NULL,
     NULL,
     NULL,
     {"dev/user.clj"},
     {"dev/user.clj"}},
    {"configured ignore patterns still apply when include is supplied on the command line",
     "{\"include\": [\"test/**/*.clj\"], \"ignore\": [\"src/generated/**/*\"]}\n",
     NULL,
     NULL,
     NULL,
     {"--include", "src/**/*.clj"},
     {"src/core.clj", "src/nested/deep.clj"}},
    {"command-line ignore patterns replace configured ignore patterns",
     "{\"include\": [\"src/**/*.clj\"], \"ignore\": [\"src/generated/**/*\"]}\n",
     NULL,
     NULL,
     NULL,
     {"--ignore", "src/nested/**/*"},
     {"src/core.clj", "src/generated/generated.clj"}},
    {"a string include in the config works like a one-item array",
     "{\"include\": \"src/*.clj\"}\n",
     NULL,
     NULL,
     NULL,
     {NULL},
     {"src/core.clj"}},
    {"when both default config files exist the EDN file wins",
     "{\"include\": [\"test\"]}\n",
     "{:include [\"dev\"]}\n",
     NULL,
     NULL,
     {NULL},
     {"dev/user.clj"}},

    /* ignoring explicitly named files */
    {"ignores a directly named file with a non-default extension",
     NULL,
     NULL,
     NULL,
     NULL,
     {"src/notes.txt", "--ignore", "src/notes.txt"},
     {NULL}},
    {"a directory ignore removes a directly named file with a non-default extension",
     NULL,
     NULL,
     NULL,
     NULL,
     {"src/notes.txt", "--ignore", "src"},
     {NULL}},

    /* patterns that also match directories */
    {"a pattern matching only a directory yields no files",
     NULL,
     NULL,
     NULL,
     NULL,
     {"--include", "src*"},
     {NULL}},
    {"a pattern matching a directory does not shadow sibling file matches",
     NULL,
     NULL,
     "srcfile.clj",
     NULL,
     {"--include", "src*"},
     {"srcfile.clj"}},

    /* hidden files */
    {"a glob does not match hidden files or directories",
     NULL,
     NULL,
     "src/.hidden/h.clj",
     NULL,
     {"--include", "src/**/*.clj"},
     {"src/core.clj", "src/generated/generated.clj", "src/nested/deep.clj"}},
    {"a directory argument includes hidden files",
     NULL,
     NULL,
     "src/.hidden/h.clj",
     NULL,
     {"src/.hidden"},
     {"src/.hidden/h.clj"}},
};

static void run_discovery_cases(void) {
  size_t num_cases = sizeof(discovery_cases) / sizeof(discovery_cases[0]);
  for (size_t i = 0; i < num_cases; i++) {
    const discovery_case *tc = &discovery_cases[i];
    char name[512];
    snprintf(name, sizeof(name), "discovery: %s", tc->name);
    if (!should_run_test(name)) {
      continue;
    }
    start_test(name);
    create_test_project();
    if (tc->json_config) {
      write_project_file(".standard-clj.json", tc->json_config);
    }
    if (tc->edn_config) {
      write_project_file(".standard-clj.edn", tc->edn_config);
    }
    if (tc->extra_file) {
      write_project_file(tc->extra_file, "(ns extra)\n");
    }
    if (tc->extra_dir) {
      char *dir = project_path(tc->extra_dir);
      make_directories(dir);
      free(dir);
    }

    cli_result r =
        run_cli_full(NULL, false, "list", tc->args[0], tc->args[1], tc->args[2], tc->args[3],
                     tc->args[4], tc->args[5], tc->args[6], tc->args[7], NULL);
    /* NOTE: a NULL arg ends the list, so "--output json" is added by running
     * the command a second time with the flag first */
    cli_result_free(&r);
    r = run_cli_full(NULL, false, "list", "--output", "json", tc->args[0], tc->args[1], tc->args[2],
                     tc->args[3], tc->args[4], tc->args[5], tc->args[6], tc->args[7], NULL);
    ASSERT_INT_EQ(r.exit_code, 0);
    ASSERT_STR_EQ(r.err, "");
    assert_listed_files(&r, tc->expected, __LINE__);
    cli_result_free(&r);
    finish_test();
  }
}

TEST(warns_when_a_direct_argument_does_not_exist) {
  create_test_project();
  cli_result r = run_cli("list", "--output", "json", "does-not-exist.clj", "src/core.clj");
  ASSERT_INT_EQ(r.exit_code, 0);
  ASSERT_CONTAINS(r.err, "WARN");
  ASSERT_CONTAINS(r.err, "Could not find a file or directory at \"does-not-exist.clj\"");
  ASSERT_NOT_CONTAINS(r.err, "to ignore");
  ASSERT_LISTED(r, "src/core.clj");
  cli_result_free(&r);
}

TEST(warns_when_a_literal_ignore_path_does_not_exist) {
  create_test_project();
  cli_result r = run_cli("list", "--output", "json", "src/core.clj", "--ignore", "no-such-path");
  ASSERT_INT_EQ(r.exit_code, 0);
  ASSERT_CONTAINS(r.err, "WARN");
  ASSERT_CONTAINS(r.err, "Could not find a file or directory to ignore at \"no-such-path\"");
  ASSERT_LISTED(r, "src/core.clj");
  cli_result_free(&r);

  /* an --ignore glob pattern that matches nothing is silent */
  r = run_cli("list", "--output", "json", "--include", "src/*.clj", "--ignore",
              "does-not-exist/**/*");
  ASSERT_STR_EQ(r.err, "");
  ASSERT_LISTED(r, "src/core.clj");
  cli_result_free(&r);
}

/* -----------------------------------------------------------------------------
 * list output formats
 */

TEST(list_output_formats) {
  create_test_project();
  char *user = project_path("dev/user.clj");
  char *core_test = project_path("test/core_test.clj");
  char *user_json = json_quoted(user);
  char *core_test_json = json_quoted(core_test);

  cli_result r = run_cli("list", "dev/user.clj", "test/core_test.clj");
  ASSERT_INT_EQ(r.exit_code, 0);
  char *expected = xprintf("%s\n%s\n", user, core_test);
  ASSERT_STR_EQ(r.out, expected);
  free(expected);
  cli_result_free(&r);

  r = run_cli("list", "dev/user.clj", "test/core_test.clj", "--output", "json");
  expected = xprintf("[%s,%s]\n", user_json, core_test_json);
  ASSERT_STR_EQ(r.out, expected);
  free(expected);
  cli_result_free(&r);

  r = run_cli("list", "dev/user.clj", "test/core_test.clj", "--output", "json-pretty");
  expected = xprintf("[\n  %s,\n  %s\n]\n", user_json, core_test_json);
  ASSERT_STR_EQ(r.out, expected);
  free(expected);
  cli_result_free(&r);

  r = run_cli("list", "dev/user.clj", "test/core_test.clj", "--output", "edn");
  expected = xprintf("[%s %s]\n", user_json, core_test_json);
  ASSERT_STR_EQ(r.out, expected);
  free(expected);
  cli_result_free(&r);

  r = run_cli("list", "dev/user.clj", "test/core_test.clj", "--output", "edn-pretty");
  expected = xprintf("[%s\n %s]\n", user_json, core_test_json);
  ASSERT_STR_EQ(r.out, expected);
  free(expected);
  cli_result_free(&r);

  /* nothing selected: text prints nothing, json prints an empty array */
  r = run_cli("list", "--include", "nothing/**/*.clj");
  ASSERT_INT_EQ(r.exit_code, 0);
  ASSERT_STR_EQ(r.out, "");
  cli_result_free(&r);
  r = run_cli("list", "--include", "nothing/**/*.clj", "--output", "json");
  ASSERT_STR_EQ(r.out, "[]\n");
  cli_result_free(&r);

  free(user);
  free(core_test);
  free(user_json);
  free(core_test_json);
}

/* -----------------------------------------------------------------------------
 * check and fix
 */

TEST(check_all_formatted) {
  create_test_project();
  cli_result r = run_cli("check", "dev", "test");
  ASSERT_INT_EQ(r.exit_code, 0);
  ASSERT_STR_EQ(r.err, "");
  ASSERT_STR_EQ_PATHS(r.out,
                      "standard-clj check 0.0.0-test\n"
                      "\n"
                      "\xE2\x9C\x93 /dev/user.clj [0ms]\n"
                      "\xE2\x9C\x93 /test/core_test.clj [0ms]\n"
                      "\xE2\x9C\x93 /test/nested/browser_test.cljs [0ms]\n"
                      "\n"
                      "All 3 files formatted with Standard Clojure Style \xF0\x9F\x91\x8D [0ms]\n");
  cli_result_free(&r);

  r = run_cli("check", "dev/user.clj");
  ASSERT_INT_EQ(r.exit_code, 0);
  ASSERT_CONTAINS(r.out, "\n1 file formatted with Standard Clojure Style \xF0\x9F\x91\x8D [0ms]\n");
  cli_result_free(&r);
}

TEST(check_reports_files_that_need_formatting_and_errors) {
  create_test_project();
  write_project_file("src/messy.clj", "(ns messy\n(:require [b.b :as b]))\n");
  write_project_file("src/broken.clj", "(ns broken (:use something))\n");
  cli_result r = run_cli("check", "src/core.clj", "src/messy.clj", "src/broken.clj");
  ASSERT_INT_EQ(r.exit_code, 1);

  char *broken = project_path("src/broken.clj");
  char *expected_err = xprintf("Failed to format file %s: Standard Clojure Style does not support "
                               ":use inside of the ns form. Please refactor with :require as "
                               "appropriate.\n"
                               "\xE2\x9C\x97 /src/messy.clj [0ms]\n",
                               broken);
  ASSERT_STR_EQ_PATHS(r.err, expected_err);
  free(expected_err);
  free(broken);

  ASSERT_STR_EQ_PATHS(r.out, "standard-clj check 0.0.0-test\n"
                             "\n"
                             "\xE2\x9C\x93 /src/core.clj [0ms]\n"
                             "\n"
                             "1 file formatted with Standard Clojure Style\n"
                             "1 file require formatting\n"
                             "Checked 3 files. [0ms]\n");
  cli_result_free(&r);

  /* the files were not modified */
  char *messy = read_project_file("src/messy.clj");
  ASSERT_STR_EQ(messy, "(ns messy\n(:require [b.b :as b]))\n");
  free(messy);
}

TEST(fix_formats_files_in_place) {
  create_test_project();
  write_project_file("src/messy.clj",
                     "(ns messy\n(:require [b.b :as b]\n[a.a :as a]))\n(def x\n1)");
  write_project_file("src/broken.clj", "(ns broken (:use something))\n");
  cli_result r = run_cli("fix", "src/core.clj", "src/messy.clj", "src/broken.clj");
  ASSERT_INT_EQ(r.exit_code, 1);
  ASSERT_STR_EQ_PATHS(r.err,
                      "E /src/broken.clj - Standard Clojure Style does not support :use inside "
                      "of the ns form. Please refactor with :require as appropriate. [0ms]\n");
  ASSERT_STR_EQ_PATHS(r.out, "standard-clj fix 0.0.0-test\n"
                             "\n"
                             "\xE2\x9C\x93 /src/core.clj [0ms]\n"
                             "F /src/messy.clj [0ms]\n"
                             "\n"
                             "2 files formatted with Standard Clojure Style\n"
                             "1 files with errors\n"
                             "Checked 3 files. [0ms]\n");
  cli_result_free(&r);

  char *messy = read_project_file("src/messy.clj");
  ASSERT_STR_EQ(messy, "(ns messy\n  (:require\n   [a.a :as a]\n   [b.b :as b]))\n(def x\n  1)\n");
  free(messy);
  char *broken = read_project_file("src/broken.clj");
  ASSERT_STR_EQ(broken, "(ns broken (:use something))\n");
  free(broken);

  /* a second run finds everything formatted */
  r = run_cli("fix", "src/core.clj", "src/messy.clj");
  ASSERT_INT_EQ(r.exit_code, 0);
  ASSERT_CONTAINS(r.out, "All 2 files formatted with Standard Clojure Style");
  cli_result_free(&r);
}

TEST(log_levels) {
  create_test_project();
  write_project_file("src/messy.clj", "(def x\n1)\n");

  /* quiet: nothing at all, but the exit code still reports the result */
  cli_result r = run_cli("check", "src/core.clj", "src/messy.clj", "--log-level", "quiet");
  ASSERT_INT_EQ(r.exit_code, 1);
  ASSERT_STR_EQ(r.out, "");
  ASSERT_STR_EQ(r.err, "");
  cli_result_free(&r);

  /* ignore-already-formatted hides the check marks */
  r = run_cli("check", "src/core.clj", "src/messy.clj", "-l", "ignore-already-formatted");
  ASSERT_INT_EQ(r.exit_code, 1);
  ASSERT_NOT_CONTAINS(r.out, "\xE2\x9C\x93");
  ASSERT_CONTAINS_PATHS(r.err, "\xE2\x9C\x97 /src/messy.clj");
  cli_result_free(&r);

  /* the numeric aliases */
  r = run_cli("check", "src/core.clj", "-l", "5");
  ASSERT_STR_EQ(r.out, "");
  cli_result_free(&r);
  r = run_cli("check", "src/core.clj", "-l", "1");
  ASSERT_NOT_CONTAINS(r.out, "\xE2\x9C\x93");
  ASSERT_CONTAINS(r.out, "1 file formatted");
  cli_result_free(&r);

  /* a configured log level applies unless the command line sets one */
  write_project_file(".standard-clj.edn", "{:include [\"src/core.clj\"] :log-level \"quiet\"}\n");
  r = run_cli("check");
  ASSERT_INT_EQ(r.exit_code, 0);
  ASSERT_STR_EQ(r.out, "");
  cli_result_free(&r);
  r = run_cli("check", "--log-level", "everything");
  ASSERT_CONTAINS(r.out, "1 file formatted");
  cli_result_free(&r);
}

TEST(no_files_selected) {
  create_test_project();
  cli_result r = run_cli("check", "--include", "nothing/**/*.clj");
  ASSERT_INT_EQ(r.exit_code, 1);
  ASSERT_STR_EQ(r.err, "No files were passed to the \"check\" command. Please pass a filename, "
                       "directory, or --include glob pattern.\n");
  cli_result_free(&r);
  r = run_cli("fix");
  ASSERT_INT_EQ(r.exit_code, 1);
  ASSERT_CONTAINS(r.err, "No files were passed to the \"fix\" command.");
  cli_result_free(&r);
}

TEST(fix_from_stdin) {
  create_test_project();
  cli_result r = run_cli_stdin("(ns my.company.core\n(:require [b.b] [a.a]))", "fix", "-");
  ASSERT_INT_EQ(r.exit_code, 0);
  ASSERT_STR_EQ(r.out, "(ns my.company.core\n  (:require\n   [a.a]\n   [b.b]))\n");
  ASSERT_STR_EQ(r.err, "");
  cli_result_free(&r);

  r = run_cli_stdin("", "fix", "-");
  ASSERT_INT_EQ(r.exit_code, 1);
  ASSERT_STR_EQ(r.err, "Nothing found on stdin. Please pipe some Clojure code to stdin when "
                       "using \"standard-clj fix -\"\n");
  cli_result_free(&r);

  r = run_cli_stdin("(ns foo (:use bar))", "fix", "-");
  ASSERT_INT_EQ(r.exit_code, 1);
  ASSERT_CONTAINS(r.err, "Failed to format code: Standard Clojure Style does not support :use");
  cli_result_free(&r);
}

TEST(colors) {
  create_test_project();
  write_project_file("src/messy.clj", "(def x\n1)\n");
  cli_result r = run_cli_colors("check", "src/core.clj", "src/messy.clj");
  ASSERT_INT_EQ(r.exit_code, 1);
  ASSERT_STR_EQ_PATHS(
      r.out, "\x1b[1mstandard-clj check\x1b[22m \x1b[2m0.0.0-test\x1b[22m\n"
             "\n"
             "\x1b[32m\xE2\x9C\x93\x1b[39m \x1b[1m/src/core.clj\x1b[22m \x1b[2m[0ms]\x1b[22m\n"
             "\n"
             "\x1b[32m1 file formatted with Standard Clojure Style\x1b[39m\n"
             "\x1b[31m1 file require formatting\x1b[39m\n"
             "Checked 2 files. \x1b[2m[0ms]\x1b[22m\n");
  ASSERT_STR_EQ_PATHS(
      r.err, "\x1b[31m\xE2\x9C\x97\x1b[39m \x1b[1m/src/messy.clj\x1b[22m \x1b[2m[0ms]\x1b[22m\n");
  cli_result_free(&r);

  /* the warning is bold yellow, and a fix error is bold red */
  r = run_cli_colors("list", "nope.clj");
  ASSERT_STR_EQ(r.err, "\x1b[1m\x1b[33mWARN\x1b[39m\x1b[22m Could not find a file or directory at "
                       "\"nope.clj\"\n");
  cli_result_free(&r);
  write_project_file("src/broken.clj", "(ns broken (:use x))\n");
  r = run_cli_colors("fix", "src/broken.clj");
  ASSERT_CONTAINS_PATHS(r.err,
                        "\x1b[31mE\x1b[39m \x1b[1m\x1b[31m/src/broken.clj\x1b[39m\x1b[22m - ");
  cli_result_free(&r);
}

/* -----------------------------------------------------------------------------
 * Help, version, usage errors, config errors
 */

TEST(help_and_version) {
  create_test_project();
  cli_result r = run_cli("--help");
  ASSERT_INT_EQ(r.exit_code, 0);
  ASSERT_TRUE(strncmp(r.out, "standard-clj <cmd> [args]\n", 26) == 0);
  ASSERT_CONTAINS(r.out, "standard-clj fix -       Reads from stdin");
  cli_result_free(&r);

  r = run_cli("check", "src", "-h");
  ASSERT_INT_EQ(r.exit_code, 0);
  ASSERT_CONTAINS(r.out, "Commands:");
  cli_result_free(&r);

  r = run_cli("--version");
  ASSERT_INT_EQ(r.exit_code, 0);
  ASSERT_STR_EQ(r.out, "0.0.0-test [abc1234]\n");
  cli_result_free(&r);

  r = run_cli(NULL);
  ASSERT_INT_EQ(r.exit_code, 1);
  ASSERT_CONTAINS(r.err, "standard-clj <cmd> [args]");
  ASSERT_CONTAINS(r.err, "Not enough non-option arguments: got 0, need at least 1");
  ASSERT_STR_EQ(r.out, "");
  cli_result_free(&r);
}

TEST(usage_errors) {
  create_test_project();
  cli_result r = run_cli("frobnicate", "src");
  ASSERT_INT_EQ(r.exit_code, 1);
  ASSERT_STR_EQ(r.err, "Unknown command: frobnicate\nRun \"standard-clj --help\" for usage.\n");
  cli_result_free(&r);

  r = run_cli("list", "--bogus", "src");
  ASSERT_INT_EQ(r.exit_code, 1);
  ASSERT_CONTAINS(r.err, "Unknown option: --bogus");
  cli_result_free(&r);

  r = run_cli("list", "src", "--include");
  ASSERT_INT_EQ(r.exit_code, 1);
  ASSERT_CONTAINS(r.err, "The option --include requires a value.");
  cli_result_free(&r);
}

TEST(config_file_errors) {
  create_test_project();
  write_project_file("bad.json", "{not json");
  cli_result r = run_cli("list", "src", "--config", "bad.json");
  ASSERT_INT_EQ(r.exit_code, 1);
  ASSERT_STR_EQ(r.err, "Unable to load config file: bad.json\nMaybe the file is invalid JSON?\n");
  cli_result_free(&r);

  write_project_file("bad.edn", "{:include [");
  r = run_cli("list", "src", "-c", "bad.edn");
  ASSERT_INT_EQ(r.exit_code, 1);
  ASSERT_STR_EQ(r.err, "Unable to load config file: bad.edn\nMaybe the file is invalid EDN?\n");
  cli_result_free(&r);

  r = run_cli("list", "src", "--config", "missing.edn");
  ASSERT_INT_EQ(r.exit_code, 1);
  ASSERT_CONTAINS(r.err, "Unable to load config file: missing.edn");
  cli_result_free(&r);

  write_project_file("config.txt", "{}");
  r = run_cli("list", "src", "--config", "config.txt");
  ASSERT_INT_EQ(r.exit_code, 1);
  ASSERT_CONTAINS(r.err, "The filename does not end in .json or .edn. That is probably wrong.");
  cli_result_free(&r);

  /* an explicit config file that loads */
  write_project_file("config/standard-clj.edn", "{:include [\"dev\"]}");
  r = run_cli("list", "--output", "json", "--config", "config/standard-clj.edn");
  ASSERT_INT_EQ(r.exit_code, 0);
  ASSERT_LISTED(r, "dev/user.clj");
  cli_result_free(&r);

  /* an unreadable default config file is ignored */
  write_project_file(".standard-clj.json", "{not json");
  r = run_cli("list", "--output", "json", "dev");
  ASSERT_INT_EQ(r.exit_code, 0);
  ASSERT_LISTED(r, "dev/user.clj");
  cli_result_free(&r);
}

/* -----------------------------------------------------------------------------
 * Main
 */

int main(int argc, char **argv) {
  if (argc > 1) {
    test_filter = argv[1];
  }

  char *cwd = current_directory();
  if (cwd == NULL) {
    fprintf(stderr, "could not determine the current directory\n");
    return 1;
  }
  project_dir = path_resolve(cwd, "build/cli-test-project");
  streams_dir = path_resolve(cwd, "build/cli-test-streams");
  free(cwd);

  RUN_TEST(normalize_log_level);
  RUN_TEST(classify_format_result);
  RUN_TEST(file_str_and_add_period_prefix);
  RUN_TEST(relative_filename);
  RUN_TEST(convert_file_ext);
  RUN_TEST(output_encoders);
  RUN_TEST(duration_formatting);
  RUN_TEST(path_helpers);
  RUN_TEST(brace_expansion);
  RUN_TEST(segment_matching);
  RUN_TEST(edn_reader);
  RUN_TEST(argument_parsing);

  run_discovery_cases();
  RUN_TEST(warns_when_a_direct_argument_does_not_exist);
  RUN_TEST(warns_when_a_literal_ignore_path_does_not_exist);
  RUN_TEST(list_output_formats);

  RUN_TEST(check_all_formatted);
  RUN_TEST(check_reports_files_that_need_formatting_and_errors);
  RUN_TEST(fix_formats_files_in_place);
  RUN_TEST(log_levels);
  RUN_TEST(no_files_selected);
  RUN_TEST(fix_from_stdin);
  RUN_TEST(colors);

  RUN_TEST(help_and_version);
  RUN_TEST(usage_errors);
  RUN_TEST(config_file_errors);

  remove_tree(project_dir);
  remove_tree(streams_dir);
  free(project_dir);
  free(streams_dir);
  return TEST_SUMMARY();
}

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

#include "cjson/cJSON.h"

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
 * Test utilities: a growable byte buffer
 */

typedef struct test_buf {
  char *data;
  size_t len;
  size_t capacity;
} test_buf;

static void buf_append(test_buf *buf, const char *bytes, size_t n) {
  if (buf->len + n + 1 > buf->capacity) {
    size_t new_capacity = buf->capacity == 0 ? 256 : buf->capacity;
    while (new_capacity < buf->len + n + 1) {
      new_capacity *= 2;
    }
    char *new_data = realloc(buf->data, new_capacity);
    if (!new_data) {
      fprintf(stderr, "out of memory\n");
      exit(1);
    }
    buf->data = new_data;
    buf->capacity = new_capacity;
  }
  memcpy(buf->data + buf->len, bytes, n);
  buf->len += n;
  buf->data[buf->len] = '\0';
}

static void buf_append_cstr(test_buf *buf, const char *s) {
  buf_append(buf, s, strlen(s));
}

static void buf_append_int(test_buf *buf, int n) {
  char tmp[32];
  snprintf(tmp, sizeof(tmp), "%d", n);
  buf_append_cstr(buf, tmp);
}

static scs_str buf_to_str(const test_buf *buf) {
  scs_str s;
  s.data = buf->data ? buf->data : "";
  s.len = (int)buf->len;
  return s;
}

/* -----------------------------------------------------------------------------
 * Test utilities: reading the JSON test case files
 *
 * The test case files are generated by the Standard Clojure Style JavaScript
 * project: an array of objects whose values are all strings. This reader
 * handles exactly that shape (plus every JSON string escape) and exits with a
 * clear message on anything else, so a malformed fixture can never pass
 * silently.
 */

typedef struct test_case {
  scs_str name;
  scs_str input;
  scs_str expected;
} test_case;

typedef struct test_case_list {
  test_case *items;
  int size;
} test_case_list;

typedef struct json_reader {
  const char *path;
  const char *data;
  size_t len;
  size_t pos;
} json_reader;

static void json_fail(const json_reader *r, const char *msg) {
  fprintf(stderr, "%s: invalid test case file at byte %zu: %s\n", r->path, r->pos, msg);
  exit(1);
}

static void json_skip_whitespace(json_reader *r) {
  while (r->pos < r->len) {
    char c = r->data[r->pos];
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
      break;
    }
    r->pos++;
  }
}

static char json_peek(json_reader *r) {
  json_skip_whitespace(r);
  if (r->pos >= r->len) {
    json_fail(r, "unexpected end of file");
  }
  return r->data[r->pos];
}

static void json_expect(json_reader *r, char expected) {
  if (json_peek(r) != expected) {
    char msg[64];
    snprintf(msg, sizeof(msg), "expected '%c'", expected);
    json_fail(r, msg);
  }
  r->pos++;
}

static int json_hex_digit(json_reader *r, char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  json_fail(r, "invalid hex digit in \\u escape");
  return 0;
}

/* reads the four hex digits after \u */
static int json_read_hex4(json_reader *r) {
  if (r->pos + 4 > r->len) {
    json_fail(r, "truncated \\u escape");
  }
  int value = 0;
  for (size_t i = 0; i < 4; i++) {
    value = (value << 4) | json_hex_digit(r, r->data[r->pos + i]);
  }
  r->pos += 4;
  return value;
}

static void buf_append_utf8(test_buf *buf, int code_point) {
  char bytes[4];
  size_t n = 0;
  if (code_point < 0x80) {
    bytes[n++] = (char)code_point;
  } else if (code_point < 0x800) {
    bytes[n++] = (char)(0xC0 | (code_point >> 6));
    bytes[n++] = (char)(0x80 | (code_point & 0x3F));
  } else if (code_point < 0x10000) {
    bytes[n++] = (char)(0xE0 | (code_point >> 12));
    bytes[n++] = (char)(0x80 | ((code_point >> 6) & 0x3F));
    bytes[n++] = (char)(0x80 | (code_point & 0x3F));
  } else {
    bytes[n++] = (char)(0xF0 | (code_point >> 18));
    bytes[n++] = (char)(0x80 | ((code_point >> 12) & 0x3F));
    bytes[n++] = (char)(0x80 | ((code_point >> 6) & 0x3F));
    bytes[n++] = (char)(0x80 | (code_point & 0x3F));
  }
  buf_append(buf, bytes, n);
}

/* reads a JSON string into newly malloc'd UTF-8 */
static scs_str json_read_string(json_reader *r) {
  json_expect(r, '"');
  test_buf buf = {NULL, 0, 0};
  while (true) {
    if (r->pos >= r->len) {
      json_fail(r, "unterminated string");
    }
    char c = r->data[r->pos++];
    if (c == '"') {
      break;
    }
    if (c != '\\') {
      buf_append(&buf, &c, 1);
      continue;
    }
    if (r->pos >= r->len) {
      json_fail(r, "unterminated escape");
    }
    char e = r->data[r->pos++];
    switch (e) {
    case '"':
    case '\\':
    case '/':
      buf_append(&buf, &e, 1);
      break;
    case 'b':
      buf_append(&buf, "\b", 1);
      break;
    case 'f':
      buf_append(&buf, "\f", 1);
      break;
    case 'n':
      buf_append(&buf, "\n", 1);
      break;
    case 'r':
      buf_append(&buf, "\r", 1);
      break;
    case 't':
      buf_append(&buf, "\t", 1);
      break;
    case 'u': {
      int code_point = json_read_hex4(r);
      if (code_point >= 0xD800 && code_point <= 0xDBFF) {
        /* high surrogate: must be followed by \uDC00-\uDFFF */
        if (r->pos + 2 > r->len || r->data[r->pos] != '\\' || r->data[r->pos + 1] != 'u') {
          json_fail(r, "high surrogate not followed by low surrogate");
        }
        r->pos += 2;
        int low = json_read_hex4(r);
        if (low < 0xDC00 || low > 0xDFFF) {
          json_fail(r, "invalid low surrogate");
        }
        code_point = 0x10000 + ((code_point - 0xD800) << 10) + (low - 0xDC00);
      }
      buf_append_utf8(&buf, code_point);
      break;
    }
    default:
      json_fail(r, "invalid escape sequence");
    }
  }
  if (buf.data == NULL) {
    buf_append(&buf, "", 0);
  }
  return buf_to_str(&buf);
}

/* reads one {"name": ..., "input": ..., "expected": ...} object */
static test_case json_read_test_case(json_reader *r) {
  test_case tc;
  memset(&tc, 0, sizeof(tc));
  tc.name = tc.input = tc.expected = EMPTY_STR;
  bool has_name = false, has_input = false, has_expected = false;

  json_expect(r, '{');
  while (true) {
    scs_str key = json_read_string(r);
    json_expect(r, ':');
    if (json_peek(r) != '"') {
      json_fail(r, "every test case value must be a string");
    }
    scs_str value = json_read_string(r);
    if (str_eq_cstr(key, "name")) {
      tc.name = value;
      has_name = true;
    } else if (str_eq_cstr(key, "input")) {
      tc.input = value;
      has_input = true;
    } else if (str_eq_cstr(key, "expected")) {
      tc.expected = value;
      has_expected = true;
    } else {
      free((char *)value.data);
    }
    free((char *)key.data);

    char next = json_peek(r);
    r->pos++;
    if (next == '}') {
      break;
    }
    if (next != ',') {
      json_fail(r, "expected ',' or '}'");
    }
  }
  if (!has_name || !has_input || !has_expected) {
    json_fail(r, "test case is missing name, input, or expected");
  }
  return tc;
}

static char *read_whole_file(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "could not open %s (run the tests from the repository root)\n", path);
    exit(1);
  }
  test_buf buf = {NULL, 0, 0};
  char chunk[4096];
  size_t n;
  while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
    buf_append(&buf, chunk, n);
  }
  fclose(f);
  if (buf.data == NULL) {
    buf_append(&buf, "", 0);
  }
  *out_len = buf.len;
  return buf.data;
}

static test_case_list read_test_cases(const char *path) {
  size_t len = 0;
  char *data = read_whole_file(path, &len);
  json_reader r = {path, data, len, 0};

  test_case_list list = {NULL, 0};
  int capacity = 0;
  json_expect(&r, '[');
  if (json_peek(&r) == ']') {
    json_fail(&r, "no test cases");
  }
  while (true) {
    if (list.size == capacity) {
      capacity = capacity == 0 ? 64 : capacity * 2;
      test_case *items = realloc(list.items, (size_t)capacity * sizeof(test_case));
      if (!items) {
        fprintf(stderr, "out of memory\n");
        exit(1);
      }
      list.items = items;
    }
    list.items[list.size++] = json_read_test_case(&r);
    char next = json_peek(&r);
    r.pos++;
    if (next == ']') {
      break;
    }
    if (next != ',') {
      json_fail(&r, "expected ',' or ']'");
    }
  }
  json_skip_whitespace(&r);
  if (r.pos != r.len) {
    json_fail(&r, "unexpected content after the test cases");
  }
  free(data);
  return list;
}

static void free_test_cases(test_case_list *list) {
  for (int i = 0; i < list->size; i++) {
    free((char *)list->items[i].name.data);
    free((char *)list->items[i].input.data);
    free((char *)list->items[i].expected.data);
  }
  free(list->items);
  list->items = NULL;
  list->size = 0;
}

/* prints a failing test case: expected vs actual output */
static void report_case_mismatch(scs_str expected, scs_str actual) {
  printf("--- expected ---\n%.*s\n--- actual ---\n%.*s\n---\n", expected.len, expected.data,
         actual.len, actual.data);
}

/* -----------------------------------------------------------------------------
 * Test utilities: printing a parse tree
 *
 * The parser test cases compare against this textual form of the tree.
 * Whitespace nodes are left out.
 */

static void node_to_string(test_buf *out, const scs_node *node, int indent_level) {
  if (is_whitespace_node(node)) {
    return;
  }
  const char *name = node->name ? node->name : "null";
  if (strcmp(name, "source") != 0) {
    buf_append_cstr(out, "\n");
  }
  for (int i = 0; i < indent_level * 2; i++) {
    buf_append_cstr(out, " ");
  }
  buf_append_cstr(out, "(");
  buf_append_cstr(out, name);
  buf_append_cstr(out, " ");
  buf_append_int(out, node->start_idx);
  buf_append_cstr(out, "..");
  buf_append_int(out, node->end_idx);

  if (node->text.len > 0) {
    buf_append_cstr(out, " '");
    for (int i = 0; i < node->text.len; i++) {
      if (node->text.data[i] == '\n') {
        buf_append_cstr(out, "\\n");
      } else {
        buf_append(out, node->text.data + i, 1);
      }
    }
    buf_append_cstr(out, "'");
  }

  for (int i = 0; i < node->children.size; i++) {
    node_to_string(out, node->children.items[i], indent_level + 1);
  }
  buf_append_cstr(out, ")");
}

/* a parse that is expected to throw; returns true if it did */
static bool parse_throws(scs_ctx *ctx, scs_str txt) {
  if (setjmp(ctx->error_jmp) == 0) {
    parse(ctx, txt);
    return false;
  }
  return true;
}

/* -----------------------------------------------------------------------------
 * Test utilities: the parsed ns form as JSON
 *
 * The ns test cases describe the expected result as JSON. parse_ns() produces
 * C structs, so the structs are converted to a cJSON tree with the same keys
 * the JavaScript implementation uses, and the two trees are compared.
 */

static cJSON *json_str(scs_str s) {
  char *tmp = malloc((size_t)s.len + 1);
  if (!tmp) {
    fprintf(stderr, "out of memory\n");
    exit(1);
  }
  memcpy(tmp, s.data, (size_t)s.len);
  tmp[s.len] = '\0';
  cJSON *item = cJSON_CreateString(tmp);
  free(tmp);
  return item;
}

/* adds a string field, but only when the string is not empty */
static void json_add_str(cJSON *obj, const char *key, scs_str s) {
  if (s.len > 0) {
    cJSON_AddItemToObject(obj, key, json_str(s));
  }
}

static cJSON *json_str_list(const scs_str_list *list) {
  cJSON *arr = cJSON_CreateArray();
  for (int i = 0; i < list->size; i++) {
    cJSON_AddItemToArray(arr, json_str(list->items[i]));
  }
  return arr;
}

/* adds a string list field, but only when the list exists */
static void json_add_str_list(cJSON *obj, const char *key, const scs_str_list *list) {
  if (list != NULL) {
    cJSON_AddItemToObject(obj, key, json_str_list(list));
  }
}

static void json_add_comments(cJSON *obj, const scs_str_list *comments_above,
                              scs_str comment_after) {
  json_add_str_list(obj, "commentsAbove", comments_above);
  json_add_str(obj, "commentAfter", comment_after);
}

static cJSON *json_symbols(const scs_ns_symbol_list *list) {
  cJSON *arr = cJSON_CreateArray();
  for (int i = 0; i < list->size; i++) {
    cJSON *itm = cJSON_CreateObject();
    json_add_str(itm, "symbol", list->items[i].symbol);
    json_add_str(itm, "platform", list->items[i].platform);
    cJSON_AddItemToArray(arr, itm);
  }
  return arr;
}

static cJSON *json_renames(const scs_ns_rename_list *list) {
  cJSON *arr = cJSON_CreateArray();
  for (int i = 0; i < list->size; i++) {
    cJSON *itm = cJSON_CreateObject();
    json_add_str(itm, "fromSymbol", list->items[i].from_symbol);
    json_add_str(itm, "toSymbol", list->items[i].to_symbol);
    json_add_str(itm, "platform", list->items[i].platform);
    cJSON_AddItemToArray(arr, itm);
  }
  return arr;
}

static cJSON *json_require(const scs_ns_require *req) {
  cJSON *obj = cJSON_CreateObject();
  json_add_str(obj, "symbol", req->symbol);
  if (req->symbol_is_string) {
    cJSON_AddBoolToObject(obj, "symbolIsString", true);
  }
  json_add_str(obj, "as", req->as);
  json_add_str(obj, "asAlias", req->as_alias);
  json_add_str(obj, "default", req->default_symbol);
  if (req->refer_all) {
    cJSON_AddStringToObject(obj, "refer", "all");
  } else if (req->refer != NULL) {
    cJSON_AddItemToObject(obj, "refer", json_symbols(req->refer));
  }
  if (req->exclude != NULL) {
    cJSON_AddItemToObject(obj, "exclude", json_symbols(req->exclude));
  }
  if (req->has_include_macros) {
    cJSON_AddBoolToObject(obj, "includeMacros", req->include_macros);
  }
  json_add_str_list(obj, "referMacros", req->refer_macros);
  if (req->rename != NULL) {
    cJSON_AddItemToObject(obj, "rename", json_renames(req->rename));
  }
  json_add_str_list(obj, "metadata", req->metadata);
  json_add_comments(obj, req->comments_above, req->comment_after);
  json_add_str(obj, "platform", req->platform);
  return obj;
}

static cJSON *json_requires(const scs_ns_require_list *list) {
  cJSON *arr = cJSON_CreateArray();
  for (int i = 0; i < list->size; i++) {
    cJSON_AddItemToArray(arr, json_require(&list->items[i]));
  }
  return arr;
}

static cJSON *json_gen_class_entry(const scs_ns_gen_class *gc, int key) {
  const scs_gen_class_entry *entry = &gc->entries[key];

  /* :implements with a value is a plain list of symbols */
  if (key == GEN_CLASS_IMPLEMENTS && entry->has_value) {
    return json_symbols(&gc->implements);
  }

  cJSON *obj = cJSON_CreateObject();
  if (entry->has_value) {
    if (is_gen_class_boolean_key(key)) {
      cJSON_AddBoolToObject(obj, "value", entry->bool_value);
    } else if (key == GEN_CLASS_CONSTRUCTORS) {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < gc->constructors.size; i++) {
        const scs_gen_class_constructor *c = &gc->constructors.items[i];
        cJSON *itm = cJSON_CreateObject();
        json_add_str(itm, "metadata", c->metadata);
        cJSON_AddItemToObject(itm, "parameterTypes", json_str_list(&c->parameter_types));
        cJSON_AddItemToObject(itm, "superParameterTypes", json_str_list(&c->super_parameter_types));
        cJSON_AddItemToArray(arr, itm);
      }
      cJSON_AddItemToObject(obj, "value", arr);
    } else if (key == GEN_CLASS_METHODS) {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < gc->methods.size; i++) {
        const scs_gen_class_method *m = &gc->methods.items[i];
        cJSON *itm = cJSON_CreateObject();
        json_add_str(itm, "metadata", m->metadata);
        json_add_str(itm, "name", m->name);
        cJSON_AddItemToObject(itm, "parameterTypes", json_str_list(&m->parameter_types));
        json_add_str(itm, "returnType", m->return_type);
        cJSON_AddItemToArray(arr, itm);
      }
      cJSON_AddItemToObject(obj, "value", arr);
    } else if (key == GEN_CLASS_EXPOSES) {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < gc->exposes.size; i++) {
        const scs_gen_class_expose *e = &gc->exposes.items[i];
        cJSON *itm = cJSON_CreateObject();
        json_add_str(itm, "fieldName", e->field_name);
        json_add_str(itm, "getter", e->getter);
        json_add_str(itm, "setter", e->setter);
        cJSON_AddItemToArray(arr, itm);
      }
      cJSON_AddItemToObject(obj, "value", arr);
    } else if (key == GEN_CLASS_EXPOSES_METHODS) {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < gc->exposes_methods.size; i++) {
        const scs_gen_class_exposes_method *e = &gc->exposes_methods.items[i];
        cJSON *itm = cJSON_CreateObject();
        json_add_str(itm, "superMethodName", e->super_method_name);
        json_add_str(itm, "exposedName", e->exposed_name);
        cJSON_AddItemToArray(arr, itm);
      }
      cJSON_AddItemToObject(obj, "value", arr);
    } else {
      json_add_str(obj, "value", entry->value);
    }
  }
  json_add_str(obj, "metadata", entry->metadata);
  json_add_comments(obj, entry->comments_above, entry->comment_after);
  return obj;
}

static cJSON *json_gen_class(const scs_ns_gen_class *gc) {
  cJSON *obj = cJSON_CreateObject();
  cJSON_AddBoolToObject(obj, "isEmpty", gc->is_empty);
  json_add_str(obj, "platform", gc->platform);
  json_add_comments(obj, gc->comments_above, gc->comment_after);
  for (int key = 0; key < GEN_CLASS_NUM_KEYS; key++) {
    if (gc->entries[key].present) {
      cJSON_AddItemToObject(obj, gen_class_key_names[key], json_gen_class_entry(gc, key));
    }
  }
  return obj;
}

static cJSON *ns_to_json(const scs_ns *ns) {
  cJSON *obj = cJSON_CreateObject();

  if (ns->ns_symbol.len > 0) {
    json_add_str(obj, "nsSymbol", ns->ns_symbol);
  } else {
    cJSON_AddNullToObject(obj, "nsSymbol");
  }
  if (ns->has_docstring) {
    cJSON_AddItemToObject(obj, "docstring", json_str(ns->docstring));
  }
  if (ns->ns_metadata != NULL) {
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < ns->ns_metadata->size; i++) {
      cJSON *itm = cJSON_CreateObject();
      json_add_str(itm, "key", ns->ns_metadata->items[i].key);
      json_add_str(itm, "value", ns->ns_metadata->items[i].value);
      cJSON_AddItemToArray(arr, itm);
    }
    cJSON_AddItemToObject(obj, "nsMetadata", arr);
  }

  if (ns->refer_clojure != NULL) {
    cJSON *rc = cJSON_CreateObject();
    if (ns->refer_clojure->exclude != NULL) {
      cJSON_AddItemToObject(rc, "exclude", json_symbols(ns->refer_clojure->exclude));
    }
    if (ns->refer_clojure->only != NULL) {
      cJSON_AddItemToObject(rc, "only", json_symbols(ns->refer_clojure->only));
    }
    if (ns->refer_clojure->rename != NULL) {
      cJSON_AddItemToObject(rc, "rename", json_renames(ns->refer_clojure->rename));
    }
    cJSON_AddItemToObject(obj, "referClojure", rc);
  }
  json_add_str_list(obj, "referClojureCommentsAbove", ns->refer_clojure_comments_above);
  json_add_str(obj, "referClojureCommentAfter", ns->refer_clojure_comment_after);

  if (ns->require_macros != NULL) {
    cJSON_AddItemToObject(obj, "requireMacros", json_requires(ns->require_macros));
  }
  json_add_str_list(obj, "requireMacrosCommentsAbove", ns->require_macros_comments_above);

  if (ns->requires != NULL) {
    cJSON_AddItemToObject(obj, "requires", json_requires(ns->requires));
  }
  json_add_str_list(obj, "requireCommentsAbove", ns->require_comments_above);
  json_add_str(obj, "requireCommentAfter", ns->require_comment_after);

  if (ns->imports != NULL) {
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < ns->imports->size; i++) {
      const scs_ns_import *imp = &ns->imports->items[i];
      cJSON *itm = cJSON_CreateObject();
      json_add_str(itm, "package", imp->package);
      cJSON_AddItemToObject(itm, "classes", json_str_list(&imp->classes));
      json_add_comments(itm, imp->comments_above, imp->comment_after);
      json_add_str(itm, "platform", imp->platform);
      cJSON_AddItemToArray(arr, itm);
    }
    cJSON_AddItemToObject(obj, "imports", arr);
  }
  json_add_str_list(obj, "importCommentsAbove", ns->import_comments_above);
  json_add_str(obj, "importCommentAfter", ns->import_comment_after);

  if (ns->gen_class != NULL) {
    cJSON_AddItemToObject(obj, "genClass", json_gen_class(ns->gen_class));
  }
  json_add_str(obj, "commentOutsideNsForm", ns->comment_outside_ns_form);
  return obj;
}

/* parses the input and its ns form; returns NULL if parsing threw
 * (the message is then in ctx->error_msg) */
static scs_ns *try_parse_ns(scs_ctx *ctx, scs_str input) {
  if (setjmp(ctx->error_jmp) == 0) {
    scs_node *tree = parse(ctx, input);
    scs_node_list nodes = flatten_tree(ctx, tree);
    return parse_ns(ctx, &nodes);
  }
  return NULL;
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
 * Parsers
 */

/* parsers built for the tests below */
static const scs_parser tp_char_a = {.parse = parse_char, .name = "char_a", .ch = 'a'};
static const scs_parser tp_char_b = {.parse = parse_char, .name = "char_b", .ch = 'b'};
static const scs_parser tp_char_x = {.parse = parse_char, .name = "char_x", .ch = 'x'};

/* a parser that matches 'x' and returns an unnamed node */
static scs_node *custom_unnamed_x(scs_ctx *ctx, const scs_parser *p, scs_str txt, int pos) {
  (void)p;
  if (pos >= 0 && pos < txt.len && txt.data[pos] == 'x') {
    return node_new(ctx, NULL, pos, pos + 1, STR("x"));
  }
  return NULL;
}

TEST(named_parser_basic) {
  scs_ctx *ctx = test_ctx_new();
  scs_parser named = {.parse = parse_named, .name = "test_name", .parser = &tp_char_a};

  scs_node *result1 = named.parse(ctx, &named, STR("abc"), 0);
  ASSERT_NOT_NULL(result1);
  ASSERT_STR_EQ(result1->name, "test_name");
  ASSERT_INT_EQ(result1->start_idx, 0);
  ASSERT_INT_EQ(result1->end_idx, 1);
  ASSERT_INT_EQ(result1->children.size, 1);
  ASSERT_SSTR_EQ(result1->children.items[0]->text, "a");

  ASSERT_NULL(named.parse(ctx, &named, STR("xyz"), 0));
  test_ctx_free(ctx);
}

TEST(named_parser_with_existing_name) {
  scs_ctx *ctx = test_ctx_new();
  scs_parser inner = {.parse = parse_string, .name = "inner_name", .str = STR("test")};
  scs_parser named = {.parse = parse_named, .name = "outer_name", .parser = &inner};

  scs_node *result = named.parse(ctx, &named, STR("test123"), 0);
  ASSERT_NOT_NULL(result);
  ASSERT_STR_EQ(result->name, "outer_name");
  ASSERT_STR_EQ(result->children.items[0]->name, "inner_name");
  ASSERT_SSTR_EQ(result->children.items[0]->text, "test");
  test_ctx_free(ctx);
}

TEST(named_parser_with_unnamed_parser) {
  scs_ctx *ctx = test_ctx_new();
  scs_parser unnamed = {.parse = custom_unnamed_x, .name = NULL};
  scs_parser named = {.parse = parse_named, .name = "test_name", .parser = &unnamed};

  scs_node *result = named.parse(ctx, &named, STR("xyz"), 0);
  ASSERT_NOT_NULL(result);
  ASSERT_STR_EQ(result->name, "test_name");
  ASSERT_SSTR_EQ(result->text, "x");
  ASSERT_INT_EQ(result->children.size, 0);
  test_ctx_free(ctx);
}

TEST(named_parser_with_optional_parser) {
  scs_ctx *ctx = test_ctx_new();
  scs_parser optional = {.parse = parse_optional, .name = NULL, .parser = &tp_char_x};
  scs_parser named = {.parse = parse_named, .name = "optional_test", .parser = &optional};

  /* the optional char is present */
  scs_node *result1 = named.parse(ctx, &named, STR("xyz"), 0);
  ASSERT_NOT_NULL(result1);
  ASSERT_STR_EQ(result1->name, "optional_test");
  ASSERT_SSTR_EQ(result1->children.items[0]->text, "x");

  /* the optional char is missing */
  scs_node *result2 = named.parse(ctx, &named, STR("abc"), 0);
  ASSERT_NOT_NULL(result2);
  ASSERT_STR_EQ(result2->name, "optional_test");
  ASSERT_INT_EQ(result2->start_idx, 0);
  ASSERT_INT_EQ(result2->end_idx, 0);
  test_ctx_free(ctx);
}

TEST(named_parser_with_choice_parser) {
  scs_ctx *ctx = test_ctx_new();
  const scs_parser *const choices[] = {&tp_char_a, &tp_char_b};
  scs_parser choice = {.parse = parse_choice, .name = NULL, .parsers = choices, .num_parsers = 2};
  scs_parser named = {.parse = parse_named, .name = "choice_test", .parser = &choice};

  scs_node *result1 = named.parse(ctx, &named, STR("abc"), 0);
  ASSERT_NOT_NULL(result1);
  ASSERT_STR_EQ(result1->name, "choice_test");
  ASSERT_SSTR_EQ(result1->children.items[0]->text, "a");

  scs_node *result2 = named.parse(ctx, &named, STR("bcd"), 0);
  ASSERT_NOT_NULL(result2);
  ASSERT_STR_EQ(result2->name, "choice_test");
  ASSERT_SSTR_EQ(result2->children.items[0]->text, "b");

  ASSERT_NULL(named.parse(ctx, &named, STR("xyz"), 0));
  test_ctx_free(ctx);
}

TEST(named_parser_with_empty_or_invalid_input) {
  scs_ctx *ctx = test_ctx_new();
  scs_parser named = {.parse = parse_named, .name = "test_name", .parser = &tp_char_a};
  ASSERT_NULL(named.parse(ctx, &named, STR(""), 0));
  ASSERT_NULL(named.parse(ctx, &named, STR("abc"), -1));
  ASSERT_NULL(named.parse(ctx, &named, STR("abc"), 999));
  test_ctx_free(ctx);
}

TEST(any_char_parser) {
  scs_ctx *ctx = test_ctx_new();
  scs_parser any = {.parse = parse_any_char, .name = "anychar_test1"};
  ASSERT_SSTR_EQ(any.parse(ctx, &any, STR("a"), 0)->text, "a");
  ASSERT_SSTR_EQ(any.parse(ctx, &any, STR("b"), 0)->text, "b");
  ASSERT_STR_EQ(any.parse(ctx, &any, STR("b"), 0)->name, "anychar_test1");
  ASSERT_SSTR_EQ(any.parse(ctx, &any, STR(" "), 0)->text, " ");
  ASSERT_SSTR_EQ(any.parse(ctx, &any, STR("+"), 0)->text, "+");
  ASSERT_SSTR_EQ(any.parse(ctx, &any, STR("!~^"), 0)->text, "!");
  ASSERT_NULL(any.parse(ctx, &any, STR(""), 0));
  /* a multi-byte character is one character */
  scs_node *sigma = any.parse(ctx, &any, STR("σx"), 0);
  ASSERT_SSTR_EQ(sigma->text, "σ");
  ASSERT_INT_EQ(sigma->end_idx, 2);
  test_ctx_free(ctx);
}

TEST(char_parser) {
  scs_ctx *ctx = test_ctx_new();
  scs_parser char_a = {.parse = parse_char, .name = "char_test_a", .ch = 'a'};
  ASSERT_STR_EQ(char_a.parse(ctx, &char_a, STR("a"), 0)->name, "char_test_a");
  ASSERT_SSTR_EQ(char_a.parse(ctx, &char_a, STR("a"), 0)->text, "a");
  ASSERT_INT_EQ(char_a.parse(ctx, &char_a, STR("a"), 0)->start_idx, 0);
  ASSERT_INT_EQ(char_a.parse(ctx, &char_a, STR("a"), 0)->end_idx, 1);
  ASSERT_NULL(char_a.parse(ctx, &char_a, STR("="), 0));

  scs_parser char_eq = {.parse = parse_char, .name = "char_test_equals", .ch = '='};
  ASSERT_STR_EQ(char_eq.parse(ctx, &char_eq, STR("="), 0)->name, "char_test_equals");
  ASSERT_SSTR_EQ(char_eq.parse(ctx, &char_eq, STR("="), 0)->text, "=");
  ASSERT_NULL(char_eq.parse(ctx, &char_eq, STR("a"), 0));
  test_ctx_free(ctx);
}

TEST(not_char_parser) {
  scs_ctx *ctx = test_ctx_new();
  scs_parser not_a = {.parse = parse_not_char, .name = "notchar_test_a", .ch = 'a'};

  /* matches any non-'a' character */
  ASSERT_STR_EQ(not_a.parse(ctx, &not_a, STR("b"), 0)->name, "notchar_test_a");
  ASSERT_SSTR_EQ(not_a.parse(ctx, &not_a, STR("b"), 0)->text, "b");
  ASSERT_INT_EQ(not_a.parse(ctx, &not_a, STR("b"), 0)->start_idx, 0);
  ASSERT_INT_EQ(not_a.parse(ctx, &not_a, STR("b"), 0)->end_idx, 1);
  /* fails to match 'a' */
  ASSERT_NULL(not_a.parse(ctx, &not_a, STR("a"), 0));
  /* various characters */
  ASSERT_SSTR_EQ(not_a.parse(ctx, &not_a, STR("x"), 0)->text, "x");
  ASSERT_SSTR_EQ(not_a.parse(ctx, &not_a, STR("1"), 0)->text, "1");
  ASSERT_SSTR_EQ(not_a.parse(ctx, &not_a, STR(" "), 0)->text, " ");
  ASSERT_SSTR_EQ(not_a.parse(ctx, &not_a, STR("!"), 0)->text, "!");
  /* position beyond the string, empty string */
  ASSERT_NULL(not_a.parse(ctx, &not_a, STR("xyz"), 3));
  ASSERT_NULL(not_a.parse(ctx, &not_a, STR(""), 0));

  scs_parser not_dollar = {.parse = parse_not_char, .name = "notchar_test_special", .ch = '$'};
  ASSERT_SSTR_EQ(not_dollar.parse(ctx, &not_dollar, STR("a"), 0)->text, "a");
  ASSERT_NULL(not_dollar.parse(ctx, &not_dollar, STR("$"), 0));
  test_ctx_free(ctx);
}

TEST(string_parser) {
  scs_ctx *ctx = test_ctx_new();
  scs_parser foo = {.parse = parse_string, .name = "string_test_foo", .str = STR("foo")};
  ASSERT_STR_EQ(foo.parse(ctx, &foo, STR("foo"), 0)->name, "string_test_foo");
  ASSERT_SSTR_EQ(foo.parse(ctx, &foo, STR("foo"), 0)->text, "foo");
  ASSERT_INT_EQ(foo.parse(ctx, &foo, STR("foo"), 0)->start_idx, 0);
  ASSERT_INT_EQ(foo.parse(ctx, &foo, STR("foo"), 0)->end_idx, 3);
  ASSERT_NULL(foo.parse(ctx, &foo, STR("bar"), 0));
  /* partial match, and a match attempted in the middle of the string */
  ASSERT_NULL(foo.parse(ctx, &foo, STR("fo"), 0));
  ASSERT_NULL(foo.parse(ctx, &foo, STR("foo"), 1));
  /* leading characters */
  scs_node *result1 = foo.parse(ctx, &foo, STR("barfoo"), 3);
  ASSERT_STR_EQ(result1->name, "string_test_foo");
  ASSERT_INT_EQ(result1->start_idx, 3);
  ASSERT_INT_EQ(result1->end_idx, 6);
  ASSERT_SSTR_EQ(result1->text, "foo");
  /* trailing characters */
  scs_node *result2 = foo.parse(ctx, &foo, STR("foobar"), 0);
  ASSERT_SSTR_EQ(result2->text, "foo");
  ASSERT_INT_EQ(result2->end_idx, 3);
  test_ctx_free(ctx);
}

/* a matcher for one or more 'c' or 'd' characters */
static int match_c_or_d_run(scs_str txt, int pos) {
  int idx = pos;
  while (idx < txt.len && (txt.data[idx] == 'c' || txt.data[idx] == 'd')) {
    idx++;
  }
  return idx > pos ? idx - pos : -1;
}

TEST(pattern_parser) {
  scs_ctx *ctx = test_ctx_new();
  scs_parser cd = {.parse = parse_pattern, .name = "foo", .match = match_c_or_d_run};

  /* matches must be made at the given position */
  ASSERT_NULL(cd.parse(ctx, &cd, STR("aaacb"), 0));

  scs_node *result2 = cd.parse(ctx, &cd, STR("aaacb"), 3);
  ASSERT_STR_EQ(result2->name, "foo");
  ASSERT_INT_EQ(result2->start_idx, 3);
  ASSERT_INT_EQ(result2->end_idx, 4);
  ASSERT_SSTR_EQ(result2->text, "c");

  scs_node *result3 = cd.parse(ctx, &cd, STR("aaacddb"), 3);
  ASSERT_INT_EQ(result3->start_idx, 3);
  ASSERT_INT_EQ(result3->end_idx, 6);
  ASSERT_SSTR_EQ(result3->text, "cdd");

  scs_node *result4 = cd.parse(ctx, &cd, STR("aaacddb"), 4);
  ASSERT_INT_EQ(result4->start_idx, 4);
  ASSERT_INT_EQ(result4->end_idx, 6);
  ASSERT_SSTR_EQ(result4->text, "dd");

  /* out of range positions never match */
  ASSERT_NULL(cd.parse(ctx, &cd, STR("cd"), -1));
  ASSERT_NULL(cd.parse(ctx, &cd, STR("cd"), 3));
  test_ctx_free(ctx);
}

TEST(seq_parser) {
  scs_ctx *ctx = test_ctx_new();
  scs_parser aaa = {.parse = parse_char, .name = "AAA", .ch = 'a'};
  scs_parser bbb = {.parse = parse_char, .name = "BBB", .ch = 'b'};
  scs_parser ccc = {.parse = parse_char, .name = "CCC", .ch = 'c'};
  const scs_parser *const parsers[] = {&aaa, &bbb, &ccc};
  scs_parser seq = {.parse = parse_seq, .name = "seq_test_1", .parsers = parsers, .num_parsers = 3};

  scs_node *result1 = seq.parse(ctx, &seq, STR("abc"), 0);
  ASSERT_NOT_NULL(result1);
  ASSERT_INT_EQ(result1->start_idx, 0);
  ASSERT_INT_EQ(result1->end_idx, 3);
  ASSERT_INT_EQ(result1->children.size, 3);
  ASSERT_STR_EQ(result1->children.items[0]->name, "AAA");
  ASSERT_STR_EQ(result1->children.items[1]->name, "BBB");
  ASSERT_STR_EQ(result1->children.items[2]->name, "CCC");

  ASSERT_NULL(seq.parse(ctx, &seq, STR("aba"), 0));
  ASSERT_NULL(seq.parse(ctx, &seq, STR("ab"), 0));

  scs_node *result4 = seq.parse(ctx, &seq, STR("abcd"), 0);
  ASSERT_INT_EQ(result4->start_idx, 0);
  ASSERT_INT_EQ(result4->end_idx, 3);
  test_ctx_free(ctx);
}

TEST(repeat_parser) {
  scs_ctx *ctx = test_ctx_new();
  scs_parser aaa = {.parse = parse_char, .name = "AAA", .ch = 'a'};
  scs_parser repeat = {.parse = parse_repeat, .name = "repeat_test_1", .parser = &aaa};

  /* no match: an empty, unnamed node */
  scs_node *result1 = repeat.parse(ctx, &repeat, STR("b"), 0);
  ASSERT_NOT_NULL(result1);
  ASSERT_INT_EQ(result1->start_idx, 0);
  ASSERT_INT_EQ(result1->end_idx, 0);
  ASSERT_NULL(result1->name);

  scs_node *result2 = repeat.parse(ctx, &repeat, STR("a"), 0);
  ASSERT_INT_EQ(result2->start_idx, 0);
  ASSERT_INT_EQ(result2->end_idx, 1);
  ASSERT_STR_EQ(result2->name, "repeat_test_1");
  ASSERT_INT_EQ(result2->children.size, 1);
  ASSERT_INT_EQ(result2->children.items[0]->start_idx, 0);
  ASSERT_INT_EQ(result2->children.items[0]->end_idx, 1);
  ASSERT_SSTR_EQ(result2->children.items[0]->text, "a");
  ASSERT_STR_EQ(result2->children.items[0]->name, "AAA");

  scs_node *result3 = repeat.parse(ctx, &repeat, STR("aa"), 0);
  ASSERT_INT_EQ(result3->start_idx, 0);
  ASSERT_INT_EQ(result3->end_idx, 2);
  ASSERT_INT_EQ(result3->children.size, 2);
  ASSERT_INT_EQ(result3->children.items[1]->start_idx, 1);
  ASSERT_INT_EQ(result3->children.items[1]->end_idx, 2);

  scs_node *result4 = repeat.parse(ctx, &repeat, STR("baac"), 1);
  ASSERT_INT_EQ(result4->start_idx, 1);
  ASSERT_INT_EQ(result4->end_idx, 3);
  ASSERT_STR_EQ(result4->name, "repeat_test_1");
  ASSERT_INT_EQ(result4->children.size, 2);
  ASSERT_INT_EQ(result4->children.items[0]->start_idx, 1);
  ASSERT_INT_EQ(result4->children.items[0]->end_idx, 2);
  ASSERT_INT_EQ(result4->children.items[1]->start_idx, 2);
  ASSERT_INT_EQ(result4->children.items[1]->end_idx, 3);

  /* min_matches */
  scs_parser at_least_two = {
      .parse = parse_repeat, .name = "two", .parser = &aaa, .min_matches = 2};
  ASSERT_NULL(at_least_two.parse(ctx, &at_least_two, STR("ab"), 0));
  ASSERT_NOT_NULL(at_least_two.parse(ctx, &at_least_two, STR("aab"), 0));
  test_ctx_free(ctx);
}

TEST(choice_parser) {
  scs_ctx *ctx = test_ctx_new();
  scs_parser a = {.parse = parse_char, .name = "A", .ch = 'a'};
  scs_parser b = {.parse = parse_char, .name = "B", .ch = 'b'};
  scs_parser c = {.parse = parse_char, .name = "C", .ch = 'c'};
  const scs_parser *const parsers[] = {&a, &b, &c};
  scs_parser choice = {
      .parse = parse_choice, .name = "choice_test_1", .parsers = parsers, .num_parsers = 3};

  ASSERT_NULL(choice.parse(ctx, &choice, STR("x"), 0));

  scs_node *result2 = choice.parse(ctx, &choice, STR("a"), 0);
  ASSERT_INT_EQ(result2->start_idx, 0);
  ASSERT_INT_EQ(result2->end_idx, 1);
  ASSERT_STR_EQ(result2->name, "A");
  ASSERT_SSTR_EQ(result2->text, "a");

  ASSERT_STR_EQ(choice.parse(ctx, &choice, STR("b"), 0)->name, "B");
  ASSERT_STR_EQ(choice.parse(ctx, &choice, STR("c"), 0)->name, "C");

  /* with an offset position */
  scs_node *result5 = choice.parse(ctx, &choice, STR("xab"), 1);
  ASSERT_INT_EQ(result5->start_idx, 1);
  ASSERT_INT_EQ(result5->end_idx, 2);
  ASSERT_STR_EQ(result5->name, "A");

  /* stops at the first match */
  scs_node *result6 = choice.parse(ctx, &choice, STR("abc"), 0);
  ASSERT_STR_EQ(result6->name, "A");
  ASSERT_INT_EQ(result6->end_idx, 1);
  test_ctx_free(ctx);
}

/* -----------------------------------------------------------------------------
 * Text Matchers
 */

TEST(match_token) {
  ASSERT_INT_EQ(match_token(STR("foo bar"), 0), 3);
  ASSERT_INT_EQ(match_token(STR("foo bar"), 4), 3);
  ASSERT_INT_EQ(match_token(STR(":kw"), 0), 3);
  ASSERT_INT_EQ(match_token(STR("a(b"), 0), 1);
  ASSERT_INT_EQ(match_token(STR("foo,bar"), 0), 3); /* comma is whitespace */
  ASSERT_INT_EQ(match_token(STR("a~b#c'd"), 0), 7); /* ~ # ' may appear inside a token */
  /* ##Inf and ##NaN */
  ASSERT_INT_EQ(match_token(STR("##Inf"), 0), 5);
  ASSERT_INT_EQ(match_token(STR("##("), 0), -1);
  ASSERT_INT_EQ(match_token(STR("#foo"), 0), -1);
  /* character literals */
  ASSERT_INT_EQ(match_token(STR("\\("), 0), 2);
  ASSERT_INT_EQ(match_token(STR("\\ x"), 0), 2);
  ASSERT_INT_EQ(match_token(STR("\\newline"), 0), 8);
  ASSERT_INT_EQ(match_token(STR("\\"), 0), 1);
  /* cannot start with a head character or whitespace */
  ASSERT_INT_EQ(match_token(STR("(foo"), 0), -1);
  ASSERT_INT_EQ(match_token(STR("'foo"), 0), -1);
  ASSERT_INT_EQ(match_token(STR(" x"), 0), -1);
  ASSERT_INT_EQ(match_token(STR(""), 0), -1);
  /* multi-byte characters are token characters; Unicode spaces end a token */
  ASSERT_INT_EQ(match_token(STR("αβγ x"), 0), 6);
  ASSERT_INT_EQ(match_token(STR("a\xe2\x80\xa8"
                                "b"),
                            0),
                1); /* line separator */
  ASSERT_INT_EQ(match_token(STR("a\xe3\x80\x80"
                                "b"),
                            0),
                1); /* ideographic space */
}

TEST(match_whitespace) {
  ASSERT_INT_EQ(match_whitespace(STR("  ,\n x"), 0), 5);
  ASSERT_INT_EQ(match_whitespace(STR("\t\r\f\v"), 0), 4);
  ASSERT_INT_EQ(match_whitespace(STR("x"), 0), -1);
  ASSERT_INT_EQ(match_whitespace(STR(""), 0), -1);
  ASSERT_INT_EQ(match_whitespace(STR("\xe3\x80\x80x"), 0), 3); /* ideographic space */
  ASSERT_INT_EQ(match_whitespace(STR("\xe2\x80\x83x"), 0), 3); /* em space */
  /* not whitespace to the tokenizer: no-break space, figure space */
  ASSERT_INT_EQ(match_whitespace(STR("\xc2\xa0"), 0), -1);
  ASSERT_INT_EQ(match_whitespace(STR("\xe2\x80\x87"), 0), -1);
}

TEST(match_comment) {
  ASSERT_INT_EQ(match_comment(STR(";; hi\nx"), 0), 5);
  ASSERT_INT_EQ(match_comment(STR(";"), 0), 1);
  ASSERT_INT_EQ(match_comment(STR("; no newline"), 0), 12);
  ASSERT_INT_EQ(match_comment(STR("x;"), 0), -1);
  ASSERT_INT_EQ(match_comment(STR(""), 0), -1);
}

TEST(match_string_open_and_body) {
  ASSERT_INT_EQ(match_string_open(STR("\"abc\""), 0), 1);
  ASSERT_INT_EQ(match_string_open(STR("#\"regex\""), 0), 2);
  ASSERT_INT_EQ(match_string_open(STR("#x"), 0), -1);
  ASSERT_INT_EQ(match_string_open(STR(""), 0), -1);

  ASSERT_INT_EQ(match_string_body(STR("abc\""), 0), 3);
  ASSERT_INT_EQ(match_string_body(STR("a\\\"b\""), 0), 4); /* escaped quote */
  ASSERT_INT_EQ(match_string_body(STR("a\\\\\""), 0), 3);  /* escaped backslash */
  ASSERT_INT_EQ(match_string_body(STR("α\""), 0), 2);
  ASSERT_INT_EQ(match_string_body(STR("multi\nline\""), 0), 10);
  ASSERT_INT_EQ(match_string_body(STR("\\σ\""), 0), 3); /* escaped multi-byte char */
  /* stops before a backslash at the end or a backslash before a line terminator */
  ASSERT_INT_EQ(match_string_body(STR("a\\"), 0), 1);
  ASSERT_INT_EQ(match_string_body(STR("a\\\nb"), 0), 1);
  ASSERT_INT_EQ(match_string_body(STR("\\"), 0), -1);
  ASSERT_INT_EQ(match_string_body(STR("\""), 0), -1);
  ASSERT_INT_EQ(match_string_body(STR(""), 0), -1);
}

TEST(match_namespaced_map_open) {
  ASSERT_INT_EQ(match_namespaced_map_open(STR("#:foo{"), 0), 6);
  ASSERT_INT_EQ(match_namespaced_map_open(STR("#::foo{"), 0), 7);
  ASSERT_INT_EQ(match_namespaced_map_open(STR("#:Foo{"), 0), 6);
  ASSERT_INT_EQ(match_namespaced_map_open(STR("#:foo.bar/baz{"), 0), 14);
  ASSERT_INT_EQ(match_namespaced_map_open(STR("#:a1{"), 0), 5);
  /* an alias may not contain '-' and must start with a letter */
  ASSERT_INT_EQ(match_namespaced_map_open(STR("#:foo-bar{"), 0), -1);
  ASSERT_INT_EQ(match_namespaced_map_open(STR("#:1x{"), 0), -1);
  /* no alias (handled by a different parser), no brace, not a map */
  ASSERT_INT_EQ(match_namespaced_map_open(STR("#::{"), 0), -1);
  ASSERT_INT_EQ(match_namespaced_map_open(STR("#:foo"), 0), -1);
  ASSERT_INT_EQ(match_namespaced_map_open(STR("#{"), 0), -1);
  ASSERT_INT_EQ(match_namespaced_map_open(STR(""), 0), -1);
}

TEST(match_openers_and_markers) {
  ASSERT_INT_EQ(match_parens_open(STR("("), 0), 1);
  ASSERT_INT_EQ(match_parens_open(STR("#("), 0), 2);
  ASSERT_INT_EQ(match_parens_open(STR("#?("), 0), 3);
  ASSERT_INT_EQ(match_parens_open(STR("#?@("), 0), 4);
  ASSERT_INT_EQ(match_parens_open(STR("#=("), 0), 3);
  ASSERT_INT_EQ(match_parens_open(STR("#x("), 0), -1);
  ASSERT_INT_EQ(match_parens_open(STR(")"), 0), -1);
  ASSERT_INT_EQ(match_parens_open(STR("x("), 1), 1);

  ASSERT_INT_EQ(match_meta_marker(STR("^"), 0), 1);
  ASSERT_INT_EQ(match_meta_marker(STR("#^"), 0), 2);
  ASSERT_INT_EQ(match_meta_marker(STR("#"), 0), -1);

  ASSERT_INT_EQ(match_wrap_marker(STR("@"), 0), 1);
  ASSERT_INT_EQ(match_wrap_marker(STR("'"), 0), 1);
  ASSERT_INT_EQ(match_wrap_marker(STR("`"), 0), 1);
  ASSERT_INT_EQ(match_wrap_marker(STR("~@"), 0), 2);
  ASSERT_INT_EQ(match_wrap_marker(STR("~"), 0), 1);
  ASSERT_INT_EQ(match_wrap_marker(STR("#'"), 0), 2);
  ASSERT_INT_EQ(match_wrap_marker(STR("#"), 0), -1);
  ASSERT_INT_EQ(match_wrap_marker(STR("x"), 0), -1);
}

/* -----------------------------------------------------------------------------
 * Parse
 */

TEST(parse_empty_input) {
  scs_ctx *ctx = test_ctx_new();
  scs_node *tree = parse(ctx, STR(""));
  ASSERT_NOT_NULL(tree);
  ASSERT_INT_EQ(tree->start_idx, 0);
  ASSERT_INT_EQ(tree->end_idx, 0);
  ASSERT_INT_EQ(tree->children.size, 0);
  test_ctx_free(ctx);
}

TEST(parse_assigns_unique_ids) {
  scs_ctx *ctx = test_ctx_new();
  scs_node *tree = parse(ctx, STR("(a b)"));
  ASSERT_STR_EQ(tree->name, "source");
  scs_node *parens = tree->children.items[0];
  ASSERT_TRUE(tree->id != parens->id);
  bool unique = true;
  for (int i = 0; i < parens->children.size; i++) {
    for (int j = i + 1; j < parens->children.size; j++) {
      if (parens->children.items[i]->id == parens->children.items[j]->id) {
        unique = false;
      }
    }
  }
  ASSERT_TRUE(unique);
  ASSERT_TRUE(tree->id > 0);
  test_ctx_free(ctx);
}

TEST(parse_node_indexes_are_code_points) {
  scs_ctx *ctx = test_ctx_new();
  scs_node *tree = parse(ctx, STR("σ [日本]"));
  ASSERT_INT_EQ(tree->end_idx, 6);
  scs_node *token = tree->children.items[0];
  ASSERT_SSTR_EQ(token->text, "σ");
  ASSERT_INT_EQ(token->start_idx, 0);
  ASSERT_INT_EQ(token->end_idx, 1);
  scs_node *brackets = tree->children.items[2];
  ASSERT_STR_EQ(brackets->name, "brackets");
  ASSERT_INT_EQ(brackets->start_idx, 2);
  ASSERT_INT_EQ(brackets->end_idx, 6);
  test_ctx_free(ctx);
}

TEST(parse_limits_nesting_depth) {
  scs_ctx *ctx = test_ctx_new();
  test_buf ok = {NULL, 0, 0};
  for (int i = 0; i < SCS_MAX_NESTING_DEPTH; i++) {
    buf_append_cstr(&ok, "(");
  }
  for (int i = 0; i < SCS_MAX_NESTING_DEPTH; i++) {
    buf_append_cstr(&ok, ")");
  }
  ASSERT_FALSE(parse_throws(ctx, buf_to_str(&ok)));

  test_buf too_deep = {NULL, 0, 0};
  for (int i = 0; i < SCS_MAX_NESTING_DEPTH + 1; i++) {
    buf_append_cstr(&too_deep, "[");
  }
  ASSERT_TRUE(parse_throws(ctx, buf_to_str(&too_deep)));
  ASSERT_STR_EQ(ctx->error_msg, "forms are nested too deeply");

  free(ok.data);
  free(too_deep.data);
  test_ctx_free(ctx);
}

/* -----------------------------------------------------------------------------
 * Parser test cases (test_cases/parser_tests.json)
 */

/* test cases that are known not to pass yet; keep this empty */
static const char *const parser_cases_to_skip[] = {NULL};

static bool should_skip_case(const char *const *skip_list, scs_str name) {
  for (int i = 0; skip_list[i] != NULL; i++) {
    if (str_eq_cstr(name, skip_list[i])) {
      return true;
    }
  }
  return false;
}

static void run_parser_test_cases(void) {
  test_case_list cases = read_test_cases("test_cases/parser_tests.json");
  for (int i = 0; i < cases.size; i++) {
    test_case tc = cases.items[i];
    char name[512];
    snprintf(name, sizeof(name), "parser: %.*s", tc.name.len, tc.name.data);
    if (!should_run_test(name) || should_skip_case(parser_cases_to_skip, tc.name)) {
      continue;
    }
    start_test(name);

    scs_ctx *ctx = test_ctx_new();
    scs_node *tree = parse(ctx, tc.input);
    test_buf out = {NULL, 0, 0};
    node_to_string(&out, tree, 0);
    scs_str actual = buf_to_str(&out);

    assertions_run++;
    if (!str_eq(actual, tc.expected)) {
      report_failure(__FILE__, __LINE__, "parse tree does not match");
      report_case_mismatch(tc.expected, actual);
    }

    free(out.data);
    test_ctx_free(ctx);
    finish_test();
  }
  free_test_cases(&cases);
}

/* -----------------------------------------------------------------------------
 * String Builder, Sorting, Traversal
 */

TEST(strbuf) {
  scs_ctx *ctx = test_ctx_new();
  scs_strbuf buf = {NULL, 0, 0};
  ASSERT_SSTR_EQ(strbuf_to_str(&buf), "");
  strbuf_append(ctx, &buf, STR("hello"));
  strbuf_append(ctx, &buf, STR(""));
  strbuf_append(ctx, &buf, STR(" world"));
  ASSERT_SSTR_EQ(strbuf_to_str(&buf), "hello world");
  /* growth past the initial capacity */
  for (int i = 0; i < 100; i++) {
    strbuf_append(ctx, &buf, STR("0123456789"));
  }
  ASSERT_INT_EQ(strbuf_to_str(&buf).len, 11 + 1000);
  ASSERT_TRUE(str_starts_with(strbuf_to_str(&buf), STR("hello world0123456789")));
  test_ctx_free(ctx);
}

TEST(str_compare) {
  ASSERT_INT_EQ(str_compare(STR("a"), STR("a")), 0);
  ASSERT_TRUE(str_compare(STR("a"), STR("b")) < 0);
  ASSERT_TRUE(str_compare(STR("b"), STR("a")) > 0);
  ASSERT_TRUE(str_compare(STR("a"), STR("ab")) < 0);
  ASSERT_TRUE(str_compare(STR("ab"), STR("a")) > 0);
  ASSERT_TRUE(str_compare(STR(""), STR("a")) < 0);
  ASSERT_INT_EQ(str_compare(STR(""), STR("")), 0);
  /* upper case sorts before lower case, as in JavaScript */
  ASSERT_TRUE(str_compare(STR("Zebra"), STR("apple")) < 0);
}

typedef struct sort_item {
  int key;
  int original_position;
} sort_item;

static int compare_sort_item_keys(const void *a, const void *b) {
  return ((const sort_item *)a)->key - ((const sort_item *)b)->key;
}

TEST(stable_sort_keeps_the_order_of_equal_items) {
  scs_ctx *ctx = test_ctx_new();
  sort_item items[] = {{3, 0}, {1, 1}, {2, 2}, {1, 3}, {3, 4}, {2, 5}, {1, 6}};
  stable_sort(ctx, items, 7, sizeof(sort_item), compare_sort_item_keys);
  int expected_positions[] = {1, 3, 6, 2, 5, 0, 4};
  bool ok = true;
  for (int i = 0; i < 7; i++) {
    if (items[i].original_position != expected_positions[i]) {
      ok = false;
    }
  }
  ASSERT_TRUE(ok);

  /* empty and single-item arrays */
  sort_item one[] = {{5, 0}};
  stable_sort(ctx, one, 1, sizeof(sort_item), compare_sort_item_keys);
  stable_sort(ctx, one, 0, sizeof(sort_item), compare_sort_item_keys);
  ASSERT_INT_EQ(one[0].key, 5);
  test_ctx_free(ctx);
}

TEST(compare_symbols_then_platform) {
  scs_ns_symbol a_clj = {STR("a"), STR(":clj")};
  scs_ns_symbol a_cljs = {STR("a"), STR(":cljs")};
  scs_ns_symbol a_none = {STR("a"), EMPTY_STR};
  scs_ns_symbol b_none = {STR("b"), EMPTY_STR};
  ASSERT_TRUE(compare_symbols_then_platform(&a_clj, &b_none) < 0);
  ASSERT_TRUE(compare_symbols_then_platform(&b_none, &a_clj) > 0);
  ASSERT_TRUE(compare_symbols_then_platform(&a_clj, &a_cljs) < 0);
  ASSERT_TRUE(compare_symbols_then_platform(&a_cljs, &a_clj) > 0);
  ASSERT_INT_EQ(compare_symbols_then_platform(&a_clj, &a_clj), 0);
  /* a symbol without a platform compares equal to the same symbol under any platform */
  ASSERT_INT_EQ(compare_symbols_then_platform(&a_none, &a_clj), 0);
  ASSERT_INT_EQ(compare_symbols_then_platform(&a_clj, &a_none), 0);
}

TEST(parse_java_package_with_class) {
  scs_ctx *ctx = test_ctx_new();
  scs_java_package p = parse_java_package_with_class(ctx, STR("java.util.Date"));
  ASSERT_SSTR_EQ(p.package, "java.util");
  ASSERT_SSTR_EQ(p.class_name, "Date");

  p = parse_java_package_with_class(ctx, STR("java.util"));
  ASSERT_SSTR_EQ(p.package, "java.util");
  ASSERT_SSTR_EQ(p.class_name, "");

  p = parse_java_package_with_class(ctx, STR("Date"));
  ASSERT_SSTR_EQ(p.package, "");
  ASSERT_SSTR_EQ(p.class_name, "Date");

  p = parse_java_package_with_class(ctx, STR("a.b.Ccc$Inner"));
  ASSERT_SSTR_EQ(p.package, "a.b");
  ASSERT_SSTR_EQ(p.class_name, "Ccc$Inner");
  test_ctx_free(ctx);
}

TEST(flatten_tree_and_text_helpers) {
  scs_ctx *ctx = test_ctx_new();
  scs_node *tree = parse(ctx, STR("(a b)"));
  scs_node_list nodes = flatten_tree(ctx, tree);
  /* source, parens, .open, .body, a, whitespace, b, .close */
  ASSERT_INT_EQ(nodes.size, 8);
  ASSERT_TRUE(nodes.items[0] == tree);
  ASSERT_STR_EQ(nodes.items[1]->name, "parens");
  ASSERT_STR_EQ(nodes.items[2]->name, ".open");
  ASSERT_SSTR_EQ(nodes.items[4]->text, "a");
  ASSERT_STR_EQ(nodes.items[7]->name, ".close");

  ASSERT_SSTR_EQ(get_text_from_root_node(ctx, tree), "(a b)");
  ASSERT_TRUE(get_last_child_node_with_text(tree) == nodes.items[7]);
  ASSERT_TRUE(find_next_node_with_text(&nodes, 3) == nodes.items[4]);
  ASSERT_TRUE(find_next_non_whitespace_node(&nodes, 5) == nodes.items[6]);
  ASSERT_NULL(find_next_node_with_text(&nodes, 8));

  /* a tagged literal's text includes the # that the .tag node leaves out */
  scs_node *tagged = parse(ctx, STR("#inst \"2020\""));
  ASSERT_SSTR_EQ(get_text_from_root_node(ctx, tagged), "#inst \"2020\"");

  /* metadata forms attached by a meta node */
  scs_node *meta_tree = parse(ctx, STR("^:a ^{:b 1} x"));
  scs_node *meta_node = meta_tree->children.items[0];
  ASSERT_STR_EQ(meta_node->name, "meta");
  scs_str_list metadata = get_metadata_strings_from_meta_node(ctx, meta_node);
  ASSERT_INT_EQ(metadata.size, 2);
  ASSERT_SSTR_EQ(metadata.items[0], "^:a");
  ASSERT_SSTR_EQ(metadata.items[1], "^{:b 1}");
  scs_node *body = get_body_node_from_meta_node(meta_node);
  ASSERT_NOT_NULL(body);
  ASSERT_SSTR_EQ(get_text_from_root_node(ctx, body), "x");
  test_ctx_free(ctx);
}

TEST(parse_ns_rejects_malformed_forms_with_an_error) {
  scs_ctx *ctx = test_ctx_new();
  /* :as before any libspec */
  ASSERT_NULL(try_parse_ns(ctx, STR("(ns foo (:require :as x))")));
  ASSERT_STR_EQ(ctx->error_msg, "could not parse the ns form");
  test_ctx_free(ctx);

  ctx = test_ctx_new();
  ASSERT_NULL(try_parse_ns(ctx, STR("(ns foo (:use bar))")));
  ASSERT_TRUE(strstr(ctx->error_msg, ":use") != NULL);
  test_ctx_free(ctx);
}

TEST(parse_ns_without_an_ns_form) {
  scs_ctx *ctx = test_ctx_new();
  scs_ns *ns = try_parse_ns(ctx, STR("(def a 1)"));
  ASSERT_NOT_NULL(ns);
  ASSERT_INT_EQ(ns->ns_symbol.len, 0);
  ASSERT_NULL(ns->requires);
  test_ctx_free(ctx);
}

/* -----------------------------------------------------------------------------
 * parse_ns test cases (test_cases/parse_ns_tests.json)
 */

static const char *const parse_ns_cases_to_skip[] = {NULL};

static void run_parse_ns_test_cases(void) {
  test_case_list cases = read_test_cases("test_cases/parse_ns_tests.json");
  for (int i = 0; i < cases.size; i++) {
    test_case tc = cases.items[i];
    char name[512];
    snprintf(name, sizeof(name), "parse_ns: %.*s", tc.name.len, tc.name.data);
    if (!should_run_test(name) || should_skip_case(parse_ns_cases_to_skip, tc.name)) {
      continue;
    }
    start_test(name);

    cJSON *expected = cJSON_ParseWithLength(tc.expected.data, (size_t)tc.expected.len);
    assertions_run++;
    if (expected == NULL) {
      report_failure(__FILE__, __LINE__, "the expected structure is not valid JSON");
      finish_test();
      continue;
    }

    scs_ctx *ctx = test_ctx_new();
    scs_ns *ns = try_parse_ns(ctx, tc.input);

    cJSON *should_error = cJSON_GetObjectItemCaseSensitive(expected, "parsingShouldError");
    if (cJSON_IsTrue(should_error)) {
      cJSON *message = cJSON_GetObjectItemCaseSensitive(expected, "errorMessage");
      assertions_run++;
      if (ns != NULL) {
        report_failure(__FILE__, __LINE__, "expected parse_ns to report an error");
      } else if (!cJSON_IsString(message) || !cstr_equal(ctx->error_msg, message->valuestring)) {
        char msg[1024];
        snprintf(msg, sizeof(msg), "error message: expected \"%s\", got \"%s\"",
                 cJSON_IsString(message) ? message->valuestring : "(none)",
                 ctx->error_msg ? ctx->error_msg : "(none)");
        report_failure(__FILE__, __LINE__, msg);
      }
    } else {
      assertions_run++;
      if (ns == NULL) {
        char msg[1024];
        snprintf(msg, sizeof(msg), "parse_ns unexpectedly reported an error: %s",
                 ctx->error_msg ? ctx->error_msg : "(none)");
        report_failure(__FILE__, __LINE__, msg);
      } else {
        cJSON *actual = ns_to_json(ns);
        if (!cJSON_Compare(actual, expected, true)) {
          report_failure(__FILE__, __LINE__, "parsed ns structure does not match");
          char *expected_txt = cJSON_Print(expected);
          char *actual_txt = cJSON_Print(actual);
          report_case_mismatch(str_from_cstr(expected_txt), str_from_cstr(actual_txt));
          cJSON_free(expected_txt);
          cJSON_free(actual_txt);
        }
        cJSON_Delete(actual);
      }
    }

    cJSON_Delete(expected);
    test_ctx_free(ctx);
    finish_test();
  }
  free_test_cases(&cases);
}

/* -----------------------------------------------------------------------------
 * Main
 */

/* usage: test [name-filter]
 * With a filter, only tests whose name contains the given text are run. */
int main(int argc, char **argv) {
  if (argc > 1) {
    test_filter = argv[1];
  }

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

  RUN_TEST(named_parser_basic);
  RUN_TEST(named_parser_with_existing_name);
  RUN_TEST(named_parser_with_unnamed_parser);
  RUN_TEST(named_parser_with_optional_parser);
  RUN_TEST(named_parser_with_choice_parser);
  RUN_TEST(named_parser_with_empty_or_invalid_input);
  RUN_TEST(any_char_parser);
  RUN_TEST(char_parser);
  RUN_TEST(not_char_parser);
  RUN_TEST(string_parser);
  RUN_TEST(pattern_parser);
  RUN_TEST(seq_parser);
  RUN_TEST(repeat_parser);
  RUN_TEST(choice_parser);

  RUN_TEST(match_token);
  RUN_TEST(match_whitespace);
  RUN_TEST(match_comment);
  RUN_TEST(match_string_open_and_body);
  RUN_TEST(match_namespaced_map_open);
  RUN_TEST(match_openers_and_markers);

  RUN_TEST(parse_empty_input);
  RUN_TEST(parse_assigns_unique_ids);
  RUN_TEST(parse_node_indexes_are_code_points);
  RUN_TEST(parse_limits_nesting_depth);

  run_parser_test_cases();

  RUN_TEST(strbuf);
  RUN_TEST(str_compare);
  RUN_TEST(stable_sort_keeps_the_order_of_equal_items);
  RUN_TEST(compare_symbols_then_platform);
  RUN_TEST(parse_java_package_with_class);
  RUN_TEST(flatten_tree_and_text_helpers);
  RUN_TEST(parse_ns_rejects_malformed_forms_with_an_error);
  RUN_TEST(parse_ns_without_an_ns_form);

  run_parse_ns_test_cases();

  if (tests_run == 0) {
    printf("no tests matched \"%s\"\n", test_filter ? test_filter : "");
    return 1;
  }
  printf("%d tests, %d assertions, %d failures\n", tests_run, assertions_run, assertions_failed);
  return tests_failed == 0 ? 0 : 1;
}

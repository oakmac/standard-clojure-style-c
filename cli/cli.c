/*
 * cli.c - the standard-clj command line interface
 * https://github.com/oakmac/standard-clojure-style-c
 *
 * Copyright (c) 2026, Chris Oakman
 * Released under the ISC license
 * https://github.com/oakmac/standard-clojure-style-c/blob/master/LICENSE.md
 *
 * This is a port of cli.mjs, cli_util.js, and cli_file_discovery.mjs from
 * Standard Clojure Style JavaScript. It has the same commands (list, check,
 * fix), options, config files, output, and exit codes.
 *
 * The formatting library (standard-clojure-style.c) is used only through its
 * public header, like any other program would use it. This file needs cJSON
 * (vendor/cjson) to read JSON config files; EDN config files are read by a
 * small parser below.
 *
 * Memory: this is a short-lived command line program, so allocation failures
 * end the process with a message (see xmalloc). Most strings are owned
 * individually and freed when done; the file lists hold owned strings.
 */

#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "cli.h"

#include "../standard-clojure-style.h"
#include "../vendor/cjson/cJSON.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
/* keep <windows.h> from defining min/max and a pile of other short macros */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

/* -----------------------------------------------------------------------------
 * Memory
 */

static void *xmalloc(size_t size) {
  void *ptr = malloc(size == 0 ? 1 : size);
  if (!ptr) {
    fputs("standard-clj: out of memory\n", stderr);
    exit(1);
  }
  return ptr;
}

static void *xrealloc(void *ptr, size_t size) {
  void *new_ptr = realloc(ptr, size == 0 ? 1 : size);
  if (!new_ptr) {
    fputs("standard-clj: out of memory\n", stderr);
    exit(1);
  }
  return new_ptr;
}

static char *xstrndup(const char *s, size_t len) {
  char *copy = xmalloc(len + 1);
  memcpy(copy, s, len);
  copy[len] = '\0';
  return copy;
}

static char *xstrdup(const char *s) {
  return xstrndup(s, strlen(s));
}

/* lets GCC and Clang check format strings against their arguments */
#if defined(__GNUC__) || defined(__clang__)
#define PRINTF_LIKE(fmt_index, args_index) __attribute__((format(printf, fmt_index, args_index)))
#else
#define PRINTF_LIKE(fmt_index, args_index)
#endif

/* GCC warns "null format string" here when the fortified vsnprintf is inlined
 * with a format string that is a variable rather than a literal. The format
 * strings all come from the literals passed to xprintf() and buf_appendf(),
 * which are checked by PRINTF_LIKE, so the warning is a false positive. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

/* vsnprintf into a newly allocated string */
static char *vxprintf(const char *fmt, va_list args) {
  va_list args2;
  va_copy(args2, args);
  /* NOTE: not named "small"; <windows.h> defines that as a macro for char */
  char stack_buf[256];
  int needed = vsnprintf(stack_buf, sizeof(stack_buf), fmt, args);
  if (needed < 0) {
    needed = 0;
  }
  char *s = xmalloc((size_t)needed + 1);
  if ((size_t)needed < sizeof(stack_buf)) {
    memcpy(s, stack_buf, (size_t)needed + 1);
  } else {
    vsnprintf(s, (size_t)needed + 1, fmt, args2);
  }
  va_end(args2);
  return s;
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

/* printf into a newly allocated string */
static PRINTF_LIKE(1, 2) char *xprintf(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char *s = vxprintf(fmt, args);
  va_end(args);
  return s;
}

/* -----------------------------------------------------------------------------
 * Growable byte buffer
 */

typedef struct buf {
  char *data; /* always NUL-terminated once anything has been appended */
  size_t len;
  size_t cap;
} buf;

static void buf_reserve(buf *b, size_t extra) {
  if (b->len + extra + 1 > b->cap) {
    size_t new_cap = b->cap == 0 ? 128 : b->cap;
    while (new_cap < b->len + extra + 1) {
      new_cap *= 2;
    }
    b->data = xrealloc(b->data, new_cap);
    b->cap = new_cap;
  }
}

static void buf_append_n(buf *b, const char *s, size_t n) {
  buf_reserve(b, n);
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = '\0';
}

static void buf_append(buf *b, const char *s) {
  buf_append_n(b, s, strlen(s));
}

static PRINTF_LIKE(2, 3) void buf_appendf(buf *b, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char *s = vxprintf(fmt, args);
  va_end(args);
  buf_append(b, s);
  free(s);
}

static const char *buf_cstr(const buf *b) {
  return b->data ? b->data : "";
}

static void buf_clear(buf *b) {
  b->len = 0;
  if (b->data) {
    b->data[0] = '\0';
  }
}

static void buf_free(buf *b) {
  free(b->data);
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

/* -----------------------------------------------------------------------------
 * List of owned strings
 */

typedef struct strlist {
  char **items;
  size_t size;
  size_t cap;
} strlist;

/* appends s, taking ownership of it */
static void strlist_push(strlist *list, char *s) {
  if (list->size == list->cap) {
    list->cap = list->cap == 0 ? 16 : list->cap * 2;
    list->items = xrealloc(list->items, list->cap * sizeof(char *));
  }
  list->items[list->size++] = s;
}

static void strlist_push_copy(strlist *list, const char *s) {
  strlist_push(list, xstrdup(s));
}

static bool strlist_contains(const strlist *list, const char *s) {
  for (size_t i = 0; i < list->size; i++) {
    if (strcmp(list->items[i], s) == 0) {
      return true;
    }
  }
  return false;
}

static int compare_cstr_ptrs(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* sorts the list (by bytes, which matches JavaScript's default sort for
 * ASCII paths) and removes duplicates */
static void strlist_sort_unique(strlist *list) {
  if (list->size < 2) {
    return;
  }
  qsort(list->items, list->size, sizeof(char *), compare_cstr_ptrs);
  size_t out = 1;
  for (size_t i = 1; i < list->size; i++) {
    if (strcmp(list->items[i], list->items[out - 1]) == 0) {
      free(list->items[i]);
    } else {
      list->items[out++] = list->items[i];
    }
  }
  list->size = out;
}

/* is s in a sorted list? */
static bool strlist_sorted_contains(const strlist *list, const char *s) {
  size_t lo = 0;
  size_t hi = list->size;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int cmp = strcmp(list->items[mid], s);
    if (cmp == 0) {
      return true;
    }
    if (cmp < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return false;
}

static void strlist_free(strlist *list) {
  for (size_t i = 0; i < list->size; i++) {
    free(list->items[i]);
  }
  free(list->items);
  list->items = NULL;
  list->size = 0;
  list->cap = 0;
}

/* -----------------------------------------------------------------------------
 * Platform: paths and the filesystem
 */

#ifdef _WIN32
#define PATH_SEP '\\'
#else
#define PATH_SEP '/'
#endif

static bool is_path_sep(char c) {
#ifdef _WIN32
  return c == '\\' || c == '/';
#else
  return c == '/';
#endif
}

static bool path_is_absolute(const char *p) {
#ifdef _WIN32
  size_t len = strlen(p);
  if (len >= 2 && p[1] == ':' && (len == 2 || is_path_sep(p[2]))) {
    return true; /* C: or C:\ */
  }
  return len >= 2 && is_path_sep(p[0]) && is_path_sep(p[1]); /* \\server\share */
#else
  return p[0] == '/';
#endif
}

/* The length of the root prefix of an absolute path: "/" on POSIX; "C:\" or
 * "\\server\share\" on Windows. Zero for a relative path. */
static size_t path_root_len(const char *p) {
#ifdef _WIN32
  size_t len = strlen(p);
  if (len >= 2 && p[1] == ':') {
    return (len >= 3 && is_path_sep(p[2])) ? 3 : 2;
  }
  if (len >= 2 && is_path_sep(p[0]) && is_path_sep(p[1])) {
    /* \\server\share\ : skip two path components */
    size_t i = 2;
    int seps = 0;
    while (i < len) {
      if (is_path_sep(p[i])) {
        seps++;
        if (seps == 2) {
          return i + 1;
        }
      }
      i++;
    }
    return len;
  }
  return 0;
#else
  return p[0] == '/' ? 1 : 0;
#endif
}

/* Normalizes an absolute path: one separator style, no empty or "." segments,
 * ".." resolved (never above the root), no trailing separator. Returns a new
 * string. This is what path.resolve() does in Node.js. */
static char *path_normalize_absolute(const char *p) {
  const char sep = PATH_SEP;
  size_t root_len = path_root_len(p);
  buf out = {NULL, 0, 0};
  for (size_t i = 0; i < root_len; i++) {
    char c = is_path_sep(p[i]) ? PATH_SEP : p[i];
    buf_append_n(&out, &c, 1);
  }
  size_t root_out_len = out.len;

  strlist segments = {NULL, 0, 0};
  const char *s = p + root_len;
  while (*s) {
    while (*s && is_path_sep(*s)) {
      s++;
    }
    const char *start = s;
    while (*s && !is_path_sep(*s)) {
      s++;
    }
    size_t seg_len = (size_t)(s - start);
    if (seg_len == 0 || (seg_len == 1 && start[0] == '.')) {
      continue;
    }
    if (seg_len == 2 && start[0] == '.' && start[1] == '.') {
      if (segments.size > 0) {
        free(segments.items[--segments.size]);
      }
      continue;
    }
    strlist_push(&segments, xstrndup(start, seg_len));
  }

  for (size_t i = 0; i < segments.size; i++) {
    if (i > 0 || (root_out_len > 0 && out.data[root_out_len - 1] != PATH_SEP)) {
      buf_append_n(&out, &sep, 1);
    }
    buf_append(&out, segments.items[i]);
  }
  strlist_free(&segments);

  if (out.len == 0) {
    buf_append_n(&out, &sep, 1);
  }
  return out.data;
}

/* joins a directory and a name with one separator */
static char *path_join(const char *dir, const char *name) {
  size_t dir_len = strlen(dir);
  if (dir_len > 0 && is_path_sep(dir[dir_len - 1])) {
    return xprintf("%s%s", dir, name);
  }
  return xprintf("%s%c%s", dir, PATH_SEP, name);
}

/* resolves p against cwd (like path.resolve(cwd, p) in Node.js) */
static char *path_resolve(const char *cwd, const char *p) {
  if (path_is_absolute(p)) {
    return path_normalize_absolute(p);
  }
  char *joined = path_join(cwd, p);
  char *resolved = path_normalize_absolute(joined);
  free(joined);
  return resolved;
}

/* the extension of a path, including the dot (like path.extname() in Node.js:
 * a name that starts with its only dot has no extension) */
static const char *path_extname(const char *p) {
  const char *base = p;
  for (const char *s = p; *s; s++) {
    if (is_path_sep(*s)) {
      base = s + 1;
    }
  }
  const char *last_dot = strrchr(base, '.');
  if (last_dot == NULL || last_dot == base) {
    return "";
  }
  return last_dot;
}

typedef enum fs_kind { FS_MISSING, FS_FILE, FS_DIRECTORY, FS_OTHER } fs_kind;

/* what is at path? (follows symbolic links) */
static fs_kind fs_stat_kind(const char *path) {
#ifdef _WIN32
  struct _stat st;
  if (_stat(path, &st) != 0) {
    return FS_MISSING;
  }
  if (st.st_mode & _S_IFDIR) {
    return FS_DIRECTORY;
  }
  if (st.st_mode & _S_IFREG) {
    return FS_FILE;
  }
  return FS_OTHER;
#else
  struct stat st;
  if (stat(path, &st) != 0) {
    return FS_MISSING;
  }
  if (S_ISDIR(st.st_mode)) {
    return FS_DIRECTORY;
  }
  if (S_ISREG(st.st_mode)) {
    return FS_FILE;
  }
  return FS_OTHER;
#endif
}

static bool fs_is_file(const char *path) {
  return fs_stat_kind(path) == FS_FILE;
}

static bool fs_is_directory(const char *path) {
  return fs_stat_kind(path) == FS_DIRECTORY;
}

/* the names of the entries in a directory (not "." and ".."), sorted */
static strlist fs_list_directory(const char *path) {
  strlist names = {NULL, 0, 0};
#ifdef _WIN32
  char *pattern = path_join(path, "*");
  WIN32_FIND_DATAA data;
  HANDLE h = FindFirstFileA(pattern, &data);
  free(pattern);
  if (h == INVALID_HANDLE_VALUE) {
    return names;
  }
  do {
    if (strcmp(data.cFileName, ".") != 0 && strcmp(data.cFileName, "..") != 0) {
      strlist_push_copy(&names, data.cFileName);
    }
  } while (FindNextFileA(h, &data));
  FindClose(h);
#else
  DIR *dir = opendir(path);
  if (!dir) {
    return names;
  }
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
      strlist_push_copy(&names, entry->d_name);
    }
  }
  closedir(dir);
#endif
  if (names.size > 1) {
    qsort(names.items, names.size, sizeof(char *), compare_cstr_ptrs);
  }
  return names;
}

/* reads a whole file; returns NULL if it cannot be read */
static char *fs_read_file(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    return NULL;
  }
  buf b = {NULL, 0, 0};
  char chunk[65536];
  size_t n;
  while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
    buf_append_n(&b, chunk, n);
  }
  bool failed = ferror(f) != 0;
  fclose(f);
  if (failed) {
    buf_free(&b);
    return NULL;
  }
  if (b.data == NULL) {
    buf_append_n(&b, "", 0);
  }
  *out_len = b.len;
  return b.data;
}

static bool fs_write_file(const char *path, const char *data, size_t len) {
  FILE *f = fopen(path, "wb");
  if (!f) {
    return false;
  }
  bool ok = fwrite(data, 1, len, f) == len;
  if (fclose(f) != 0) {
    ok = false;
  }
  return ok;
}

/* the directories nested more deeply than this are not entered (a guard
 * against symbolic link cycles, which would otherwise recurse forever) */
#define MAX_DIRECTORY_DEPTH 256

/* -----------------------------------------------------------------------------
 * Platform: terminal, clock, working directory
 */

static bool stream_is_tty(FILE *f) {
#ifdef _WIN32
  return _isatty(_fileno(f)) != 0;
#else
  return isatty(fileno(f)) != 0;
#endif
}

/* Should colors be used on this stream? The environment wins over the
 * terminal check: FORCE_COLOR=0 or NO_COLOR disable colors anywhere, any other
 * FORCE_COLOR value enables them anywhere. Otherwise colors are used when the
 * stream is a terminal (and TERM is not "dumb"). */
static bool stream_wants_color(FILE *f) {
  const char *force = getenv("FORCE_COLOR");
  if (force != NULL) {
    return strcmp(force, "0") != 0;
  }
  if (getenv("NO_COLOR") != NULL) {
    return false;
  }
  const char *term = getenv("TERM");
  if (term != NULL && strcmp(term, "dumb") == 0) {
    return false;
  }
  return stream_is_tty(f);
}

static double monotonic_now_ms(void) {
#ifdef _WIN32
  static LARGE_INTEGER frequency;
  if (frequency.QuadPart == 0) {
    QueryPerformanceFrequency(&frequency);
  }
  LARGE_INTEGER counter;
  QueryPerformanceCounter(&counter);
  return (double)counter.QuadPart * 1000.0 / (double)frequency.QuadPart;
#else
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0.0;
  }
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
#endif
}

static char *current_directory(void) {
  size_t cap = 256;
  char *dir = xmalloc(cap);
  while (true) {
#ifdef _WIN32
    if (_getcwd(dir, (int)cap) != NULL) {
      return dir;
    }
#else
    if (getcwd(dir, cap) != NULL) {
      return dir;
    }
#endif
    if (errno != ERANGE) {
      free(dir);
      return NULL;
    }
    cap *= 2;
    dir = xrealloc(dir, cap);
  }
}

#ifdef _WIN32
/* lets the Windows console interpret ANSI color sequences */
static void enable_console_colors(void) {
  DWORD handles[] = {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
  for (size_t i = 0; i < 2; i++) {
    HANDLE h = GetStdHandle(handles[i]);
    DWORD mode = 0;
    if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
      SetConsoleMode(h, mode | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */);
    }
  }
}
#endif

bool scs_cli_default_env(scs_cli_env *env) {
  memset(env, 0, sizeof(*env));
  env->out = stdout;
  env->err = stderr;
  env->in = stdin;
  env->cwd = current_directory();
  if (env->cwd == NULL) {
    fputs("standard-clj: could not determine the current directory\n", stderr);
    return false;
  }
  env->color_out = stream_wants_color(stdout);
  env->color_err = stream_wants_color(stderr);
#ifdef _WIN32
  if (env->color_out || env->color_err) {
    enable_console_colors();
  }
  /* read and write bytes exactly as they are */
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
  _setmode(_fileno(stderr), _O_BINARY);
#endif
  env->now_ms = monotonic_now_ms;
  env->version = SCS_VERSION;
#ifdef SCS_GIT_HASH
  env->version_verbose = SCS_VERSION " [" SCS_GIT_HASH "]";
#else
  env->version_verbose = SCS_VERSION;
#endif
  return true;
}

/* -----------------------------------------------------------------------------
 * Colors
 *
 * The same ANSI styles as the JavaScript CLI: bold, dim, red, green, yellow.
 */

typedef struct style {
  const char *open;
  const char *close;
} style;

static const style STYLE_BOLD = {"\x1b[1m", "\x1b[22m"};
static const style STYLE_DIM = {"\x1b[2m", "\x1b[22m"};
static const style STYLE_RED = {"\x1b[31m", "\x1b[39m"};
static const style STYLE_GREEN = {"\x1b[32m", "\x1b[39m"};
static const style STYLE_YELLOW = {"\x1b[33m", "\x1b[39m"};

/* appends text to b, wrapped in the style when colors are enabled */
static void buf_styled(buf *b, bool colors, style st, const char *text) {
  if (colors) {
    buf_append(b, st.open);
  }
  buf_append(b, text);
  if (colors) {
    buf_append(b, st.close);
  }
}

/* appends text wrapped in two styles (outer first) */
static void buf_styled2(buf *b, bool colors, style outer, style inner, const char *text) {
  if (colors) {
    buf_append(b, outer.open);
  }
  buf_styled(b, colors, inner, text);
  if (colors) {
    buf_append(b, outer.close);
  }
}

/* -----------------------------------------------------------------------------
 * CLI state
 */

typedef enum cli_log_level {
  LOG_EVERYTHING,
  LOG_IGNORE_ALREADY_FORMATTED,
  LOG_QUIET
} cli_log_level;

typedef struct cli {
  const scs_cli_env *env;
  cli_log_level log_level;
  bool at_least_one_file_printed;
  double script_start_time;
  buf line; /* scratch buffer for building one line of output */
} cli;

/* Output alternates between stdout and stderr. Flushing the other stream first
 * keeps the lines in order when both are redirected to the same place (a CI
 * log, for example), where stdout would otherwise be buffered and stderr not. */
static void print_to_stdout(cli *c, const char *s) {
  if (c->log_level != LOG_QUIET) {
    fflush(c->env->err);
    fputs(s, c->env->out);
    fputc('\n', c->env->out);
  }
}

static void print_to_stderr(cli *c, const char *s) {
  if (c->log_level != LOG_QUIET) {
    fflush(c->env->out);
    fputs(s, c->env->err);
    fputc('\n', c->env->err);
  }
}

/* prints the scratch line to stdout and clears it */
static void flush_line_to_stdout(cli *c) {
  print_to_stdout(c, buf_cstr(&c->line));
  buf_clear(&c->line);
}

static void flush_line_to_stderr(cli *c) {
  print_to_stderr(c, buf_cstr(&c->line));
  buf_clear(&c->line);
}

/* -----------------------------------------------------------------------------
 * Pure helpers (cli_util.js)
 */

/* converts a raw --log-level value into one of the three levels */
static cli_log_level normalize_log_level(const char *level) {
  if (level == NULL) {
    return LOG_EVERYTHING;
  }
  if (strcmp(level, "ignore-already-formatted") == 0 || strcmp(level, "1") == 0) {
    return LOG_IGNORE_ALREADY_FORMATTED;
  }
  if (strcmp(level, "quiet") == 0 || strcmp(level, "5") == 0) {
    return LOG_QUIET;
  }
  return LOG_EVERYTHING;
}

typedef enum classification_action {
  ACTION_ALREADY_FORMATTED,
  ACTION_FORMATTED,
  ACTION_ERROR
} classification_action;

typedef struct classification {
  classification_action action;
  char *output_text; /* the formatted text with a trailing newline (owned) */
  size_t output_len;
  char *error_message; /* owned */
} classification;

static const char *UNKNOWN_FORMAT_ERROR = "Unknown error! Please help the standard-clj project by "
                                          "opening an issue to report this \xF0\x9F\x99\x8F";

/* Given the original file text and the result of formatting it, decides what
 * to do: the file is already formatted, needs to be (re)written, or failed.
 * The output text always has exactly one trailing newline. */
static classification classify_format_result(const char *original_text, size_t original_len,
                                             const scs_format_result *result) {
  classification cls;
  memset(&cls, 0, sizeof(cls));

  if (result->status == SCS_STATUS_SUCCESS && result->out != NULL) {
    cls.output_len = result->out_len + 1;
    cls.output_text = xmalloc(cls.output_len + 1);
    memcpy(cls.output_text, result->out, result->out_len);
    cls.output_text[result->out_len] = '\n';
    cls.output_text[cls.output_len] = '\0';

    bool same =
        cls.output_len == original_len && memcmp(cls.output_text, original_text, original_len) == 0;
    cls.action = same ? ACTION_ALREADY_FORMATTED : ACTION_FORMATTED;
    return cls;
  }

  cls.action = ACTION_ERROR;
  if (result->status == SCS_STATUS_ERROR && result->reason != NULL) {
    cls.error_message = xstrdup(result->reason);
  } else {
    cls.error_message = xstrdup(UNKNOWN_FORMAT_ERROR);
  }
  return cls;
}

static void classification_free(classification *cls) {
  free(cls->output_text);
  free(cls->error_message);
  cls->output_text = NULL;
  cls->error_message = NULL;
}

static const char *file_str(size_t num_files) {
  return num_files == 1 ? "file" : "files";
}

/* ".clj" from "clj" (a string that already starts with a period is copied) */
static char *add_period_prefix(const char *ext) {
  if (ext[0] != '.') {
    return xprintf(".%s", ext);
  }
  return xstrdup(ext);
}

/* removes the first occurrence of root_dir from filename (so a file inside
 * the project prints as "/src/core.clj") */
static char *relative_filename(const char *filename, const char *root_dir) {
  if (root_dir[0] == '\0') {
    return xstrdup(filename);
  }
  const char *found = strstr(filename, root_dir);
  if (found == NULL) {
    return xstrdup(filename);
  }
  size_t root_len = strlen(root_dir);
  return xprintf("%.*s%s", (int)(found - filename), filename, found + root_len);
}

/* splits a comma-separated --file-ext value into extensions with periods */
static strlist convert_file_ext(const char *value) {
  strlist exts = {NULL, 0, 0};
  const char *s = value;
  while (true) {
    const char *comma = strchr(s, ',');
    size_t len = comma ? (size_t)(comma - s) : strlen(s);
    char *piece = xstrndup(s, len);
    char *with_period = add_period_prefix(piece);
    free(piece);
    strlist_push(&exts, with_period);
    if (!comma) {
      break;
    }
    s = comma + 1;
  }
  return exts;
}

static const char *const DEFAULT_FILE_EXTENSIONS[] = {".clj", ".cljs", ".cljc", ".jank", ".edn"};

static bool has_allowed_extension(const char *filename, const strlist *file_extensions) {
  return strlist_contains(file_extensions, path_extname(filename));
}

/* "[1.5ms]" dimmed; the number has at most two decimals and no trailing zeros */
static void append_duration(buf *b, bool colors, double duration_ms) {
  /* round to two decimals the way Math.round(ms * 100) / 100 does */
  double scaled = duration_ms * 100.0;
  double rounded = (double)(long long)(scaled + (scaled >= 0 ? 0.5 : -0.5)) / 100.0;
  char num[64];
  snprintf(num, sizeof(num), "%.2f", rounded);
  size_t len = strlen(num);
  while (len > 0 && num[len - 1] == '0') {
    num[--len] = '\0';
  }
  if (len > 0 && num[len - 1] == '.') {
    num[--len] = '\0';
  }
  char *text = xprintf("[%sms]", num);
  buf_styled(b, colors, STYLE_DIM, text);
  free(text);
}

/* -----------------------------------------------------------------------------
 * Output encoders for the "list" command
 */

/* appends s as a JSON string literal */
static void append_json_string(buf *b, const char *s) {
  buf_append(b, "\"");
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    switch (*p) {
    case '"':
      buf_append(b, "\\\"");
      break;
    case '\\':
      buf_append(b, "\\\\");
      break;
    case '\n':
      buf_append(b, "\\n");
      break;
    case '\r':
      buf_append(b, "\\r");
      break;
    case '\t':
      buf_append(b, "\\t");
      break;
    case '\b':
      buf_append(b, "\\b");
      break;
    case '\f':
      buf_append(b, "\\f");
      break;
    default:
      if (*p < 0x20) {
        buf_appendf(b, "\\u%04x", (unsigned int)*p);
      } else {
        buf_append_n(b, (const char *)p, 1);
      }
    }
  }
  buf_append(b, "\"");
}

/* ["a","b"] */
static char *encode_json_array(const strlist *items) {
  buf b = {NULL, 0, 0};
  buf_append(&b, "[");
  for (size_t i = 0; i < items->size; i++) {
    if (i > 0) {
      buf_append(&b, ",");
    }
    append_json_string(&b, items->items[i]);
  }
  buf_append(&b, "]");
  return b.data;
}

/* the two-space indented form of JSON.stringify(items, null, 2) */
static char *encode_json_array_pretty(const strlist *items) {
  if (items->size == 0) {
    return xstrdup("[]");
  }
  buf b = {NULL, 0, 0};
  buf_append(&b, "[\n");
  for (size_t i = 0; i < items->size; i++) {
    buf_append(&b, "  ");
    append_json_string(&b, items->items[i]);
    buf_append(&b, i + 1 < items->size ? ",\n" : "\n");
  }
  buf_append(&b, "]");
  return b.data;
}

/* ["a" "b"] */
static char *encode_edn_array(const strlist *items) {
  buf b = {NULL, 0, 0};
  buf_append(&b, "[");
  for (size_t i = 0; i < items->size; i++) {
    if (i > 0) {
      buf_append(&b, " ");
    }
    append_json_string(&b, items->items[i]);
  }
  buf_append(&b, "]");
  return b.data;
}

/* the compact JSON array with each "," between items turned into a newline
 * and one space: ["a"\n "b"] */
static char *encode_edn_array_pretty(const strlist *items) {
  char *json = encode_json_array(items);
  buf b = {NULL, 0, 0};
  for (const char *p = json; *p; p++) {
    if (p[0] == '"' && p[1] == ',' && p[2] == '"') {
      buf_append(&b, "\"\n \"");
      p += 2;
    } else {
      buf_append_n(&b, p, 1);
    }
  }
  free(json);
  return b.data;
}

/* -----------------------------------------------------------------------------
 * EDN reader
 *
 * Just enough EDN for a .standard-clj.edn config file: maps, vectors, lists,
 * sets, strings, keywords, symbols, numbers, comments, and #_ discards.
 */

typedef enum edn_type {
  EDN_MAP,
  EDN_VECTOR, /* also used for lists and sets */
  EDN_STRING,
  EDN_KEYWORD, /* text is the name without the leading colon */
  EDN_SYMBOL,  /* also numbers, true, false, nil */
} edn_type;

typedef struct edn_value edn_value;

struct edn_value {
  edn_type type;
  char *text;        /* EDN_STRING, EDN_KEYWORD, EDN_SYMBOL */
  edn_value **items; /* EDN_VECTOR: items; EDN_MAP: key, value, key, value, ... */
  size_t num_items;
  size_t cap_items;
};

typedef struct edn_reader {
  const char *s;
  size_t pos;
  size_t len;
  bool failed;
} edn_reader;

static void edn_free(edn_value *v) {
  if (!v) {
    return;
  }
  free(v->text);
  for (size_t i = 0; i < v->num_items; i++) {
    edn_free(v->items[i]);
  }
  free(v->items);
  free(v);
}

static edn_value *edn_new(edn_type type) {
  edn_value *v = xmalloc(sizeof(edn_value));
  memset(v, 0, sizeof(*v));
  v->type = type;
  return v;
}

static void edn_add_item(edn_value *v, edn_value *item) {
  if (v->num_items == v->cap_items) {
    v->cap_items = v->cap_items == 0 ? 8 : v->cap_items * 2;
    v->items = xrealloc(v->items, v->cap_items * sizeof(edn_value *));
  }
  v->items[v->num_items++] = item;
}

static bool edn_is_whitespace(char c) {
  return c == ' ' || c == ',' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\v';
}

static bool edn_is_delimiter(char c) {
  return edn_is_whitespace(c) || c == '(' || c == ')' || c == '[' || c == ']' || c == '{' ||
         c == '}' || c == '"' || c == ';' || c == '\0';
}

static void edn_skip_whitespace_and_comments(edn_reader *r) {
  while (r->pos < r->len) {
    char c = r->s[r->pos];
    if (edn_is_whitespace(c)) {
      r->pos++;
    } else if (c == ';') {
      while (r->pos < r->len && r->s[r->pos] != '\n') {
        r->pos++;
      }
    } else {
      break;
    }
  }
}

static edn_value *edn_read_value(edn_reader *r);

/* reads values until the closing character, into container */
static edn_value *edn_read_collection(edn_reader *r, edn_value *container, char close) {
  while (true) {
    edn_skip_whitespace_and_comments(r);
    if (r->pos >= r->len) {
      r->failed = true;
      edn_free(container);
      return NULL;
    }
    if (r->s[r->pos] == close) {
      r->pos++;
      if (container->type == EDN_MAP && container->num_items % 2 != 0) {
        r->failed = true;
        edn_free(container);
        return NULL;
      }
      return container;
    }
    edn_value *item = edn_read_value(r);
    if (item == NULL) {
      if (r->failed) {
        edn_free(container);
        return NULL;
      }
      continue; /* a discarded form */
    }
    edn_add_item(container, item);
  }
}

static void buf_append_utf8(buf *b, unsigned int code_point) {
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
  buf_append_n(b, bytes, n);
}

static edn_value *edn_read_string(edn_reader *r) {
  r->pos++; /* the opening quote */
  buf b = {NULL, 0, 0};
  while (true) {
    if (r->pos >= r->len) {
      r->failed = true;
      buf_free(&b);
      return NULL;
    }
    char c = r->s[r->pos++];
    if (c == '"') {
      break;
    }
    if (c != '\\') {
      buf_append_n(&b, &c, 1);
      continue;
    }
    if (r->pos >= r->len) {
      r->failed = true;
      buf_free(&b);
      return NULL;
    }
    char e = r->s[r->pos++];
    switch (e) {
    case '"':
    case '\\':
    case '/':
      buf_append_n(&b, &e, 1);
      break;
    case 'n':
      buf_append(&b, "\n");
      break;
    case 't':
      buf_append(&b, "\t");
      break;
    case 'r':
      buf_append(&b, "\r");
      break;
    case 'b':
      buf_append(&b, "\b");
      break;
    case 'f':
      buf_append(&b, "\f");
      break;
    case 'u': {
      unsigned int code_point = 0;
      for (int i = 0; i < 4; i++) {
        if (r->pos >= r->len) {
          r->failed = true;
          buf_free(&b);
          return NULL;
        }
        char h = r->s[r->pos++];
        unsigned int digit;
        if (h >= '0' && h <= '9') {
          digit = (unsigned int)(h - '0');
        } else if (h >= 'a' && h <= 'f') {
          digit = (unsigned int)(h - 'a' + 10);
        } else if (h >= 'A' && h <= 'F') {
          digit = (unsigned int)(h - 'A' + 10);
        } else {
          r->failed = true;
          buf_free(&b);
          return NULL;
        }
        code_point = (code_point << 4) | digit;
      }
      buf_append_utf8(&b, code_point);
      break;
    }
    default:
      r->failed = true;
      buf_free(&b);
      return NULL;
    }
  }
  if (b.data == NULL) {
    buf_append_n(&b, "", 0);
  }
  edn_value *v = edn_new(EDN_STRING);
  v->text = b.data;
  return v;
}

/* reads one value; returns NULL for a discarded form (#_ x) or on failure
 * (r->failed is then set) */
static edn_value *edn_read_value(edn_reader *r) {
  edn_skip_whitespace_and_comments(r);
  if (r->pos >= r->len) {
    r->failed = true;
    return NULL;
  }
  char c = r->s[r->pos];

  if (c == '{') {
    r->pos++;
    return edn_read_collection(r, edn_new(EDN_MAP), '}');
  }
  if (c == '[') {
    r->pos++;
    return edn_read_collection(r, edn_new(EDN_VECTOR), ']');
  }
  if (c == '(') {
    r->pos++;
    return edn_read_collection(r, edn_new(EDN_VECTOR), ')');
  }
  if (c == '"') {
    return edn_read_string(r);
  }
  if (c == '#') {
    if (r->pos + 1 < r->len && r->s[r->pos + 1] == '{') {
      r->pos += 2;
      return edn_read_collection(r, edn_new(EDN_VECTOR), '}');
    }
    if (r->pos + 1 < r->len && r->s[r->pos + 1] == '_') {
      r->pos += 2;
      edn_value *discarded = edn_read_value(r);
      edn_free(discarded);
      if (r->failed) {
        return NULL;
      }
      return NULL; /* nothing to add */
    }
    r->failed = true; /* tagged literals and other dispatch forms are not supported */
    return NULL;
  }
  if (c == ')' || c == ']' || c == '}') {
    r->failed = true;
    return NULL;
  }

  /* a keyword, symbol, or number: everything up to the next delimiter */
  size_t start = r->pos;
  while (r->pos < r->len && !edn_is_delimiter(r->s[r->pos])) {
    r->pos++;
  }
  size_t len = r->pos - start;
  if (c == ':') {
    if (len < 2) {
      r->failed = true;
      return NULL;
    }
    edn_value *v = edn_new(EDN_KEYWORD);
    v->text = xstrndup(r->s + start + 1, len - 1);
    return v;
  }
  edn_value *v = edn_new(EDN_SYMBOL);
  v->text = xstrndup(r->s + start, len);
  return v;
}

/* parses a complete EDN document holding exactly one value; NULL on failure */
static edn_value *edn_parse(const char *text, size_t len) {
  edn_reader r = {text, 0, len, false};
  edn_value *v = NULL;
  while (v == NULL && !r.failed) {
    edn_skip_whitespace_and_comments(&r);
    if (r.pos >= r.len) {
      return NULL; /* nothing but whitespace, comments, or discards */
    }
    v = edn_read_value(&r);
  }
  if (r.failed) {
    return NULL;
  }
  edn_skip_whitespace_and_comments(&r);
  while (!r.failed && r.pos < r.len) {
    /* only discards may follow the value */
    edn_value *extra = edn_read_value(&r);
    if (extra != NULL) {
      edn_free(extra);
      edn_free(v);
      return NULL;
    }
    edn_skip_whitespace_and_comments(&r);
  }
  if (r.failed) {
    edn_free(v);
    return NULL;
  }
  return v;
}

/* the value for a map key that is the keyword or string `name`, or NULL */
static const edn_value *edn_map_get(const edn_value *map, const char *name) {
  for (size_t i = 0; i + 1 < map->num_items; i += 2) {
    const edn_value *key = map->items[i];
    if ((key->type == EDN_KEYWORD || key->type == EDN_STRING) && strcmp(key->text, name) == 0) {
      return map->items[i + 1];
    }
  }
  return NULL;
}

/* -----------------------------------------------------------------------------
 * Config files
 */

typedef struct config {
  bool loaded;     /* a config file was read (even an empty one) */
  char *log_level; /* NULL when the config has none */
  bool has_include;
  strlist include;
  bool has_ignore;
  strlist ignore;
} config;

static void config_free(config *cfg) {
  free(cfg->log_level);
  strlist_free(&cfg->include);
  strlist_free(&cfg->ignore);
  memset(cfg, 0, sizeof(*cfg));
}

/* a string value becomes a one-item list; an array keeps its string items */
static void config_set_patterns_json(const cJSON *value, bool *has, strlist *list) {
  if (cJSON_IsString(value)) {
    *has = true;
    strlist_push_copy(list, value->valuestring);
  } else if (cJSON_IsArray(value)) {
    *has = true;
    const cJSON *item;
    cJSON_ArrayForEach(item, value) {
      if (cJSON_IsString(item)) {
        strlist_push_copy(list, item->valuestring);
      }
    }
  }
}

static bool load_json_config(const char *path, config *cfg) {
  size_t len = 0;
  char *text = fs_read_file(path, &len);
  if (text == NULL) {
    return false;
  }
  cJSON *root = cJSON_ParseWithLength(text, len);
  free(text);
  if (root == NULL || !cJSON_IsObject(root)) {
    cJSON_Delete(root);
    return false;
  }

  cfg->loaded = true;
  const cJSON *log_level = cJSON_GetObjectItemCaseSensitive(root, "log-level");
  if (cJSON_IsString(log_level)) {
    cfg->log_level = xstrdup(log_level->valuestring);
  } else if (cJSON_IsNumber(log_level)) {
    cfg->log_level = xprintf("%g", log_level->valuedouble);
  }
  config_set_patterns_json(cJSON_GetObjectItemCaseSensitive(root, "include"), &cfg->has_include,
                           &cfg->include);
  config_set_patterns_json(cJSON_GetObjectItemCaseSensitive(root, "ignore"), &cfg->has_ignore,
                           &cfg->ignore);
  cJSON_Delete(root);
  return true;
}

static void config_set_patterns_edn(const edn_value *value, bool *has, strlist *list) {
  if (value == NULL) {
    return;
  }
  if (value->type == EDN_STRING) {
    *has = true;
    strlist_push_copy(list, value->text);
  } else if (value->type == EDN_VECTOR) {
    *has = true;
    for (size_t i = 0; i < value->num_items; i++) {
      if (value->items[i]->type == EDN_STRING) {
        strlist_push_copy(list, value->items[i]->text);
      }
    }
  }
}

static bool load_edn_config(const char *path, config *cfg) {
  size_t len = 0;
  char *text = fs_read_file(path, &len);
  if (text == NULL) {
    return false;
  }
  edn_value *root = edn_parse(text, len);
  free(text);
  if (root == NULL || root->type != EDN_MAP) {
    edn_free(root);
    return false;
  }

  cfg->loaded = true;
  const edn_value *log_level = edn_map_get(root, "log-level");
  if (log_level != NULL && (log_level->type == EDN_STRING || log_level->type == EDN_SYMBOL)) {
    cfg->log_level = xstrdup(log_level->text);
  }
  config_set_patterns_edn(edn_map_get(root, "include"), &cfg->has_include, &cfg->include);
  config_set_patterns_edn(edn_map_get(root, "ignore"), &cfg->has_ignore, &cfg->ignore);
  edn_free(root);
  return true;
}

static bool ends_with(const char *s, const char *suffix) {
  size_t s_len = strlen(s);
  size_t suffix_len = strlen(suffix);
  return suffix_len <= s_len && strcmp(s + s_len - suffix_len, suffix) == 0;
}

/* -----------------------------------------------------------------------------
 * Glob patterns
 *
 * The documented pattern features are `*` (within one path segment), `**`
 * (any number of segments), and `{a,b,c}` alternatives. `?` and `[...]`
 * character classes also work. Names that start with a dot are only matched by
 * a pattern segment that starts with a dot.
 */

static bool contains_glob_syntax(const char *pattern) {
  return strpbrk(pattern, "*?[]{}") != NULL;
}

/* Expands {a,b,c} alternatives into separate patterns (nested braces are
 * expanded too). A pattern without braces is returned as its own one-item list. */
static void expand_braces_into(const char *pattern, strlist *out) {
  const char *open = NULL;
  const char *close = NULL;
  int depth = 0;
  for (const char *p = pattern; *p; p++) {
    if (*p == '{') {
      if (depth == 0) {
        open = p;
      }
      depth++;
    } else if (*p == '}' && depth > 0) {
      depth--;
      if (depth == 0) {
        close = p;
        break;
      }
    }
  }
  if (open == NULL || close == NULL) {
    strlist_push_copy(out, pattern);
    return;
  }

  size_t prefix_len = (size_t)(open - pattern);
  const char *suffix = close + 1;

  /* split the alternatives on the commas at this nesting level */
  const char *alt_start = open + 1;
  int alt_depth = 0;
  for (const char *p = open + 1; p <= close; p++) {
    if (*p == '{') {
      alt_depth++;
    } else if (*p == '}' && p != close) {
      alt_depth--;
    }
    if (p == close || (*p == ',' && alt_depth == 0)) {
      char *expanded =
          xprintf("%.*s%.*s%s", (int)prefix_len, pattern, (int)(p - alt_start), alt_start, suffix);
      expand_braces_into(expanded, out);
      free(expanded);
      alt_start = p + 1;
    }
  }
}

static strlist expand_braces(const char *pattern) {
  strlist out = {NULL, 0, 0};
  expand_braces_into(pattern, &out);
  return out;
}

/* matches a [...] character class at `pat` (which points at the '[') against
 * c; sets *pat_end to the character after the class. Returns -1 if the
 * brackets do not form a class. */
static int match_char_class(const char *pat, char c, const char **pat_end) {
  const char *p = pat + 1;
  bool negate = false;
  if (*p == '!' || *p == '^') {
    negate = true;
    p++;
  }
  bool matched = false;
  bool first = true;
  while (*p && (*p != ']' || first)) {
    first = false;
    char lo = *p;
    if (lo == '\\' && p[1]) {
      lo = p[1];
      p++;
    }
    if (p[1] == '-' && p[2] && p[2] != ']') {
      char hi = p[2];
      if (c >= lo && c <= hi) {
        matched = true;
      }
      p += 3;
    } else {
      if (c == lo) {
        matched = true;
      }
      p++;
    }
  }
  if (*p != ']') {
    return -1;
  }
  *pat_end = p + 1;
  return (matched != negate) ? 1 : 0;
}

/* matches one path segment against a pattern segment (no separators in either) */
static bool match_segment(const char *pat, const char *name) {
  /* a leading dot must be matched literally */
  if (name[0] == '.' && pat[0] != '.') {
    return false;
  }
  while (*pat) {
    if (*pat == '*') {
      while (*pat == '*') {
        pat++;
      }
      if (*pat == '\0') {
        return true;
      }
      for (const char *n = name; *n; n++) {
        if (match_segment(pat, n)) {
          return true;
        }
      }
      return match_segment(pat, name);
    }
    if (*name == '\0') {
      return false;
    }
    if (*pat == '?') {
      pat++;
      name++;
      continue;
    }
    if (*pat == '[') {
      const char *pat_end = NULL;
      int result = match_char_class(pat, *name, &pat_end);
      if (result == 1) {
        pat = pat_end;
        name++;
        continue;
      }
      if (result == 0) {
        return false;
      }
      /* not a class: match the '[' literally */
    }
    char literal = *pat;
    if (literal == '\\' && pat[1]) {
      pat++;
      literal = *pat;
    }
    if (literal != *name) {
      return false;
    }
    pat++;
    name++;
  }
  return *name == '\0';
}

static bool segment_has_wildcard(const char *seg) {
  return strpbrk(seg, "*?[") != NULL;
}

/* splits a pattern on separators into segments (empty segments dropped) */
static strlist split_pattern(const char *pattern) {
  strlist segments = {NULL, 0, 0};
  const char *s = pattern;
  while (*s) {
    while (*s && (*s == '/' || is_path_sep(*s))) {
      s++;
    }
    const char *start = s;
    while (*s && !(*s == '/' || is_path_sep(*s))) {
      s++;
    }
    if (s > start) {
      strlist_push(&segments, xstrndup(start, (size_t)(s - start)));
    }
  }
  return segments;
}

/* Walks the filesystem from dir, matching pattern segments from index idx
 * onwards, and adds every regular file that matches. */
static void glob_walk(strlist *files, const char *dir, const strlist *segments, size_t idx,
                      int depth) {
  if (depth > MAX_DIRECTORY_DEPTH) {
    return;
  }
  if (idx == segments->size) {
    if (fs_is_file(dir)) {
      strlist_push_copy(files, dir);
    }
    return;
  }

  const char *seg = segments->items[idx];
  if (strcmp(seg, "**") == 0) {
    /* zero segments */
    glob_walk(files, dir, segments, idx + 1, depth);
    /* one or more segments: every non-hidden entry below dir */
    strlist names = fs_list_directory(dir);
    for (size_t i = 0; i < names.size; i++) {
      if (names.items[i][0] == '.') {
        continue;
      }
      char *child = path_join(dir, names.items[i]);
      fs_kind kind = fs_stat_kind(child);
      if (kind == FS_DIRECTORY) {
        glob_walk(files, child, segments, idx, depth + 1);
      } else if (kind == FS_FILE && idx + 1 == segments->size) {
        strlist_push_copy(files, child);
      }
      free(child);
    }
    strlist_free(&names);
    return;
  }

  if (segment_has_wildcard(seg)) {
    strlist names = fs_list_directory(dir);
    for (size_t i = 0; i < names.size; i++) {
      if (match_segment(seg, names.items[i])) {
        char *child = path_join(dir, names.items[i]);
        glob_walk(files, child, segments, idx + 1, depth + 1);
        free(child);
      }
    }
    strlist_free(&names);
    return;
  }

  /* a literal segment: no need to list the directory */
  char *child = path_join(dir, seg);
  glob_walk(files, child, segments, idx + 1, depth + 1);
  free(child);
}

/* the regular files matched by a glob pattern, as absolute paths */
static strlist files_from_glob(const char *cwd, const char *pattern) {
  strlist files = {NULL, 0, 0};
  strlist patterns = expand_braces(pattern);
  for (size_t i = 0; i < patterns.size; i++) {
    const char *p = patterns.items[i];
    const char *root = cwd;
    char *root_copy = NULL;
    if (path_is_absolute(p)) {
      size_t root_len = path_root_len(p);
      root_copy = xstrndup(p, root_len);
      root = root_copy;
      p += root_len;
    }
    strlist segments = split_pattern(p);
    glob_walk(&files, root, &segments, 0, 0);
    strlist_free(&segments);
    free(root_copy);
  }
  strlist_free(&patterns);

  /* normalize so that paths compare equal to resolved literal paths */
  for (size_t i = 0; i < files.size; i++) {
    char *normalized = path_normalize_absolute(files.items[i]);
    free(files.items[i]);
    files.items[i] = normalized;
  }
  return files;
}

/* -----------------------------------------------------------------------------
 * File discovery (cli_file_discovery.mjs)
 */

typedef bool (*file_predicate)(const char *filename, const strlist *file_extensions);

static bool accept_any_file(const char *filename, const strlist *file_extensions) {
  (void)filename;
  (void)file_extensions;
  return true;
}

/* adds every regular file beneath directory that passes the predicate */
static void add_files_from_directory(strlist *files, const char *directory, file_predicate pred,
                                     const strlist *file_extensions, int depth) {
  if (depth > MAX_DIRECTORY_DEPTH) {
    return;
  }
  strlist names = fs_list_directory(directory);
  for (size_t i = 0; i < names.size; i++) {
    char *child = path_join(directory, names.items[i]);
    fs_kind kind = fs_stat_kind(child);
    if (kind == FS_DIRECTORY) {
      add_files_from_directory(files, child, pred, file_extensions, depth + 1);
    } else if (kind == FS_FILE && pred(child, file_extensions)) {
      strlist_push_copy(files, child);
    }
    free(child);
  }
  strlist_free(&names);
}

typedef enum missing_path_kind { MISSING_INCLUDE, MISSING_IGNORE } missing_path_kind;

static void warn_missing_path(cli *c, missing_path_kind kind, const char *filename) {
  buf_clear(&c->line);
  buf_styled2(&c->line, c->env->color_err, STYLE_BOLD, STYLE_YELLOW, "WARN");
  buf_appendf(&c->line, " Could not find a file or directory%s at \"%s\"",
              kind == MISSING_IGNORE ? " to ignore" : "", filename);
  flush_line_to_stderr(c);
}

/* a file or directory named directly on the command line */
static void add_direct_argument(cli *c, strlist *files, const char *direct_arg,
                                const strlist *file_extensions) {
  char *possible = path_resolve(c->env->cwd, direct_arg);
  if (fs_is_file(possible)) {
    /* a directly named file is explicit, so any extension is allowed */
    strlist_push(files, possible);
    return;
  }
  if (fs_is_directory(possible)) {
    add_files_from_directory(files, possible, has_allowed_extension, file_extensions, 0);
  } else {
    warn_missing_path(c, MISSING_INCLUDE, direct_arg);
  }
  free(possible);
}

static void add_include_pattern(cli *c, strlist *files, const char *pattern,
                                const strlist *file_extensions) {
  char *possible = path_resolve(c->env->cwd, pattern);
  if (fs_is_file(possible)) {
    if (has_allowed_extension(possible, file_extensions)) {
      strlist_push(files, possible);
      return;
    }
  } else if (fs_is_directory(possible)) {
    add_files_from_directory(files, possible, has_allowed_extension, file_extensions, 0);
  } else {
    strlist matched = files_from_glob(c->env->cwd, pattern);
    for (size_t i = 0; i < matched.size; i++) {
      if (has_allowed_extension(matched.items[i], file_extensions)) {
        strlist_push_copy(files, matched.items[i]);
      }
    }
    strlist_free(&matched);
  }
  free(possible);
}

static void add_ignore_pattern(cli *c, strlist *files, const char *pattern) {
  char *possible = path_resolve(c->env->cwd, pattern);
  if (fs_is_file(possible)) {
    strlist_push(files, possible);
    return;
  }
  if (fs_is_directory(possible)) {
    /* every file beneath an ignored directory is ignored, whatever its extension */
    add_files_from_directory(files, possible, accept_any_file, NULL, 0);
  } else {
    strlist matched = files_from_glob(c->env->cwd, pattern);
    for (size_t i = 0; i < matched.size; i++) {
      strlist_push_copy(files, matched.items[i]);
    }
    /* a glob that matches nothing is valid and silent; a literal path that
     * does not exist gets a warning */
    if (matched.size == 0 && !contains_glob_syntax(pattern)) {
      warn_missing_path(c, MISSING_IGNORE, pattern);
    }
    strlist_free(&matched);
  }
  free(possible);
}

/* the sorted, unique, absolute paths selected by the arguments and patterns */
static strlist discover_files(cli *c, const strlist *direct_args, const strlist *include_patterns,
                              const strlist *ignore_patterns, const strlist *file_extensions) {
  strlist include_files = {NULL, 0, 0};
  strlist ignore_files = {NULL, 0, 0};

  for (size_t i = 0; i < direct_args->size; i++) {
    add_direct_argument(c, &include_files, direct_args->items[i], file_extensions);
  }
  for (size_t i = 0; i < include_patterns->size; i++) {
    add_include_pattern(c, &include_files, include_patterns->items[i], file_extensions);
  }
  for (size_t i = 0; i < ignore_patterns->size; i++) {
    add_ignore_pattern(c, &ignore_files, ignore_patterns->items[i]);
  }

  strlist_sort_unique(&include_files);
  strlist_sort_unique(&ignore_files);

  strlist discovered = {NULL, 0, 0};
  for (size_t i = 0; i < include_files.size; i++) {
    if (!strlist_sorted_contains(&ignore_files, include_files.items[i])) {
      strlist_push_copy(&discovered, include_files.items[i]);
    }
  }
  strlist_free(&include_files);
  strlist_free(&ignore_files);
  return discovered;
}

/* -----------------------------------------------------------------------------
 * Command line arguments
 */

typedef struct args {
  const char *command; /* NULL when none was given */
  strlist direct_args; /* positional arguments after the command */
  bool include_passed;
  strlist include;
  bool ignore_passed;
  strlist ignore;
  const char *config;
  const char *log_level;
  const char *file_ext;
  const char *output;
  bool help;
  bool version;
  char *error; /* a usage error, or NULL */
} args;

static void args_free(args *a) {
  strlist_free(&a->direct_args);
  strlist_free(&a->include);
  strlist_free(&a->ignore);
  free(a->error);
}

/* does arg name one of the given option spellings? */
static bool is_option(const char *arg, const char *long_name, const char *alias) {
  return strcmp(arg, long_name) == 0 || (alias != NULL && strcmp(arg, alias) == 0);
}

static args parse_args(int argc, char **argv) {
  args a;
  memset(&a, 0, sizeof(a));

  bool only_positionals = false;
  for (int i = 1; i < argc && a.error == NULL; i++) {
    const char *arg = argv[i];

    if (only_positionals || arg[0] != '-' || strcmp(arg, "-") == 0) {
      if (a.command == NULL) {
        a.command = arg;
      } else {
        strlist_push_copy(&a.direct_args, arg);
      }
      continue;
    }
    if (strcmp(arg, "--") == 0) {
      only_positionals = true;
      continue;
    }

    /* --name=value */
    char *name_copy = NULL;
    const char *value = NULL;
    const char *name = arg;
    const char *eq = strchr(arg, '=');
    if (eq != NULL && arg[1] == '-') {
      name_copy = xstrndup(arg, (size_t)(eq - arg));
      name = name_copy;
      value = eq + 1;
    }

    if (is_option(name, "--help", "-h")) {
      a.help = true;
    } else if (is_option(name, "--version", "-v")) {
      a.version = true;
    } else {
      /* the remaining options all take a value */
      if (value == NULL) {
        if (i + 1 < argc) {
          i++;
          value = argv[i];
        } else {
          a.error = xprintf("The option %s requires a value.", name);
        }
      }
      if (a.error == NULL) {
        if (is_option(name, "--config", "-c")) {
          a.config = value;
        } else if (is_option(name, "--include", "-in")) {
          a.include_passed = true;
          strlist_push_copy(&a.include, value);
        } else if (is_option(name, "--ignore", "-ig")) {
          a.ignore_passed = true;
          strlist_push_copy(&a.ignore, value);
        } else if (is_option(name, "--log-level", "-l")) {
          a.log_level = value;
        } else if (is_option(name, "--file-ext", NULL)) {
          a.file_ext = value;
        } else if (is_option(name, "--output", NULL)) {
          a.output = value;
        } else {
          a.error = xprintf("Unknown option: %s", name);
        }
      }
    }
    free(name_copy);
  }
  return a;
}

static const char *HELP_TEXT =
    "standard-clj <cmd> [args]\n"
    "\n"
    "Commands:\n"
    "  standard-clj check  Checks if files are formatted according to Standard Clojure Style. "
    "This\n"
    "                      command does not modify files. Returns exit code 0 if all files are "
    "formatted,\n"
    "                      1 otherwise.\n"
    "  standard-clj fix    Formats files according to Standard Clojure Style. This command will "
    "modify\n"
    "                      your files on disk. Returns exit code 0 if all files are formatted, 1\n"
    "                      otherwise.\n"
    "  standard-clj list   Prints a list of files that will be used by the \"check\" or \"fix\" "
    "commands.\n"
    "                      Useful for debugging your .standard-clj.edn file or glob patterns.\n"
    "\n"
    "Options:\n"
    "  -c, --config     Path to a .standard-clj.json or .standard-clj.edn config file\n"
    "  -in, --include   A file, directory, or glob pattern to include (may be repeated)\n"
    "  -ig, --ignore    A file, directory, or glob pattern to ignore (may be repeated)\n"
    "  -l, --log-level  Set the logging level: \"everything\", \"ignore-already-formatted\", "
    "\"quiet\"\n"
    "      --file-ext   Comma-separated file extensions to format (default: "
    "clj,cljs,cljc,jank,edn)\n"
    "      --output     Output format for \"list\": text, json, json-pretty, edn, edn-pretty\n"
    "  -v, --version    Show version number\n"
    "  -h, --help       Show help\n"
    "\n"
    "Examples:\n"
    "  standard-clj list src/   List files that will be formatted in the src/ directory "
    "(recursive)\n"
    "  standard-clj check src/  Check if files are already formatted in src/\n"
    "  standard-clj fix src/    Format all files in src/\n"
    "  standard-clj fix -       Reads from stdin, prints a formatted file to stdout\n"
    "\n"
    "For more information, see the README at https://github.com/oakmac/standard-clojure-style-js\n";

/* -----------------------------------------------------------------------------
 * Commands
 */

/* the options that decide which files a command works on */
typedef struct file_selection {
  strlist direct_args;
  strlist include;
  strlist ignore;
  strlist file_extensions;
} file_selection;

static void file_selection_free(file_selection *sel) {
  strlist_free(&sel->direct_args);
  strlist_free(&sel->include);
  strlist_free(&sel->ignore);
  strlist_free(&sel->file_extensions);
}

static void strlist_copy_into(strlist *dst, const strlist *src) {
  for (size_t i = 0; i < src->size; i++) {
    strlist_push_copy(dst, src->items[i]);
  }
}

/* Combines the command line and the config file: command line file selection
 * replaces the configured include patterns, and a command line --ignore
 * replaces the configured ignore patterns. */
static file_selection build_file_selection(const args *a, const config *cfg) {
  file_selection sel;
  memset(&sel, 0, sizeof(sel));
  strlist_copy_into(&sel.direct_args, &a->direct_args);

  bool any_cli_file_selection = a->direct_args.size > 0 || a->include_passed;
  if (a->include_passed) {
    strlist_copy_into(&sel.include, &a->include);
  } else if (!any_cli_file_selection && cfg->loaded && cfg->has_include) {
    strlist_copy_into(&sel.include, &cfg->include);
  }

  if (a->ignore_passed) {
    strlist_copy_into(&sel.ignore, &a->ignore);
  } else if (cfg->loaded && cfg->has_ignore) {
    strlist_copy_into(&sel.ignore, &cfg->ignore);
  }

  if (a->file_ext != NULL && a->file_ext[0] != '\0') {
    sel.file_extensions = convert_file_ext(a->file_ext);
  } else {
    for (size_t i = 0; i < sizeof(DEFAULT_FILE_EXTENSIONS) / sizeof(DEFAULT_FILE_EXTENSIONS[0]);
         i++) {
      strlist_push_copy(&sel.file_extensions, DEFAULT_FILE_EXTENSIONS[i]);
    }
  }
  return sel;
}

static void print_program_info(cli *c, const char *command) {
  buf_clear(&c->line);
  char *title = xprintf("standard-clj %s", command);
  buf_styled(&c->line, c->env->color_out, STYLE_BOLD, title);
  free(title);
  buf_append(&c->line, " ");
  buf_styled(&c->line, c->env->color_out, STYLE_DIM, c->env->version);
  flush_line_to_stdout(c);
  print_to_stdout(c, "");
}

typedef struct file_results {
  size_t num_already_formatted;
  size_t num_formatted; /* fix: written; check: would be written */
  size_t num_errors;
} file_results;

/* reads, formats, and classifies one file; fills cls and returns false if the
 * file could not be read */
static bool format_one_file(cli *c, const char *filename, classification *cls,
                            double *duration_ms) {
  double start = c->env->now_ms();
  size_t len = 0;
  char *text = fs_read_file(filename, &len);
  if (text == NULL) {
    /* this message intentionally has no color or timing, as in the JavaScript CLI */
    char *msg = xprintf("Unable to read file: %s", filename);
    print_to_stderr(c, msg);
    free(msg);
    c->at_least_one_file_printed = true;
    return false;
  }
  scs_format_result result = scs_format(text, len);
  *cls = classify_format_result(text, len, &result);
  scs_format_result_free(&result);
  free(text);
  *duration_ms = c->env->now_ms() - start;
  return true;
}

static void check_file(cli *c, file_results *results, const char *filename) {
  classification cls;
  double duration_ms = 0;
  if (!format_one_file(c, filename, &cls, &duration_ms)) {
    results->num_errors++;
    return;
  }
  char *rel = relative_filename(filename, c->env->cwd);

  if (cls.action == ACTION_ALREADY_FORMATTED) {
    results->num_already_formatted++;
    if (c->log_level != LOG_IGNORE_ALREADY_FORMATTED) {
      buf_clear(&c->line);
      buf_styled(&c->line, c->env->color_out, STYLE_GREEN, "\xE2\x9C\x93"); /* check mark */
      buf_append(&c->line, " ");
      buf_styled(&c->line, c->env->color_out, STYLE_BOLD, rel);
      buf_append(&c->line, " ");
      append_duration(&c->line, c->env->color_out, duration_ms);
      flush_line_to_stdout(c);
      c->at_least_one_file_printed = true;
    }
  } else if (cls.action == ACTION_FORMATTED) {
    results->num_formatted++;
    buf_clear(&c->line);
    buf_styled(&c->line, c->env->color_err, STYLE_RED, "\xE2\x9C\x97"); /* ballot x */
    buf_append(&c->line, " ");
    buf_styled(&c->line, c->env->color_err, STYLE_BOLD, rel);
    buf_append(&c->line, " ");
    append_duration(&c->line, c->env->color_err, duration_ms);
    flush_line_to_stderr(c);
    c->at_least_one_file_printed = true;
  } else {
    results->num_errors++;
    char *msg = xprintf("Failed to format file %s: %s", filename, cls.error_message);
    print_to_stderr(c, msg);
    free(msg);
    c->at_least_one_file_printed = true;
  }

  free(rel);
  classification_free(&cls);
}

static void fix_file(cli *c, file_results *results, const char *filename) {
  classification cls;
  double duration_ms = 0;
  if (!format_one_file(c, filename, &cls, &duration_ms)) {
    results->num_errors++;
    return;
  }
  char *rel = relative_filename(filename, c->env->cwd);

  if (cls.action == ACTION_ERROR) {
    results->num_errors++;
    buf_clear(&c->line);
    buf_styled(&c->line, c->env->color_err, STYLE_RED, "E");
    buf_append(&c->line, " ");
    buf_styled2(&c->line, c->env->color_err, STYLE_BOLD, STYLE_RED, rel);
    buf_appendf(&c->line, " - %s ", cls.error_message);
    append_duration(&c->line, c->env->color_err, duration_ms);
    flush_line_to_stderr(c);
    c->at_least_one_file_printed = true;
  } else if (cls.action == ACTION_FORMATTED) {
    if (fs_write_file(filename, cls.output_text, cls.output_len)) {
      results->num_formatted++;
      buf_clear(&c->line);
      buf_styled(&c->line, c->env->color_out, STYLE_GREEN, "F");
      buf_append(&c->line, " ");
      buf_styled(&c->line, c->env->color_out, STYLE_BOLD, rel);
      buf_append(&c->line, " ");
      append_duration(&c->line, c->env->color_out, duration_ms);
      flush_line_to_stdout(c);
    } else {
      results->num_errors++;
      buf_clear(&c->line);
      buf_styled(&c->line, c->env->color_err, STYLE_RED, "E");
      buf_append(&c->line, " ");
      buf_styled2(&c->line, c->env->color_err, STYLE_BOLD, STYLE_RED, rel);
      buf_append(&c->line, " - Unable to write file ");
      append_duration(&c->line, c->env->color_err, duration_ms);
      flush_line_to_stderr(c);
    }
    c->at_least_one_file_printed = true;
  } else {
    results->num_already_formatted++;
    if (c->log_level != LOG_IGNORE_ALREADY_FORMATTED) {
      buf_clear(&c->line);
      buf_styled(&c->line, c->env->color_out, STYLE_GREEN, "\xE2\x9C\x93");
      buf_append(&c->line, " ");
      buf_styled(&c->line, c->env->color_out, STYLE_BOLD, rel);
      buf_append(&c->line, " ");
      append_duration(&c->line, c->env->color_out, duration_ms);
      flush_line_to_stdout(c);
      c->at_least_one_file_printed = true;
    }
  }

  free(rel);
  classification_free(&cls);
}

/* "1 file formatted with Standard Clojure Style 👍 [ms]" or "All N files ..." */
static void print_all_formatted_summary(cli *c, size_t num_files, double script_duration_ms) {
  buf_clear(&c->line);
  char *msg = num_files == 1
                  ? xstrdup("1 file formatted with Standard Clojure Style \xF0\x9F\x91\x8D")
                  : xprintf("All %zu files formatted with Standard Clojure Style \xF0\x9F\x91\x8D",
                            num_files);
  buf_styled(&c->line, c->env->color_out, STYLE_GREEN, msg);
  free(msg);
  buf_append(&c->line, " ");
  append_duration(&c->line, c->env->color_out, script_duration_ms);
  flush_line_to_stdout(c);
}

static void print_colored_line(cli *c, style st, const char *text) {
  buf_clear(&c->line);
  buf_styled(&c->line, c->env->color_out, st, text);
  flush_line_to_stdout(c);
}

static int run_check(cli *c, const strlist *files) {
  file_results results = {0, 0, 0};
  for (size_t i = 0; i < files->size; i++) {
    check_file(c, &results, files->items[i]);
  }

  size_t total = results.num_already_formatted + results.num_formatted + results.num_errors;
  bool all_formatted = results.num_already_formatted == total && total > 0;
  double script_duration_ms = c->env->now_ms() - c->script_start_time;

  if (c->at_least_one_file_printed) {
    print_to_stdout(c, "");
  }

  if (all_formatted) {
    print_all_formatted_summary(c, files->size, script_duration_ms);
    return 0;
  }

  char *msg = xprintf("%zu %s formatted with Standard Clojure Style", results.num_already_formatted,
                      file_str(results.num_already_formatted));
  print_colored_line(c, STYLE_GREEN, msg);
  free(msg);
  msg =
      xprintf("%zu %s require formatting", results.num_formatted, file_str(results.num_formatted));
  print_colored_line(c, STYLE_RED, msg);
  free(msg);
  buf_clear(&c->line);
  buf_appendf(&c->line, "Checked %zu %s. ", total, file_str(total));
  append_duration(&c->line, c->env->color_out, script_duration_ms);
  flush_line_to_stdout(c);
  return 1;
}

static int run_fix(cli *c, const strlist *files) {
  file_results results = {0, 0, 0};
  for (size_t i = 0; i < files->size; i++) {
    fix_file(c, &results, files->items[i]);
  }

  size_t total = results.num_already_formatted + results.num_formatted + results.num_errors;
  bool all_success = results.num_errors == 0 && total > 0;
  double script_duration_ms = c->env->now_ms() - c->script_start_time;

  if (c->at_least_one_file_printed) {
    print_to_stdout(c, "");
  }

  if (all_success) {
    print_all_formatted_summary(c, files->size, script_duration_ms);
    return 0;
  }

  char *msg = xprintf("%zu files formatted with Standard Clojure Style",
                      results.num_already_formatted + results.num_formatted);
  print_colored_line(c, STYLE_GREEN, msg);
  free(msg);
  msg = xprintf("%zu files with errors", results.num_errors);
  print_colored_line(c, STYLE_RED, msg);
  free(msg);
  buf_clear(&c->line);
  buf_appendf(&c->line, "Checked %zu files. ", files->size);
  append_duration(&c->line, c->env->color_out, script_duration_ms);
  flush_line_to_stdout(c);
  return 1;
}

/* "fix -": formats stdin to stdout */
static int run_fix_stdin(cli *c) {
  buf input = {NULL, 0, 0};
  char chunk[65536];
  size_t n;
  while ((n = fread(chunk, 1, sizeof(chunk), c->env->in)) > 0) {
    buf_append_n(&input, chunk, n);
  }

  if (input.len == 0) {
    buf_free(&input);
    print_to_stderr(c, "Nothing found on stdin. Please pipe some Clojure code to stdin when using "
                       "\"standard-clj fix -\"");
    return 1;
  }

  scs_format_result result = scs_format(input.data, input.len);
  buf_free(&input);

  int exit_code;
  if (result.status == SCS_STATUS_SUCCESS && result.out != NULL) {
    fwrite(result.out, 1, result.out_len, c->env->out);
    fputc('\n', c->env->out);
    exit_code = 0;
  } else if (result.status == SCS_STATUS_ERROR && result.reason != NULL) {
    char *msg = xprintf("Failed to format code: %s", result.reason);
    print_to_stderr(c, msg);
    free(msg);
    exit_code = 1;
  } else {
    print_to_stderr(c, "Failed to format your code due to unknown error with the format() "
                       "function. Please help the standard-clj project by opening an issue to "
                       "report this \xF0\x9F\x99\x8F");
    exit_code = 1;
  }
  scs_format_result_free(&result);
  return exit_code;
}

static int run_list(cli *c, const strlist *files, const char *output) {
  char *text = NULL;
  if (output != NULL && strcmp(output, "json") == 0) {
    text = encode_json_array(files);
  } else if (output != NULL && strcmp(output, "json-pretty") == 0) {
    text = encode_json_array_pretty(files);
  } else if (output != NULL && strcmp(output, "edn") == 0) {
    text = encode_edn_array(files);
  } else if (output != NULL && strcmp(output, "edn-pretty") == 0) {
    text = encode_edn_array_pretty(files);
  }

  if (text != NULL) {
    print_to_stdout(c, text);
    free(text);
  } else {
    for (size_t i = 0; i < files->size; i++) {
      print_to_stdout(c, files->items[i]);
    }
  }
  return 0;
}

/* Loads the config file named by --config, or else the default
 * .standard-clj.json / .standard-clj.edn in the working directory. Returns
 * false (after printing why) when a --config file cannot be loaded. */
static bool load_config(cli *c, const args *a, config *cfg) {
  memset(cfg, 0, sizeof(*cfg));

  if (a->config != NULL && a->config[0] != '\0') {
    bool is_json = ends_with(a->config, ".json");
    bool is_edn = ends_with(a->config, ".edn");
    char *path = path_resolve(c->env->cwd, a->config);
    bool ok = false;
    if (is_json) {
      ok = load_json_config(path, cfg);
    } else if (is_edn) {
      ok = load_edn_config(path, cfg);
    }
    free(path);
    if (!ok) {
      char *msg = xprintf("Unable to load config file: %s", a->config);
      print_to_stderr(c, msg);
      free(msg);
      if (is_json) {
        print_to_stderr(c, "Maybe the file is invalid JSON?");
      } else if (is_edn) {
        print_to_stderr(c, "Maybe the file is invalid EDN?");
      } else {
        print_to_stderr(c, "The filename does not end in .json or .edn. That is probably wrong.");
      }
      return false;
    }
    return true;
  }

  /* the default config files; when both exist and parse, the EDN file wins */
  char *json_path = path_join(c->env->cwd, ".standard-clj.json");
  char *edn_path = path_join(c->env->cwd, ".standard-clj.edn");
  config edn_cfg;
  memset(&edn_cfg, 0, sizeof(edn_cfg));
  load_json_config(json_path, cfg);
  if (load_edn_config(edn_path, &edn_cfg)) {
    config_free(cfg);
    *cfg = edn_cfg;
  }
  free(json_path);
  free(edn_path);
  return true;
}

int scs_cli_run(int argc, char **argv, const scs_cli_env *env) {
  cli c;
  memset(&c, 0, sizeof(c));
  c.env = env;
  c.log_level = LOG_EVERYTHING;
  c.script_start_time = env->now_ms();

  int exit_code = 0;
  args a = parse_args(argc, argv);
  config cfg;
  memset(&cfg, 0, sizeof(cfg));

  if (a.error != NULL) {
    fprintf(env->err, "%s\nRun \"standard-clj --help\" for usage.\n", a.error);
    exit_code = 1;
  } else if (a.help) {
    fputs(HELP_TEXT, env->out);
  } else if (a.version) {
    fprintf(env->out, "%s\n", env->version_verbose);
  } else if (a.command == NULL) {
    fputs(HELP_TEXT, env->err);
    fputs("\nNot enough non-option arguments: got 0, need at least 1\n", env->err);
    exit_code = 1;
  } else if (strcmp(a.command, "check") != 0 && strcmp(a.command, "fix") != 0 &&
             strcmp(a.command, "list") != 0) {
    fprintf(env->err, "Unknown command: %s\nRun \"standard-clj --help\" for usage.\n", a.command);
    exit_code = 1;
  } else if (!load_config(&c, &a, &cfg)) {
    exit_code = 1;
  } else {
    bool is_fix = strcmp(a.command, "fix") == 0;
    bool is_check = strcmp(a.command, "check") == 0;
    bool stdin_mode = is_fix && a.direct_args.size > 0 &&
                      strcmp(a.direct_args.items[a.direct_args.size - 1], "-") == 0;

    if (stdin_mode) {
      exit_code = run_fix_stdin(&c);
    } else {
      if (is_fix || is_check) {
        const char *level = a.log_level != NULL ? a.log_level : cfg.log_level;
        c.log_level = normalize_log_level(level);
        print_program_info(&c, a.command);
      }

      file_selection sel = build_file_selection(&a, &cfg);
      strlist files =
          discover_files(&c, &sel.direct_args, &sel.include, &sel.ignore, &sel.file_extensions);

      if (is_fix || is_check) {
        if (files.size == 0) {
          char *msg = xprintf("No files were passed to the \"%s\" command. Please pass a filename, "
                              "directory, or --include glob pattern.",
                              a.command);
          print_to_stderr(&c, msg);
          free(msg);
          exit_code = 1;
        } else if (is_check) {
          exit_code = run_check(&c, &files);
        } else {
          exit_code = run_fix(&c, &files);
        }
      } else {
        exit_code = run_list(&c, &files, a.output);
      }

      strlist_free(&files);
      file_selection_free(&sel);
    }
  }

  config_free(&cfg);
  args_free(&a);
  buf_free(&c.line);
  fflush(env->out);
  fflush(env->err);
  return exit_code;
}

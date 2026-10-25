/*
 * standard-clojure-style.h - public API for Standard Clojure Style in C
 * https://github.com/oakmac/standard-clojure-style-c
 *
 * Copyright (c) 2026, Chris Oakman
 * Released under the ISC license
 * https://github.com/oakmac/standard-clojure-style-c/blob/master/LICENSE.md
 *
 * Standard Clojure Style is a formatter for Clojure code. This library is a
 * port of the JavaScript implementation:
 * https://github.com/oakmac/standard-clojure-style-js
 *
 * Usage:
 *
 *   scs_format_result result = scs_format(source, source_len);
 *   if (result.status == SCS_STATUS_SUCCESS) {
 *     fwrite(result.out, 1, result.out_len, stdout);
 *   } else {
 *     fprintf(stderr, "error: %s\n", result.reason);
 *   }
 *   scs_format_result_free(&result);
 */

#ifndef STANDARD_CLOJURE_STYLE_H
#define STANDARD_CLOJURE_STYLE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The version of this library.
 * NOTE: version numbers track the version of Standard Clojure Style JavaScript
 * that this port implements. The port is not finished, hence 0.0.0. */
#define SCS_VERSION "0.0.0"

typedef enum scs_status { SCS_STATUS_SUCCESS = 0, SCS_STATUS_ERROR = 1 } scs_status;

/* The result of formatting a piece of Clojure code.
 *
 * Every string in this struct is owned by the result. Release them by passing
 * the result to scs_format_result_free() when you are done with it. */
typedef struct scs_format_result {
  scs_status status;

  /* true when the input contained a #_ :standard-clj/ignore-file marker.
   * In that case `out` is a copy of the input, unchanged. */
  bool file_was_ignored;

  /* The formatted code (only when status == SCS_STATUS_SUCCESS, otherwise NULL).
   * UTF-8, NUL-terminated. `out_len` is the length in bytes, not counting the
   * NUL terminator. */
  char *out;
  size_t out_len;

  /* A human readable description of what went wrong
   * (only when status == SCS_STATUS_ERROR, otherwise NULL).
   * May be NULL in the unlikely event that memory could not be allocated for
   * the message itself. */
  char *reason;
} scs_format_result;

/* Formats `input_len` bytes of Clojure code starting at `input` according to
 * Standard Clojure Style.
 *
 * - `input` must be UTF-8 encoded. It does not need to be NUL-terminated.
 * - CRLF line endings in the input are converted to LF in the output.
 * - `input` is never modified.
 * - This function has no global state and may be called from multiple threads
 *   at the same time.
 *
 * Always check `.status` on the returned result, and always release the result
 * with scs_format_result_free(), even when status is SCS_STATUS_ERROR. */
scs_format_result scs_format(const char *input, size_t input_len);

/* Releases the memory owned by a result and sets its string fields to NULL.
 * Passing NULL is allowed and does nothing. It is safe to call this more than
 * once on the same result. */
void scs_format_result_free(scs_format_result *result);

/* Returns the library version as a NUL-terminated string (see SCS_VERSION). */
const char *scs_version(void);

#ifdef __cplusplus
}
#endif

#endif /* STANDARD_CLOJURE_STYLE_H */

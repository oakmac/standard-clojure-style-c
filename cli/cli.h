/*
 * cli.h - the standard-clj command line interface
 * https://github.com/oakmac/standard-clojure-style-c
 *
 * Copyright (c) 2026, Chris Oakman
 * Released under the ISC license
 * https://github.com/oakmac/standard-clojure-style-c/blob/master/LICENSE.md
 *
 * The CLI is a function that takes argv and an environment describing where
 * to read and write, so that the test suite can run it in-process with
 * captured output. main.c builds the environment from the real process.
 */

#ifndef STANDARD_CLJ_CLI_H
#define STANDARD_CLJ_CLI_H

#include <stdbool.h>
#include <stdio.h>

typedef struct scs_cli_env {
  FILE *out;                   /* standard output */
  FILE *err;                   /* standard error */
  FILE *in;                    /* standard input (for "fix -") */
  const char *cwd;             /* absolute path of the working directory */
  bool color_out;              /* emit ANSI colors on `out` */
  bool color_err;              /* emit ANSI colors on `err` */
  double (*now_ms)(void);      /* a monotonic clock, in milliseconds */
  const char *version;         /* shown in the header of check / fix output */
  const char *version_verbose; /* shown by --version */
} scs_cli_env;

/* Fills env from the current process: the standard streams, the working
 * directory, color support of each stream, a monotonic clock, and the version.
 * Returns false (with a message on stderr) if the working directory could not
 * be determined. */
bool scs_cli_default_env(scs_cli_env *env);

/* Runs the CLI. argv[0] is the program name. Returns the process exit code. */
int scs_cli_run(int argc, char **argv, const scs_cli_env *env);

#endif /* STANDARD_CLJ_CLI_H */

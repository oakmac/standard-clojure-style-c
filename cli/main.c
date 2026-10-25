/*
 * main.c - entry point of the standard-clj binary
 *
 * Copyright (c) 2026, Chris Oakman
 * Released under the ISC license
 * https://github.com/oakmac/standard-clojure-style-c/blob/master/LICENSE.md
 */

#include "cli.h"

int main(int argc, char **argv) {
  scs_cli_env env;
  if (!scs_cli_default_env(&env)) {
    return 1;
  }
  return scs_cli_run(argc, argv, &env);
}

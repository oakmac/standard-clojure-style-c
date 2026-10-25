# Changelog

All notable changes to this project will be documented in this file.

> NOTE: releases of this library will skip version numbers in order to stay in sync with
> [Standard Clojure Style JavaScript](https://github.com/oakmac/standard-clojure-style-js)

## [Unreleased]

- project skeleton: build files, public header, arena allocator, error handling, test runner
- UTF-8 string helpers, node and string lists, stack operations, whitespace and comment helpers
- parser: combinators, hand-written text matchers, grammar, nesting depth limit; passes all parser test cases
- ns form parsing into typed structs, with a stable sort; passes all parse_ns test cases (the test suite vendors cJSON to compare results)

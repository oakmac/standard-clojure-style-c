# CMake build for Standard Clojure Style in C
#
# This exists mainly for Windows / MSVC and for IDE users. On Linux and macOS
# the Makefile does the same job with less ceremony.
#
#   cmake -B build
#   cmake --build build --config Release
#   ctest --test-dir build -C Release --output-on-failure

cmake_minimum_required(VERSION 3.15)

project(standard_clojure_style
  VERSION 0.0.0
  DESCRIPTION "Standard Clojure Style - a formatter for Clojure code"
  LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(SCS_BUILD_TESTS "Build the test suite" ON)

# Strict warnings, and every warning is an error.
# NOTE: unused-function warnings are disabled TEMPORARILY while the port is in
# progress: helper functions are ported (and tested) before the code that calls
# them exists. Remove -Wno-unused-function / /wd4505 once the port is complete.
if(MSVC)
  set(SCS_WARNINGS /W4 /WX /wd4505)
else()
  set(SCS_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes
                   -Wmissing-prototypes -Wno-unused-function -Werror)
endif()

add_library(standard_clojure_style STATIC standard-clojure-style.c)
target_include_directories(standard_clojure_style PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(standard_clojure_style PRIVATE ${SCS_WARNINGS})

if(SCS_BUILD_TESTS)
  enable_testing()
  # NOTE: the test binary compiles standard-clojure-style.c via #include, so it
  # is built from test/test_main.c alone (see the comment at the top of that file)
  add_executable(scs_test test/test_main.c)
  target_include_directories(scs_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(scs_test PRIVATE ${SCS_WARNINGS})
  # the tests read test_cases/*.json relative to the repository root
  add_test(NAME scs_test COMMAND scs_test WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

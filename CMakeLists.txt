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
if(MSVC)
  set(SCS_WARNINGS /W4 /WX)
  # C4204 is MSVC's warning that initializing a struct with values that are not
  # constants is a "nonstandard extension"; it is standard C99, which we use.
  list(APPEND SCS_WARNINGS /wd4204)
  # fopen(), getenv() and friends are not "unsafe" in the way we use them
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  set(SCS_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes
                   -Wmissing-prototypes -Werror)
endif()

add_library(standard_clojure_style STATIC standard-clojure-style.c)
target_include_directories(standard_clojure_style PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(standard_clojure_style PRIVATE ${SCS_WARNINGS})

# cJSON (used by the CLI and the tests) is vendored, so it is compiled without our warning flags
add_library(scs_cjson STATIC vendor/cjson/cJSON.c)
if(UNIX)
  target_link_libraries(scs_cjson PUBLIC m)
endif()

# the standard-clj command line program
add_executable(standard-clj cli/cli.c cli/main.c)
target_compile_options(standard-clj PRIVATE ${SCS_WARNINGS})
target_link_libraries(standard-clj PRIVATE standard_clojure_style scs_cjson)
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
                  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                  OUTPUT_VARIABLE SCS_GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET)
  if(SCS_GIT_HASH)
    target_compile_definitions(standard-clj PRIVATE SCS_GIT_HASH="${SCS_GIT_HASH}")
  endif()
endif()
install(TARGETS standard-clj RUNTIME DESTINATION bin)

if(SCS_BUILD_TESTS)
  enable_testing()
  # NOTE: the library test binary compiles standard-clojure-style.c via #include,
  # so it is built from test/test_main.c (see the comment at the top of that
  # file). The CLI test binary does the same with cli/cli.c.
  add_executable(scs_test test/test_main.c)
  target_include_directories(scs_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(scs_test PRIVATE ${SCS_WARNINGS})
  target_link_libraries(scs_test PRIVATE scs_cjson)
  # the tests read test_cases/*.json relative to the repository root
  add_test(NAME scs_test COMMAND scs_test WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

  add_executable(scs_cli_test test/test_cli.c standard-clojure-style.c)
  target_include_directories(scs_cli_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(scs_cli_test PRIVATE ${SCS_WARNINGS})
  target_link_libraries(scs_cli_test PRIVATE scs_cjson)
  add_test(NAME scs_cli_test COMMAND scs_cli_test WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

  # run the built binary itself, so CI exercises it on every platform
  add_test(NAME standard_clj_version COMMAND standard-clj --version)
  add_test(NAME standard_clj_help COMMAND standard-clj --help)
endif()

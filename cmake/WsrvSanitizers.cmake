# SPDX-License-Identifier: Apache-2.0
#
# Sanitizer selection (docs/TESTING_STRATEGY.md section 7). Flags are applied GLOBALLY, before any
# target exists, because instrumentation must be consistent: ThreadSanitizer in particular misses races
# if some objects (e.g. GoogleTest, the standard library headers) are not instrumented.
#
#   asan-ubsan : AddressSanitizer + UndefinedBehaviorSanitizer (memory errors, UB). GCC or Clang.
#   tsan       : ThreadSanitizer (data races). Cannot be combined with asan.
#   msan       : MemorySanitizer (uninitialised reads). Clang only, needs an instrumented libc++.
include_guard(GLOBAL)

set(WSRV_SANITIZER "none" CACHE STRING "Sanitizer: none | asan-ubsan | tsan | msan")
set_property(CACHE WSRV_SANITIZER PROPERTY STRINGS none asan-ubsan tsan msan)

set(_wsrv_san_flags "")
if(WSRV_SANITIZER STREQUAL "asan-ubsan")
    set(_wsrv_san_flags -fsanitize=address,undefined)
elseif(WSRV_SANITIZER STREQUAL "tsan")
    set(_wsrv_san_flags -fsanitize=thread)
elseif(WSRV_SANITIZER STREQUAL "msan")
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR "WSRV_SANITIZER=msan requires Clang.")
    endif()
    message(WARNING "msan needs an MSan-instrumented libc++ to avoid false positives (nightly job only).")
    set(_wsrv_san_flags -fsanitize=memory -fsanitize-memory-track-origins)
elseif(NOT WSRV_SANITIZER STREQUAL "none")
    message(FATAL_ERROR "Unknown WSRV_SANITIZER='${WSRV_SANITIZER}' (none|asan-ubsan|tsan|msan)")
endif()

if(_wsrv_san_flags)
    # -fno-sanitize-recover: the first finding aborts the process, so a test cannot "pass" past a bug.
    add_compile_options(${_wsrv_san_flags} -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_link_options(${_wsrv_san_flags})
    message(STATUS "Sanitizer: ${WSRV_SANITIZER}")
endif()

# Coverage (gcov/llvm-cov). Optimisation off so line counts match the source.
option(WSRV_COVERAGE "Instrument for code coverage" OFF)
if(WSRV_COVERAGE)
    add_compile_options(--coverage -O0 -fno-inline)
    add_link_options(--coverage)
    message(STATUS "Coverage instrumentation enabled")
endif()

# Fuzzing (libFuzzer, Clang only). Coverage instrumentation must be present in the LIBRARIES under test
# too (not just the harness), otherwise the fuzzer cannot see which code paths an input reaches.
if(WSRV_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR "WSRV_BUILD_FUZZERS requires Clang (libFuzzer); use the `fuzz` preset.")
    endif()
    add_compile_options(-fsanitize=fuzzer-no-link)
    message(STATUS "libFuzzer instrumentation enabled")
endif()

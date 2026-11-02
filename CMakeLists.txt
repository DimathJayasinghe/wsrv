# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.25)

# Refuse in-source builds BEFORE project(): once project() runs, CMake has already started
# writing files into the directory. Build outputs belong in build/<preset>.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR
        "In-source builds are not supported. Use a preset: cmake --preset dev "
        "(see docs/BUILD_AND_TOOLING.md). Delete CMakeCache.txt and CMakeFiles/ if they were created.")
endif()

project(wsrv
    VERSION 0.1.0
    DESCRIPTION "From-scratch C++23 WebSocket (RFC 6455) server core"
    LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

# ---- Options (see docs/BUILD_AND_TOOLING.md section 3) ----
option(WSRV_BUILD_TESTS       "Build unit/integration tests"        ${PROJECT_IS_TOP_LEVEL})
option(WSRV_BUILD_EXAMPLES    "Build examples"                      OFF)
option(WSRV_BUILD_FUZZERS     "Build libFuzzer targets (Clang)"     OFF)
option(WSRV_BUILD_BENCH       "Build benchmarks"                    OFF)
option(WSRV_ENABLE_TLS        "Build the OpenSSL TLS transport"     OFF)
option(WSRV_ENABLE_DEFLATE    "Build permessage-deflate (zlib)"     OFF)
option(WSRV_WARNINGS_AS_ERRORS "Treat compiler warnings as errors"  ON)
option(WSRV_CLANG_TIDY        "Run clang-tidy on production code (src/)"  OFF)
option(WSRV_CLANG_TIDY_TESTS  "Also run clang-tidy on tests (slow)"       OFF)
if(WSRV_CLANG_TIDY OR WSRV_CLANG_TIDY_TESTS)
    find_program(WSRV_CLANG_TIDY_EXE NAMES clang-tidy-18 clang-tidy REQUIRED)
endif()

# ---- Language level: C++23, no compiler extensions (ADR-0001) ----
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # consumed by clang-tidy / IDEs

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

include(WsrvToolchainChecks)   # fails fast on unsupported compilers / standard libraries
include(WsrvSanitizers)         # global flags: must precede every target, incl. FetchContent deps
include(WsrvWarnings)
include(WsrvHardening)
include(WsrvModule)

# ---- Modules, in dependency order (allowed graph: docs/ARCHITECTURE.md section 2.1) ----
add_subdirectory(src/base)
add_subdirectory(src/crypto)
add_subdirectory(src/http)
add_subdirectory(src/protocol)
add_subdirectory(src/ipc)
add_subdirectory(src/extensions)
add_subdirectory(src/net)
add_subdirectory(src/server)
add_subdirectory(src/capi)

if(PROJECT_IS_TOP_LEVEL)
    include(WsrvChecks)         # format-check, check-banned, check-includes, header-check
endif()

if(WSRV_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(WSRV_BUILD_TESTS)
    enable_testing()
    include(WsrvTests)
    add_subdirectory(tests)
endif()

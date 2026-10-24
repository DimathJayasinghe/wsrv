# SPDX-License-Identifier: Apache-2.0
#
# Fail at configure time, in one readable sentence, when the toolchain cannot build this project.
# The alternative is hundreds of lines of template errors the first time <expected> is included.
include_guard(GLOBAL)

set(_wsrv_min_gcc 13)
set(_wsrv_min_clang 17)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS _wsrv_min_gcc)
        message(FATAL_ERROR
            "GCC ${CMAKE_CXX_COMPILER_VERSION} is too old; wsrv needs GCC >= ${_wsrv_min_gcc}. "
            "Use the Docker dev environment (make shell) or install a newer GCC.")
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS _wsrv_min_clang)
        message(FATAL_ERROR
            "Clang ${CMAKE_CXX_COMPILER_VERSION} is too old; wsrv needs Clang >= ${_wsrv_min_clang}.")
    endif()
    # Clang <= 18 combined with libstdc++ has no std::expected (BUILD_AND_TOOLING 1.1), so Clang
    # builds use libc++. CMake puts CMAKE_CXX_FLAGS on both compile and link lines.
    option(WSRV_USE_LIBCXX "Use libc++ with Clang" ON)
    if(WSRV_USE_LIBCXX)
        string(APPEND CMAKE_CXX_FLAGS " -stdlib=libc++")
    endif()
else()
    message(WARNING "Compiler '${CMAKE_CXX_COMPILER_ID}' is untested; supported: GCC >= 13, Clang >= 17.")
endif()

# Prove the C++23 library features we depend on actually work with this compiler + stdlib pair.
unset(WSRV_HAS_STD_EXPECTED CACHE)
try_compile(WSRV_HAS_STD_EXPECTED
    "${CMAKE_BINARY_DIR}/toolchain_check"
    SOURCES "${PROJECT_SOURCE_DIR}/cmake/checks/std_expected.cpp"
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
if(NOT WSRV_HAS_STD_EXPECTED)
    message(FATAL_ERROR
        "This toolchain lacks std::expected/std::span in C++23 mode. "
        "With Clang use libc++ (-DWSRV_USE_LIBCXX=ON, the default); "
        "with GCC use version >= 13. See docs/BUILD_AND_TOOLING.md section 1.1.")
endif()

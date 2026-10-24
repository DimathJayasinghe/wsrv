# SPDX-License-Identifier: Apache-2.0
#
# One warning policy for all first-party code (docs/CODING_STANDARDS.md section 1).
# Third-party code (GoogleTest) is deliberately NOT given these flags.
include_guard(GLOBAL)

set(WSRV_WARNING_FLAGS
    -Wall -Wextra -Wpedantic
    -Wconversion -Wsign-conversion      # silent narrowing of wire-derived lengths is a classic bug
    -Wshadow
    -Wnon-virtual-dtor -Woverloaded-virtual
    -Wold-style-cast -Wcast-align
    -Wnull-dereference
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough)

function(wsrv_apply_warnings target)
    get_target_property(_type ${target} TYPE)
    if(_type STREQUAL "INTERFACE_LIBRARY")
        return()
    endif()
    target_compile_options(${target} PRIVATE ${WSRV_WARNING_FLAGS})
    # clang-tidy: production code with WSRV_CLANG_TIDY; test code only with WSRV_CLANG_TIDY_TESTS
    # (each GoogleTest file costs ~10 s of tidy time, which CI should not pay on every run).
    set(_tidy "${WSRV_CLANG_TIDY}")
    if("TEST" IN_LIST ARGN)
        set(_tidy "${WSRV_CLANG_TIDY_TESTS}")
    endif()
    if(_tidy)
        set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${WSRV_CLANG_TIDY_EXE};--quiet")
    endif()
    if(WSRV_WARNINGS_AS_ERRORS)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
endfunction()

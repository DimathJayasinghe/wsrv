# SPDX-License-Identifier: Apache-2.0
#
# Test support: pinned GoogleTest + wsrv_add_test().
# Dependency policy (docs/BUILD_AND_TOOLING.md section 7): exact version AND SHA-256, so a changed or
# tampered download is rejected instead of silently built.
include_guard(GLOBAL)

include(FetchContent)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
FetchContent_Declare(googletest
    URL https://github.com/google/googletest/releases/download/v1.15.2/googletest-1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(googletest)
include(GoogleTest)

# wsrv_add_test(NAME <n> SOURCES <files...> LINK <targets...>)
function(wsrv_add_test)
    cmake_parse_arguments(ARG "" "NAME" "SOURCES;LINK" ${ARGN})
    add_executable(${ARG_NAME} ${ARG_SOURCES})
    target_link_libraries(${ARG_NAME} PRIVATE ${ARG_LINK} GTest::gtest_main)
    set_target_properties(${ARG_NAME} PROPERTIES CXX_EXTENSIONS OFF)
    target_include_directories(${ARG_NAME} PRIVATE "${PROJECT_SOURCE_DIR}/tests")   # support/fakes.hpp
    wsrv_apply_warnings(${ARG_NAME} TEST)
    gtest_discover_tests(${ARG_NAME} DISCOVERY_MODE PRE_TEST)
endfunction()

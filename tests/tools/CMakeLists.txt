# SPDX-License-Identifier: Apache-2.0
# Tests for the repository's own quality tooling (tools/).
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(NAME tools_check_includes
    COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/test_check_includes.py)
add_test(NAME tools_banned_functions
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_banned_functions.sh ${PROJECT_SOURCE_DIR})
set_tests_properties(tools_check_includes tools_banned_functions PROPERTIES LABELS tools)

# Hardening must really be applied in optimised builds, not merely configured (review finding I5).
if(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
    add_test(NAME hardening_compile_flags
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/check_release_flags.py
                ${CMAKE_BINARY_DIR}/compile_commands.json)
    add_test(NAME hardening_binary
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/check_binary_hardening.sh $<TARGET_FILE:base_version_test>)
    set_tests_properties(hardening_compile_flags hardening_binary PROPERTIES LABELS hardening)
endif()

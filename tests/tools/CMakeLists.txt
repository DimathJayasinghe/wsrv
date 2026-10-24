# SPDX-License-Identifier: Apache-2.0
# Tests for the repository's own quality tooling (tools/).
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(NAME tools_check_includes
    COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/test_check_includes.py)
add_test(NAME tools_banned_functions
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_banned_functions.sh ${PROJECT_SOURCE_DIR})
set_tests_properties(tools_check_includes tools_banned_functions PROPERTIES LABELS tools)

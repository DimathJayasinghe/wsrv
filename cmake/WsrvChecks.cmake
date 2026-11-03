# SPDX-License-Identifier: Apache-2.0
#
# Quality-gate targets (docs/BUILD_AND_TOOLING.md section 5). CI runs exactly these.
#   format-check   clang-format must produce no diff
#   check-banned   no calls to banned C functions
#   check-includes module dependency graph + sans-I/O include rules
#   header-check   every header compiles on its own (include-what-you-use hygiene)
include_guard(GLOBAL)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_custom_target(format-check
    COMMAND "${PROJECT_SOURCE_DIR}/tools/format.sh" check
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "clang-format --dry-run -Werror"
    VERBATIM)

add_custom_target(check-banned
    COMMAND "${PROJECT_SOURCE_DIR}/tools/check_banned_functions.sh"
    COMMENT "Scanning for banned functions"
    VERBATIM)

add_custom_target(check-includes
    COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/tools/check_includes.py"
            --root "${PROJECT_SOURCE_DIR}"
    COMMENT "Checking module include rules"
    VERBATIM)

# Header self-containment: one generated translation unit per header, containing only its #include.
# (Globbing is deliberate here: new headers must be picked up automatically.)
file(GLOB_RECURSE _wsrv_headers CONFIGURE_DEPENDS
    RELATIVE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/src/*.hpp")
set(_wsrv_header_tus "")
foreach(_h IN LISTS _wsrv_headers)
    string(REPLACE "/" "__" _stem "${_h}")
    set(_tu "${PROJECT_BINARY_DIR}/header_check/${_stem}.cpp")
    file(CONFIGURE OUTPUT "${_tu}" CONTENT "#include \"${_h}\"\n" @ONLY)   # rewritten only if changed
    list(APPEND _wsrv_header_tus "${_tu}")
endforeach()

if(_wsrv_header_tus)
    add_library(wsrv_header_check OBJECT EXCLUDE_FROM_ALL ${_wsrv_header_tus})
    target_link_libraries(wsrv_header_check PRIVATE
        wsrv::base wsrv::crypto wsrv::http wsrv::protocol wsrv::extensions wsrv::net wsrv::server wsrv::capi wsrv::ipc)
    wsrv_apply_warnings(wsrv_header_check)
    add_custom_target(header-check DEPENDS wsrv_header_check)
endif()

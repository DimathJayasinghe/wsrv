# SPDX-License-Identifier: Apache-2.0
# wsrv_capi: the C++ behaviour (EmbeddedServer & co) plus the status table, testable without any C.
wsrv_add_module(NAME capi SOURCES event_queue.cpp handshake_blob.cpp host_bridge.cpp embedded_config.cpp embedded_server.cpp c_status.cpp DEPS wsrv::base wsrv::net wsrv::server)
# The one public header lives outside src/ (include/wsrv/wsrv.h, ADR-0016 section 1).
target_include_directories(wsrv_capi PUBLIC "${PROJECT_SOURCE_DIR}/include")

# libwsrv.so: the extern "C" shim only. Everything is hidden except the WSRV_API functions, and
# the static libraries it absorbs are hidden too (--exclude-libs), so the export list is exactly
# include/wsrv/wsrv.symbols (checked by the capi_abi_symbols test).
add_library(wsrv_shared SHARED c_api.cpp)
set_target_properties(wsrv_shared PROPERTIES
    OUTPUT_NAME wsrv
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    VERSION ${PROJECT_VERSION}
    SOVERSION 0)
target_link_libraries(wsrv_shared PRIVATE wsrv::capi)
# The linker enforces the export list: an anonymous version script generated from wsrv.symbols makes
# every other symbol local (standard-library template typeinfo would otherwise leak out with default
# visibility). It is anonymous on purpose: no symbol versioning (name@@VERSION) is introduced; that
# would be an ABI decision for an ADR. tools/check_c_api.py checks wsrv.h against wsrv.symbols.
set(WSRV_SYMBOLS_FILE "${PROJECT_SOURCE_DIR}/include/wsrv/wsrv.symbols")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${WSRV_SYMBOLS_FILE}")
file(STRINGS "${WSRV_SYMBOLS_FILE}" _wsrv_exports)
set(_wsrv_map "${CMAKE_CURRENT_BINARY_DIR}/wsrv.map")
set(_wsrv_map_text "{\n  global:\n")
foreach(_symbol IN LISTS _wsrv_exports)
    string(APPEND _wsrv_map_text "    ${_symbol};\n")
endforeach()
string(APPEND _wsrv_map_text "  local:\n    *;\n};\n")
file(GENERATE OUTPUT "${_wsrv_map}" CONTENT "${_wsrv_map_text}")
target_link_options(wsrv_shared PRIVATE -Wl,--exclude-libs,ALL "-Wl,--version-script=${_wsrv_map}")
set_target_properties(wsrv_shared PROPERTIES LINK_DEPENDS "${_wsrv_map}")
wsrv_apply_warnings(wsrv_shared)
add_library(wsrv::shared ALIAS wsrv_shared)

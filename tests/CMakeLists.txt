# SPDX-License-Identifier: Apache-2.0
wsrv_add_test(NAME base_version_test SOURCES base/version_test.cpp LINK wsrv::base)
wsrv_add_test(NAME base_error_test SOURCES base/error_test.cpp LINK wsrv::base)
wsrv_add_test(NAME base_checked_test SOURCES base/checked_test.cpp LINK wsrv::base)
wsrv_add_test(NAME base_assert_test SOURCES base/assert_test.cpp LINK wsrv::base)
wsrv_add_test(NAME base_bytes_test SOURCES base/bytes_test.cpp LINK wsrv::base)
wsrv_add_test(NAME base_byte_buffer_test SOURCES base/byte_buffer_test.cpp LINK wsrv::base)
wsrv_add_test(NAME base_ring_buffer_test SOURCES base/ring_buffer_test.cpp LINK wsrv::base)
wsrv_add_test(NAME base_clock_test SOURCES base/clock_test.cpp LINK wsrv::base)
wsrv_add_test(NAME base_logger_test SOURCES base/logger_test.cpp LINK wsrv::base)
wsrv_add_test(NAME base_random_test SOURCES base/random_test.cpp LINK wsrv::base)
wsrv_add_test(NAME net_system_random_test SOURCES net/system_random_test.cpp LINK wsrv::net)

# Architecture guard: the real link graph must equal the allowed graph (docs/ARCHITECTURE.md 2.1).
include(WsrvModuleGraph)
wsrv_write_module_graph("${CMAKE_BINARY_DIR}/module_graph.actual"
    MODULES base crypto http protocol extensions net server)
add_test(NAME module_graph
    COMMAND ${CMAKE_COMMAND} -E compare_files
            "${CMAKE_CURRENT_SOURCE_DIR}/module_graph.expected"
            "${CMAKE_BINARY_DIR}/module_graph.actual")
add_subdirectory(sanitizer_selftest)
add_subdirectory(tools)

# Self-test of the guard above: a PRIVATE link on a STATIC library must be visible to the graph writer
# (CMake records it as $<LINK_ONLY:...>), otherwise a forbidden private dependency would pass unnoticed.
add_library(wsrv_zzsrc STATIC EXCLUDE_FROM_ALL module_graph_fixture/empty.cpp)
add_library(wsrv_zzdst INTERFACE)
target_link_libraries(wsrv_zzsrc PRIVATE wsrv_zzdst)
wsrv_write_module_graph("${CMAKE_BINARY_DIR}/module_graph_private.actual" MODULES zzsrc)
add_test(NAME module_graph_sees_private_links
    COMMAND ${CMAKE_COMMAND} -E compare_files
            "${CMAKE_CURRENT_SOURCE_DIR}/module_graph_private.expected"
            "${CMAKE_BINARY_DIR}/module_graph_private.actual")

# SPDX-License-Identifier: Apache-2.0
wsrv_add_test(NAME base_version_test SOURCES base/version_test.cpp LINK wsrv::base)

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

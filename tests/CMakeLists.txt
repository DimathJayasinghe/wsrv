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

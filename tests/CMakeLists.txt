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
wsrv_add_test(NAME net_io_error_test SOURCES net/io_error_test.cpp LINK wsrv::net)
wsrv_add_test(NAME net_fd_test SOURCES net/fd_test.cpp LINK wsrv::net)
wsrv_add_test(NAME net_syscalls_test SOURCES net/syscalls_test.cpp LINK wsrv::net)
wsrv_add_test(NAME net_timer_wheel_test SOURCES net/timer_wheel_test.cpp LINK wsrv::net)
wsrv_add_test(NAME net_epoll_poller_test SOURCES net/epoll_poller_test.cpp LINK wsrv::net)
wsrv_add_test(NAME net_event_loop_test SOURCES net/event_loop_test.cpp LINK wsrv::net)
wsrv_add_test(NAME net_signal_watcher_test SOURCES net/signal_watcher_test.cpp LINK wsrv::net)
wsrv_add_test(NAME net_endpoint_test SOURCES net/endpoint_test.cpp LINK wsrv::net)
wsrv_add_test(NAME net_plain_transport_test SOURCES net/plain_transport_test.cpp LINK wsrv::net)
wsrv_add_test(NAME net_acceptor_test SOURCES net/acceptor_test.cpp LINK wsrv::net)
wsrv_add_test(NAME net_loopback_test SOURCES net/loopback_test.cpp LINK wsrv::net)

# Differential tests against OpenSSL are test-only and optional (ADR-0005: no runtime dependency).
find_package(OpenSSL QUIET)

wsrv_add_test(NAME http_token_test SOURCES http/token_test.cpp LINK wsrv::http)
wsrv_add_test(NAME http_request_test SOURCES http/request_test.cpp LINK wsrv::http)
wsrv_add_test(NAME http_request_parser_test SOURCES http/request_parser_test.cpp LINK wsrv::http)
wsrv_add_test(NAME http_host_test SOURCES http/host_test.cpp LINK wsrv::http)
wsrv_add_test(NAME http_response_test SOURCES http/response_test.cpp LINK wsrv::http)
wsrv_add_test(NAME crypto_base64_test SOURCES crypto/base64_test.cpp LINK wsrv::crypto)
wsrv_add_test(NAME crypto_sha1_test SOURCES crypto/sha1_test.cpp LINK wsrv::crypto)
wsrv_add_test(NAME protocol_extension_offer_test SOURCES protocol/extension_offer_test.cpp LINK wsrv::protocol)
wsrv_add_test(NAME protocol_handshake_test SOURCES protocol/handshake_test.cpp LINK wsrv::protocol)
wsrv_add_test(NAME protocol_handshake_flow_test SOURCES protocol/handshake_flow_test.cpp LINK wsrv::protocol)
wsrv_add_test(NAME protocol_opcode_test SOURCES protocol/opcode_test.cpp LINK wsrv::protocol)
wsrv_add_test(NAME protocol_close_code_test SOURCES protocol/close_code_test.cpp LINK wsrv::protocol)
wsrv_add_test(NAME protocol_masker_test SOURCES protocol/masker_test.cpp LINK wsrv::protocol)
wsrv_add_test(NAME protocol_frame_writer_test SOURCES protocol/frame_writer_test.cpp LINK wsrv::protocol)
wsrv_add_test(NAME protocol_frame_parser_test SOURCES protocol/frame_parser_test.cpp LINK wsrv::protocol)
wsrv_add_test(NAME protocol_message_assembler_test SOURCES protocol/message_assembler_test.cpp LINK wsrv::protocol)
wsrv_add_test(NAME protocol_outbound_scheduler_test SOURCES protocol/outbound_scheduler_test.cpp LINK wsrv::protocol)
wsrv_add_test(NAME protocol_session_test SOURCES protocol/session_test.cpp LINK wsrv::protocol)
wsrv_add_test(NAME protocol_utf8_test SOURCES protocol/utf8_test.cpp LINK wsrv::protocol)
if(TARGET OpenSSL::Crypto)
    target_link_libraries(crypto_sha1_test PRIVATE OpenSSL::Crypto)
    target_compile_definitions(crypto_sha1_test PRIVATE WSRV_TEST_HAS_OPENSSL=1)
endif()

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

if(WSRV_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()

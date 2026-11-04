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
wsrv_add_test(NAME net_unix_listener_test SOURCES net/unix_listener_test.cpp LINK wsrv::net)
wsrv_add_test(NAME net_acceptor_test SOURCES net/acceptor_test.cpp LINK wsrv::net)
wsrv_add_test(NAME net_loopback_test SOURCES net/loopback_test.cpp LINK wsrv::net)

if(WSRV_ENABLE_DEFLATE)
    wsrv_add_test(NAME extensions_zlib_codec_test SOURCES extensions/zlib_codec_test.cpp LINK wsrv::extensions)
endif()
if(WSRV_ENABLE_TLS)
    wsrv_add_test(NAME tls_sni_test SOURCES tls/sni_test.cpp LINK wsrv::tls)
    wsrv_add_test(NAME tls_server_context_sni_test SOURCES tls/server_context_sni_test.cpp
                  LINK wsrv::tls OpenSSL::SSL OpenSSL::Crypto)
    wsrv_add_test(NAME tls_server_context_test SOURCES tls/server_context_test.cpp
                  LINK wsrv::tls OpenSSL::SSL OpenSSL::Crypto)
    wsrv_add_test(NAME tls_transport_test SOURCES tls/tls_transport_test.cpp
                  LINK wsrv::tls OpenSSL::SSL OpenSSL::Crypto)
    wsrv_add_test(NAME tls_server_test SOURCES tls/tls_server_test.cpp
                  LINK wsrv::server wsrv::tls OpenSSL::SSL OpenSSL::Crypto)
endif()

wsrv_add_test(NAME server_connection_test SOURCES server/connection_test.cpp LINK wsrv::server)
wsrv_add_test(NAME server_server_test SOURCES server/server_test.cpp LINK wsrv::server)
wsrv_add_test(NAME integration_server_test SOURCES integration/server_integration_test.cpp LINK wsrv::server)
wsrv_add_test(NAME integration_multithread_test SOURCES integration/multithread_stress_test.cpp LINK wsrv::server)
wsrv_add_test(NAME server_multi_loop_test SOURCES server/multi_loop_test.cpp LINK wsrv::server)
wsrv_add_test(NAME server_session_handle_test SOURCES server/session_handle_test.cpp LINK wsrv::server)
wsrv_add_test(NAME server_handshake_decider_test SOURCES server/handshake_decider_test.cpp LINK wsrv::server)
wsrv_add_test(NAME server_hook_test SOURCES server/server_hook_test.cpp LINK wsrv::server)
wsrv_add_test(NAME server_config_test SOURCES server/config_test.cpp LINK wsrv::server)
wsrv_add_test(NAME capi_event_queue_test SOURCES capi/event_queue_test.cpp LINK wsrv::capi)
wsrv_add_test(NAME capi_host_bridge_test SOURCES capi/host_bridge_test.cpp LINK wsrv::capi)
wsrv_add_test(NAME capi_handshake_blob_test SOURCES capi/handshake_blob_test.cpp LINK wsrv::capi)
wsrv_add_test(NAME capi_embedded_config_test SOURCES capi/embedded_config_test.cpp LINK wsrv::capi)
wsrv_add_test(NAME capi_embedded_server_test SOURCES capi/embedded_server_test.cpp LINK wsrv::capi)
if(WSRV_ENABLE_TLS)
    target_compile_definitions(capi_embedded_server_test PRIVATE WSRV_TEST_HAS_TLS=1)
    wsrv_add_test(NAME capi_embedded_tls_test SOURCES capi/embedded_tls_test.cpp
                  LINK wsrv::capi OpenSSL::SSL OpenSSL::Crypto)
endif()
wsrv_add_test(NAME ipc_message_test SOURCES ipc/message_test.cpp LINK wsrv::ipc)
wsrv_add_test(NAME ipc_frame_decoder_test SOURCES ipc/frame_decoder_test.cpp LINK wsrv::ipc)
wsrv_add_test(NAME wsrvd_token_test SOURCES wsrvd/token_test.cpp LINK wsrv::wsrvd)
wsrv_add_test(NAME wsrvd_handshake_test SOURCES wsrvd/handshake_test.cpp LINK wsrv::wsrvd)
wsrv_add_test(NAME wsrvd_config_test SOURCES wsrvd/config_test.cpp LINK wsrv::wsrvd)
wsrv_add_test(NAME wsrvd_token_file_test SOURCES wsrvd/token_file_test.cpp LINK wsrv::wsrvd)
wsrv_add_test(NAME wsrvd_ipc_server_test SOURCES wsrvd/ipc_server_test.cpp LINK wsrv::wsrvd)
wsrv_add_test(NAME wsrvd_startup_test SOURCES wsrvd/startup_test.cpp LINK wsrv::wsrvd)
wsrv_add_test(NAME wsrvd_event_convert_test SOURCES wsrvd/event_convert_test.cpp LINK wsrv::wsrvd)
wsrv_add_test(NAME wsrvd_relay_test SOURCES wsrvd/relay_test.cpp LINK wsrv::wsrvd)
wsrv_add_test(NAME wsrvd_reload_test SOURCES wsrvd/reload_test.cpp LINK wsrv::wsrvd)
if(WSRV_ENABLE_TLS)
    target_link_libraries(wsrvd_reload_test PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(wsrvd_reload_test PRIVATE WSRV_TEST_HAS_TLS=1)
endif()
wsrv_add_test(NAME capi_c_status_test SOURCES capi/c_status_test.cpp LINK wsrv::capi)

# The C ABI (ADR-0016 section 8): exports equal the checked-in list, and a C-only program can use
# the shared library. The consumer is C99 with the strictest warnings, so wsrv.h stays pure C.
enable_language(C)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
add_executable(capi_consumer capi/consumer.c)
target_link_libraries(capi_consumer PRIVATE wsrv::shared)
target_include_directories(capi_consumer PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_options(capi_consumer PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    -Wstrict-prototypes -Wmissing-prototypes $<$<BOOL:${WSRV_WARNINGS_AS_ERRORS}>:-Werror>)
add_test(NAME capi_consumer COMMAND capi_consumer)
add_test(NAME capi_abi_symbols
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/capi/check_symbols.sh
            $<TARGET_FILE:wsrv_shared> ${PROJECT_SOURCE_DIR}/include/wsrv/wsrv.symbols)
# wsrv.h and wsrv.symbols must agree, otherwise the (symbols-driven) linker script hides a function
# the header promises. Python is already a tooling requirement (WsrvChecks).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME capi_header_matches_symbols
    COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/check_c_api.py --root ${PROJECT_SOURCE_DIR})
set_tests_properties(capi_consumer capi_abi_symbols capi_header_matches_symbols PROPERTIES LABELS capi)

# PHP binding (M12, bindings/php): unit tests of the pure-PHP sidecar client, and an interop test that
# starts the REAL wsrvd and drives it with the PHP Worker and a raw WebSocket client. PHP is optional
# for building the server; CI checks that it is present so these cannot be skipped there unnoticed.
find_program(WSRV_PHP php)
if(WSRV_PHP)
    add_test(NAME php_unit
        COMMAND ${WSRV_PHP} ${PROJECT_SOURCE_DIR}/bindings/php/tests/run.php)
    add_test(NAME php_interop
        COMMAND ${WSRV_PHP} ${PROJECT_SOURCE_DIR}/bindings/php/tests/interop.php $<TARGET_FILE:wsrvd>)
    set_tests_properties(php_unit php_interop PROPERTIES
        LABELS php TIMEOUT 120 ENVIRONMENT "WSRV_PHP_NO_SKIP=1")
else()
    message(STATUS "php not found: the PHP binding tests are not registered")
endif()

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
wsrv_add_test(NAME protocol_permessage_deflate_test SOURCES protocol/permessage_deflate_test.cpp LINK wsrv::protocol)
wsrv_add_test(NAME protocol_handshake_test SOURCES protocol/handshake_test.cpp LINK wsrv::protocol)
wsrv_add_test(NAME protocol_handshake_driver_test SOURCES protocol/handshake_driver_test.cpp LINK wsrv::protocol)
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
set(_graph_modules base crypto http protocol extensions net server capi ipc wsrvd)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/module_graph.expected" _graph_expected)
if(WSRV_ENABLE_TLS)
    list(APPEND _graph_modules tls)
    string(APPEND _graph_expected "tls: base net\n")   # the optional module comes last (ADR-0019)
    # The embedded server (a composition root) uses it when TLS is built.
    string(REPLACE "capi: base net server\n" "capi: base net server tls\n" _graph_expected "${_graph_expected}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/module_graph.expected")
file(WRITE "${CMAKE_BINARY_DIR}/module_graph.expected" "${_graph_expected}")
wsrv_write_module_graph("${CMAKE_BINARY_DIR}/module_graph.actual" MODULES ${_graph_modules})
add_test(NAME module_graph
    COMMAND ${CMAKE_COMMAND} -E compare_files
            "${CMAKE_BINARY_DIR}/module_graph.expected"
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

if(WSRV_BUILD_BENCH)
    find_package(Threads REQUIRED)
    add_subdirectory(bench)
endif()

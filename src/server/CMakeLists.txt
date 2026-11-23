# SPDX-License-Identifier: Apache-2.0
wsrv_add_module(NAME server SOURCES config.cpp connection.cpp loop_host.cpp connection_table.cpp loop_shard.cpp worker_thread.cpp server.cpp server_builder.cpp session_handle.cpp message_writer.cpp handshake_decider.cpp ip_limits.cpp client_ip.cpp admin_http.cpp admin_endpoint.cpp publish_endpoint.cpp publish_http.cpp broker.cpp DEPS wsrv::base wsrv::http wsrv::protocol wsrv::net wsrv::extensions)
# ServerBuilder::permessage_deflate() builds the zlib codecs only when the extension is compiled in
# (ADR-0020); without it the call fails closed with a ConfigError.
if(WSRV_ENABLE_DEFLATE)
    target_compile_definitions(wsrv_server PRIVATE WSRV_HAS_DEFLATE=1)
endif()

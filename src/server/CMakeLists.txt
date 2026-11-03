# SPDX-License-Identifier: Apache-2.0
wsrv_add_module(NAME server SOURCES config.cpp connection.cpp loop_host.cpp connection_table.cpp loop_shard.cpp worker_thread.cpp server.cpp server_builder.cpp session_handle.cpp message_writer.cpp handshake_decider.cpp DEPS wsrv::base wsrv::http wsrv::protocol wsrv::net wsrv::extensions)

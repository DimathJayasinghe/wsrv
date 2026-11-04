# SPDX-License-Identifier: Apache-2.0
# Sans-I/O (ADR-0002): must never depend on net/ or any OS header.
wsrv_add_module(NAME protocol SOURCES utf8.cpp extension_offer.cpp permessage_deflate.cpp handshake.cpp handshake_rules.cpp close_payload.cpp masker.cpp frame_writer.cpp frame_parser.cpp message_assembler.cpp outbound_scheduler.cpp session.cpp handshake_driver.cpp DEPS wsrv::base wsrv::crypto wsrv::http)

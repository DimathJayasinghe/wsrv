# SPDX-License-Identifier: Apache-2.0
# Sans-I/O (ADR-0002): must never depend on net/ or any OS header.
wsrv_add_module(NAME protocol SOURCES utf8.cpp extension_offer.cpp handshake.cpp handshake_rules.cpp close_payload.cpp DEPS wsrv::base wsrv::crypto wsrv::http)

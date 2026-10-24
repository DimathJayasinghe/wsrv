# SPDX-License-Identifier: Apache-2.0
# Sans-I/O (ADR-0002): strict HTTP/1.1 handshake parsing and serialisation; base only.
wsrv_add_module(NAME http SOURCES token.cpp request.cpp DEPS wsrv::base)

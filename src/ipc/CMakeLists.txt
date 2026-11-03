# SPDX-License-Identifier: Apache-2.0
# Sans-I/O (ADR-0017, like protocol/): depends on base only, no OS headers.
wsrv_add_module(NAME ipc SOURCES message.cpp frame_decoder.cpp DEPS wsrv::base)

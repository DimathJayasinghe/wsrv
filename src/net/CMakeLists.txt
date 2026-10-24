# SPDX-License-Identifier: Apache-2.0
# All OS-specific code lives here (ADR-0004). Must never depend on protocol/.
wsrv_add_module(NAME net SOURCES system_random.cpp DEPS wsrv::base)

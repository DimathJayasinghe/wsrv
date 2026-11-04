# SPDX-License-Identifier: Apache-2.0
# The OpenSSL TLS transport (ADR-0019). The only module that includes <openssl/...>; OpenSSL is a
# PRIVATE link so nothing that links wsrv::tls inherits it. Built only with WSRV_ENABLE_TLS.
wsrv_add_module(NAME tls SOURCES server_context.cpp DEPS wsrv::base wsrv::net)
target_link_libraries(wsrv_tls PRIVATE OpenSSL::SSL OpenSSL::Crypto)

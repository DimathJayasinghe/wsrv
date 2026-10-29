# SPDX-License-Identifier: Apache-2.0
wsrv_add_module(NAME server SOURCES config.cpp connection.cpp DEPS wsrv::base wsrv::http wsrv::protocol wsrv::net wsrv::extensions)

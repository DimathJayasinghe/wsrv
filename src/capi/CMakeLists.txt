# SPDX-License-Identifier: Apache-2.0
wsrv_add_module(NAME capi SOURCES event_queue.cpp host_bridge.cpp DEPS wsrv::base wsrv::net wsrv::server)

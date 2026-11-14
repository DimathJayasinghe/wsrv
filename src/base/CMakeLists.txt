# SPDX-License-Identifier: Apache-2.0
wsrv_add_module(NAME base SOURCES metrics.cpp version.cpp error.cpp assert.cpp byte_buffer.cpp ring_buffer.cpp)

# The version string comes from project(VERSION ...) so there is exactly one place to change it.
target_compile_definitions(wsrv_base PUBLIC WSRV_VERSION="${PROJECT_VERSION}")

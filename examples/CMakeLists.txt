# SPDX-License-Identifier: Apache-2.0
# Examples are consumers of the public API: they link wsrv::server and nothing else.
add_executable(echo echo/main.cpp)
target_link_libraries(echo PRIVATE wsrv::server)
wsrv_apply_warnings(echo)

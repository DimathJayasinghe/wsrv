# SPDX-License-Identifier: Apache-2.0
# Examples are consumers of the public API: they link wsrv::server and nothing else.
add_executable(echo echo/main.cpp)
target_link_libraries(echo PRIVATE wsrv::server)
if(WSRV_ENABLE_TLS)
    target_link_libraries(echo PRIVATE wsrv::tls)
    target_compile_definitions(echo PRIVATE WSRV_HAS_TLS=1)
endif()
wsrv_apply_warnings(echo)

add_executable(chat chat/main.cpp)
target_link_libraries(chat PRIVATE wsrv::server)
wsrv_apply_warnings(chat)

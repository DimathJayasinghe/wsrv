# SPDX-License-Identifier: Apache-2.0
# wsrvd (ADR-0017): the sidecar daemon. This is a composition root, so it may use concrete types
# from capi, ipc and net (docs/ARCHITECTURE.md section 2.1). The daemon core is a library so tests
# run it in-process; main.cpp stays thin.
wsrv_add_module(NAME wsrvd SOURCES handshake.cpp config.cpp token_file.cpp ipc_server.cpp startup.cpp event_convert.cpp relay.cpp reload.cpp healthcheck.cpp DEPS wsrv::base wsrv::net wsrv::ipc wsrv::capi)
# Headers are included as "wsrvd/<file>.hpp" (include root: apps/).
target_include_directories(wsrv_wsrvd PUBLIC "${PROJECT_SOURCE_DIR}/apps")

# The official container image names its default IPC socket here, so that `-e WSRVD_IPC_PORT=...` alone
# selects the TCP transport there (ADR-0038). Every other build has no default.
set(WSRVD_DEFAULT_IPC_SOCKET "" CACHE STRING "Unix socket used when neither ipc_socket nor ipc_port is set")
if(WSRVD_DEFAULT_IPC_SOCKET)
    target_compile_definitions(wsrv_wsrvd PRIVATE "WSRVD_DEFAULT_IPC_SOCKET=\"${WSRVD_DEFAULT_IPC_SOCKET}\"")
endif()

add_executable(wsrvd main.cpp)
target_link_libraries(wsrvd PRIVATE wsrv::wsrvd)
wsrv_apply_warnings(wsrvd)

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

# The C twin of the echo server: plain C99 against include/wsrv/wsrv.h and libwsrv.so, nothing else
# (M11: "the C consumer passes Autobahn", tests/autobahn/run.sh --capi). Same strict flags as the
# C consumer test, so the public header stays pure C.
enable_language(C)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
add_executable(echo_c echo_c/main.c)
target_link_libraries(echo_c PRIVATE wsrv::shared)
target_include_directories(echo_c PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_options(echo_c PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    -Wstrict-prototypes -Wmissing-prototypes $<$<BOOL:${WSRV_WARNINGS_AS_ERRORS}>:-Werror>)

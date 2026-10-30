# SPDX-License-Identifier: Apache-2.0
# Load generator for the scaling benchmark and the soak run (build with the `bench` preset).
add_executable(wsrv_load load.cpp)
target_link_libraries(wsrv_load PRIVATE wsrv::server Threads::Threads)
target_include_directories(wsrv_load PRIVATE "${PROJECT_SOURCE_DIR}/tests")
wsrv_apply_warnings(wsrv_load)

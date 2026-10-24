# SPDX-License-Identifier: Apache-2.0
#
# Sanitizer self-tests: each program commits one specific bug, and the test PASSES only if the matching
# sanitizer prints its specific report. This proves the sanitizers are active, not merely configured.
# These executables intentionally get no project warning flags (they contain deliberate bugs).
function(wsrv_add_selftest name source regex)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE Threads::Threads)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        PASS_REGULAR_EXPRESSION "${regex}"
        LABELS sanitizer-selftest)
endfunction()

find_package(Threads REQUIRED)

if(WSRV_SANITIZER STREQUAL "asan-ubsan")
    wsrv_add_selftest(selftest_asan_heap_overflow heap_overflow.cpp "AddressSanitizer: heap-buffer-overflow")
    wsrv_add_selftest(selftest_ubsan_signed_overflow signed_overflow.cpp "runtime error: signed integer overflow")
elseif(WSRV_SANITIZER STREQUAL "tsan")
    wsrv_add_selftest(selftest_tsan_data_race data_race.cpp "WARNING: ThreadSanitizer: data race")
endif()

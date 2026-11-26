# SPDX-License-Identifier: Apache-2.0
#
# libFuzzer targets (docs/TESTING_STRATEGY.md section 5). Built only with WSRV_BUILD_FUZZERS=ON (preset
# `fuzz`, Clang). Run one:  build/fuzz/tests/fuzz/fuzz_utf8 tests/fuzz/corpus/utf8 -max_total_time=60
# Each target also has a ctest that replays the committed corpus once (-runs=0): a regression check that
# every past crasher (added to the corpus directory) stays fixed.
function(wsrv_add_fuzzer name sources link)
    add_executable(${name} ${sources})
    target_link_libraries(${name} PRIVATE ${link})
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
    # Ubuntu's libFuzzer runtime is built against libstdc++ while our Clang builds use libc++ (needed for
    # std::expected, BUILD_AND_TOOLING 1.1). The runtime only talks to us through extern "C", so linking
    # libstdc++ as well is safe and resolves its std::__throw_* symbols. Two traps found while doing this:
    #  * with -stdlib=libc++ the Clang driver silently rewrites `-lstdc++` into `-lc++`, so the library
    #    must be named by file (`-l:libstdc++.so.6`);
    #  * --no-as-needed keeps it even though nothing in OUR objects references it.
    # push-state/pop-state confine --no-as-needed to this one library instead of every library after it.
    target_link_libraries(${name} PRIVATE -Wl,--push-state,--no-as-needed -l:libstdc++.so.6 -Wl,--pop-state)
    wsrv_apply_warnings(${name} TEST)   # harnesses are code too: warnings are errors
    target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/src")
    string(REPLACE "fuzz_" "" _corpus ${name})
    add_test(NAME ${name}_corpus_replay
        COMMAND ${name} -runs=0 "${CMAKE_CURRENT_SOURCE_DIR}/corpus/${_corpus}")
    set_tests_properties(${name}_corpus_replay PROPERTIES LABELS fuzz-regression)
endfunction()

wsrv_add_fuzzer(fuzz_utf8 fuzz_utf8.cpp wsrv::protocol)
wsrv_add_fuzzer(fuzz_base64 fuzz_base64.cpp wsrv::crypto)
wsrv_add_fuzzer(fuzz_sha1 fuzz_sha1.cpp wsrv::crypto)
wsrv_add_fuzzer(fuzz_http_request fuzz_http_request.cpp wsrv::http)
wsrv_add_fuzzer(fuzz_publish_request fuzz_publish_request.cpp wsrv::http)
wsrv_add_fuzzer(fuzz_admin_http fuzz_admin_http.cpp wsrv::server)
wsrv_add_fuzzer(fuzz_handshake fuzz_handshake.cpp wsrv::protocol)
wsrv_add_fuzzer(fuzz_frame_parser fuzz_frame_parser.cpp wsrv::protocol)
wsrv_add_fuzzer(fuzz_message_pipeline fuzz_message_pipeline.cpp wsrv::protocol)
if(WSRV_ENABLE_DEFLATE)
    wsrv_add_fuzzer(fuzz_session fuzz_session.cpp "wsrv::protocol;wsrv::extensions")
    target_compile_definitions(fuzz_session PRIVATE WSRV_FUZZ_DEFLATE=1)
else()
    wsrv_add_fuzzer(fuzz_session fuzz_session.cpp wsrv::protocol)
endif()
wsrv_add_fuzzer(fuzz_ipc_frame fuzz_ipc_frame.cpp wsrv::ipc)
wsrv_add_fuzzer(fuzz_handshake_blob fuzz_handshake_blob.cpp wsrv::capi)

# permessage-deflate inflater (ADR-0020): zlib behind IMessageInflater, fed hostile bytes.
if(WSRV_ENABLE_DEFLATE)
    wsrv_add_fuzzer(fuzz_inflate fuzz_inflate.cpp wsrv::extensions)
endif()

# TLS server side (ADR-0019): a real OpenSSL server behind TlsTransport, fed hostile bytes. It builds a
# certificate in-process with the test fixtures (tests/support/tls_fixtures.hpp).
if(WSRV_ENABLE_TLS)
    wsrv_add_fuzzer(fuzz_tls_server fuzz_tls_server.cpp "wsrv::tls;OpenSSL::SSL;OpenSSL::Crypto")
    target_include_directories(fuzz_tls_server PRIVATE "${PROJECT_SOURCE_DIR}/tests")
endif()

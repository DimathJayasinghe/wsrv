// SPDX-License-Identifier: Apache-2.0
#pragma once

// ASan/TSan treat an impossible `operator new` as fatal (they cannot throw bad_alloc), so behaviour
// on allocation failure can only be observed in unsanitised builds; the `release` preset runs it.
// Defines WSRV_TEST_SANITIZED when the test binary is built with a sanitizer allocator.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define WSRV_TEST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) ||                         \
    __has_feature(memory_sanitizer)
#define WSRV_TEST_SANITIZED 1
#endif
#endif

// SPDX-License-Identifier: Apache-2.0
// libFuzzer target: the admin listener's answer to any head (ADR-0023, ADR-0041). Whatever the
// bytes are, the answer is one of the documented statuses, and the metrics page is rendered only
// for a head that contains the configured token: a refusal never depends on which rule the bytes
// broke, and nothing the fuzzer sends can reach `metrics_text()` without the credential.
#include "base/assert.hpp"
#include "server/admin_http.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view kToken = "wsrv1-fuzz-token-0123456789abcdef0123456789abcdef";
constexpr std::string_view kPage = "wsrv_fuzz_page_marker 1\n";

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // The listener reads at most this much of a head; the same bound keeps the target fast.
    const std::string head{reinterpret_cast<const char*>(data),
                           std::min(size, wsrv::kMaxAdminHeadBytes)};

    wsrv::AdminSources open;
    open.metrics_text = [] { return std::string{kPage}; };
    open.ready = [] { return true; };

    wsrv::AdminSources guarded = open;
    guarded.token = std::string{kToken};
    std::size_t refusals = 0;
    guarded.unauthorized = [&refusals] { ++refusals; };

    for (const wsrv::AdminSources* sources : {&open, &guarded}) {
        const std::string answer = wsrv::admin_response(head, *sources);
        WSRV_ASSERT(answer.rfind("HTTP/1.1 ", 0) == 0);
        const std::string_view status = std::string_view{answer}.substr(9, 3);
        WSRV_ASSERT(status == "200" || status == "400" || status == "401" || status == "404" ||
                    status == "405" || status == "503");
    }

    // With a token, the page is the answer only when the token is in the head, and a 401 is the
    // same bytes whatever was wrong with the credential.
    const std::string answer = wsrv::admin_response(head, guarded);
    if (answer.find(kPage) != std::string::npos) {
        WSRV_ASSERT(head.find(kToken) != std::string::npos);
        WSRV_ASSERT(refusals == 0);
    }
    if (answer.rfind("HTTP/1.1 401", 0) == 0) {
        WSRV_ASSERT(refusals >= 1);
        WSRV_ASSERT(answer.find("WWW-Authenticate: Bearer\r\n") != std::string::npos);
        WSRV_ASSERT(answer.find(kToken) == std::string::npos);
    }
    return 0;
}

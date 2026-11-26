// SPDX-License-Identifier: Apache-2.0
#include "http/bearer.hpp"

#include "http/token.hpp"

namespace wsrv {

namespace {

constexpr std::string_view kScheme = "Bearer";

/// "Bearer" (any case, RFC 9110 section 11.1), exactly one space, then a credential with no space
/// or control character in it. Anything else is "not a credential".
std::optional<std::string_view> bearer_of(std::string_view value) {
    if (value.size() <= kScheme.size() + 1 || !iequals(value.substr(0, kScheme.size()), kScheme) ||
        value[kScheme.size()] != ' ') {
        return std::nullopt;
    }
    const std::string_view credential = value.substr(kScheme.size() + 1);
    for (const char c : credential) {
        if (c <= ' ' || c == 0x7f) {
            return std::nullopt;
        }
    }
    return credential;
}

}  // namespace

std::optional<std::string_view> bearer_credential(const HttpRequest& head) {
    const auto auth = head.values("Authorization");
    return auth.size() == 1 ? bearer_of(auth.front()) : std::nullopt;
}

}  // namespace wsrv

// SPDX-License-Identifier: Apache-2.0
#include "http/publish_request.hpp"

#include "http/token.hpp"

#include <cstddef>
#include <limits>
#include <optional>
#include <string_view>

namespace wsrv {

namespace {

constexpr std::string_view kTarget = "/publish";
constexpr std::string_view kScheme = "Bearer";
/// 2^64 has 20 digits; a length with more than 19 cannot be a std::size_t, so it is refused
/// without being parsed.
constexpr std::size_t kMaxDecimalDigits = 19;

HttpResult<PublishRequest> refuse(int status, std::string_view reason) {
    return std::unexpected{HttpError{status, reason}};
}

/// A non-empty run of ASCII digits, nothing else, no leading zero (one value, one spelling), and
/// no overflow: checked per digit, never through atoi/strtoull, which accept signs, spaces and
/// "0x".
std::optional<std::uint64_t> parse_decimal(std::string_view text) noexcept {
    if (text.empty() || text.size() > kMaxDecimalDigits + 1 ||
        (text.size() > 1 && text[0] == '0')) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return std::nullopt;
        }
        value = value * 10U + digit;
    }
    return value;
}

/// "Bearer" (any case, RFC 9110 section 11.1), exactly one space, then a credential with no space
/// or control character in it. Anything else is "not a credential": 401.
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

/// Which names the query has shown so far: each may appear once.
struct Seen {
    bool topic = false;
    bool except = false;
    bool binary = false;
};

/// One `name=value` pair into `out`; false for an unknown, repeated or malformed one.
bool read_pair(std::string_view name, std::string_view value, Seen& seen, PublishRequest& out) {
    if (name == "topic" && !seen.topic && !value.empty()) {
        seen.topic = true;
        out.topic = std::string{value};
        return true;
    }
    if (name == "except" && !seen.except) {
        const std::optional<std::uint64_t> id = parse_decimal(value);
        seen.except = id.has_value();
        out.except = id.value_or(0);
        return seen.except;
    }
    if (name == "binary" && !seen.binary && (value == "0" || value == "1")) {
        seen.binary = true;
        out.binary = value == "1";
        return true;
    }
    return false;
}

/// Fill `out` from the query string, strictly. Each name once, no unknown name, no empty pair, no
/// '%' anywhere, no '=' or '?' inside a value: the topic is compared as the bytes that were sent.
/// May throw std::bad_alloc (the strings are bounded by the 4 KiB head); the caller catches it.
bool read_query(std::string_view query, PublishRequest& out) {
    if (query.find('%') != std::string_view::npos) {
        return false;
    }
    Seen seen;
    for (bool more = true; more;) {
        const std::size_t amp = query.find('&');
        more = amp != std::string_view::npos;
        const std::string_view pair = query.substr(0, amp);
        const std::size_t eq = pair.find('=');
        const std::string_view value =
            eq == std::string_view::npos ? std::string_view{} : pair.substr(eq + 1);
        if (eq == std::string_view::npos || value.find_first_of("=?") != std::string_view::npos ||
            !read_pair(pair.substr(0, eq), value, seen, out)) {
            return false;  // "a second separator in a value" was found by the fuzzer, not assumed
        }
        query = more ? query.substr(amp + 1) : std::string_view{};
    }
    return seen.topic;
}

}  // namespace

HttpResult<PublishRequest> interpret_publish(const HttpRequest& head) {
    if (head.method != "POST") {
        return refuse(405, "only POST");
    }
    const std::string_view target = head.target;
    const std::size_t mark = target.find('?');
    if (target.substr(0, mark) != kTarget) {
        return refuse(404, "no such page");
    }
    PublishRequest out;
    // Credential: shape only. Whether it is the right one is the caller's decision.
    const auto auth = head.values("Authorization");
    const std::optional<std::string_view> bearer =
        auth.size() == 1 ? bearer_of(auth.front()) : std::nullopt;
    if (!bearer) {
        return refuse(401, "credential required");
    }
    out.bearer = std::string{*bearer};
    // Framing: one unambiguous way to find the end of the body, or nothing (RFC 9112 6.3).
    if (head.count("Transfer-Encoding") != 0) {
        return refuse(501, "transfer codings are not supported");
    }
    if (head.count("Expect") != 0) {
        return refuse(417, "Expect is not supported");
    }
    const auto lengths = head.values("Content-Length");
    if (lengths.empty()) {
        return refuse(411, "Content-Length required");
    }
    const std::optional<std::uint64_t> length =
        lengths.size() == 1 ? parse_decimal(lengths.front()) : std::nullopt;
    if (!length || *length > std::numeric_limits<std::size_t>::max()) {
        return refuse(400, "bad Content-Length");
    }
    out.content_length = static_cast<std::size_t>(*length);
    if (mark == std::string_view::npos || !read_query(target.substr(mark + 1), out)) {
        return refuse(400, "bad query");
    }
    return out;
}

}  // namespace wsrv

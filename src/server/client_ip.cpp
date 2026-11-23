// SPDX-License-Identifier: Apache-2.0
#include "server/client_ip.hpp"

#include "http/token.hpp"

#include <algorithm>
#include <charconv>

namespace wsrv {

namespace {

constexpr std::size_t kMaxEntryBytes = 64;  // "[ffff:...:ffff]:65535" is 47; more is not an address

using Wide = std::array<std::uint8_t, 16>;

/// The 16-byte form of an endpoint: an IPv4 address becomes `::ffff:a.b.c.d`.
[[nodiscard]] Wide widen(const Endpoint& endpoint) noexcept {
    if (endpoint.v6) {
        return endpoint.address;
    }
    Wide wide{};
    wide[10] = 0xFF;
    wide[11] = 0xFF;
    std::copy_n(endpoint.address.begin(), 4, wide.begin() + 12);
    return wide;
}

/// Do the first `bits` bits of `a` and `b` agree?
[[nodiscard]] bool prefix_matches(const Wide& a, const Wide& b, unsigned bits) noexcept {
    const unsigned whole = bits / 8U;
    const unsigned rest = bits % 8U;
    for (unsigned i = 0; i < whole; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    if (rest == 0) {
        return true;
    }
    const auto mask = static_cast<std::uint8_t>(0xFFU << (8U - rest));
    return (a[whole] & mask) == (b[whole] & mask);
}

/// Decimal digits only, no sign, no leading zero (except "0" itself), at most `max_digits`.
[[nodiscard]] std::optional<unsigned> canonical_number(std::string_view text,
                                                       std::size_t max_digits) {
    if (text.empty() || text.size() > max_digits ||
        std::any_of(text.begin(), text.end(), [](char c) { return c < '0' || c > '9'; }) ||
        (text.size() > 1 && text.front() == '0')) {
        return std::nullopt;
    }
    unsigned value = 0;
    std::from_chars(text.data(), text.data() + text.size(), value);
    return value;
}

[[nodiscard]] bool valid_port(std::string_view text) {
    if (text.empty() || text.size() > 5 ||
        std::any_of(text.begin(), text.end(), [](char c) { return c < '0' || c > '9'; })) {
        return false;
    }
    unsigned value = 0;
    std::from_chars(text.data(), text.data() + text.size(), value);
    return value <= 65535;
}

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

/// One header entry: `1.2.3.4`, `1.2.3.4:5678`, `2001:db8::1`, `[2001:db8::1]`,
/// `[2001:db8::1]:443`. Nothing else (no names, no zone ids, no `unknown`): the proxy appended an
/// address or the whole header is not to be believed.
[[nodiscard]] std::optional<Endpoint> parse_entry(std::string_view entry) {
    entry = trim(entry);
    if (entry.empty() || entry.size() > kMaxEntryBytes) {
        return std::nullopt;
    }
    std::string_view ip = entry;
    if (entry.front() == '[') {
        const std::size_t close = entry.find(']');
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        ip = entry.substr(1, close - 1);
        const std::string_view rest = entry.substr(close + 1);
        if (ip.find(':') == std::string_view::npos ||
            (!rest.empty() && (rest.front() != ':' || !valid_port(rest.substr(1))))) {
            return std::nullopt;
        }
    } else if (const auto colons = std::count(entry.begin(), entry.end(), ':'); colons == 1) {
        const std::size_t at = entry.find(':');
        if (!valid_port(entry.substr(at + 1))) {
            return std::nullopt;
        }
        ip = entry.substr(0, at);
    }
    const Result<Endpoint> parsed = parse_endpoint(ip, 0);
    if (!parsed) {
        return std::nullopt;
    }
    return *parsed;
}

}  // namespace

Result<IpNet> parse_ip_net(std::string_view text) {
    const std::size_t slash = text.find('/');
    const std::string_view address = text.substr(0, slash);
    const Result<Endpoint> parsed = parse_endpoint(address, 0);
    if (!parsed) {
        return fail(ErrorCode::InvalidConfig, "trusted_proxy: not an address or range");
    }
    const unsigned family_bits = parsed->v6 ? 128U : 32U;
    unsigned prefix = family_bits;
    if (slash != std::string_view::npos) {
        const std::optional<unsigned> number = canonical_number(text.substr(slash + 1), 3);
        if (!number || *number > family_bits) {
            return fail(ErrorCode::InvalidConfig, "trusted_proxy: bad prefix length");
        }
        prefix = *number;
    }
    IpNet net;
    net.address = widen(*parsed);
    net.prefix = static_cast<std::uint8_t>(parsed->v6 ? prefix : 96U + prefix);
    return net;
}

bool is_trusted_proxy(const std::vector<IpNet>& trusted, const Endpoint& peer) noexcept {
    const Wide wide = widen(peer);
    return std::any_of(trusted.begin(), trusted.end(), [&](const IpNet& net) {
        return prefix_matches(wide, net.address, net.prefix);
    });
}

ForwardedStyle forwarded_style(std::string_view header_name) noexcept {
    return iequals(header_name, "X-Forwarded-For") ? ForwardedStyle::List : ForwardedStyle::Single;
}

namespace {

/// Every comma separated entry of every header line, in order. @return false when there are more
/// than kMaxClientIpHops (the list is then not examined at all, never truncated).
[[nodiscard]] bool split_entries(std::span<const std::string_view> values,
                                 std::vector<std::string_view>& entries) {
    for (const std::string_view value : values) {
        std::size_t from = 0;
        bool more = true;
        while (more) {
            const std::size_t comma = value.find(',', from);
            more = comma != std::string_view::npos;
            entries.push_back(value.substr(from, more ? comma - from : std::string_view::npos));
            if (entries.size() > kMaxClientIpHops) {
                return false;
            }
            from = more ? comma + 1 : from;
        }
    }
    return true;
}

/// Walk from the right: the first entry that is not a trusted proxy is the client; if all are
/// trusted the leftmost is. An entry that is not an address spoils the whole list.
[[nodiscard]] std::optional<Endpoint>
rightmost_untrusted(const std::vector<IpNet>& trusted, std::span<const std::string_view> entries) {
    std::optional<Endpoint> client;
    for (std::size_t i = entries.size(); i-- > 0;) {
        client = parse_entry(entries[i]);
        if (!client || !is_trusted_proxy(trusted, *client)) {
            return client;  // not an address (nothing): or the client itself
        }
    }
    return client;  // every hop is one of ours: the leftmost is the client
}

[[nodiscard]] std::size_t total_size(std::span<const std::string_view> values) noexcept {
    std::size_t total = 0;
    for (const std::string_view value : values) {
        total += value.size();
    }
    return total;
}

}  // namespace

std::optional<Endpoint> client_address(const std::vector<IpNet>& trusted,
                                       ForwardedStyle style,
                                       std::span<const std::string_view> values) {
    if (values.empty() || total_size(values) > kMaxClientIpHeaderBytes) {
        return std::nullopt;
    }
    if (style == ForwardedStyle::Single) {
        return values.size() == 1 ? parse_entry(values.front()) : std::nullopt;
    }
    std::vector<std::string_view> entries;
    if (!split_entries(values, entries)) {
        return std::nullopt;
    }
    return rightmost_untrusted(trusted, entries);
}

}  // namespace wsrv

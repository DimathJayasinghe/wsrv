// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/error.hpp"
#include "net/endpoint.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace wsrv {

/// How many `trusted_proxy` entries a configuration may hold, how many list entries of a forwarded
/// header are examined and how long the whole header value may be (ADR-0029 section 4). Beyond a
/// bound the header is ignored, never truncated: a half-read list could name the wrong client.
inline constexpr std::size_t kMaxTrustedProxies = 64;
inline constexpr std::size_t kMaxClientIpHops = 32;
inline constexpr std::size_t kMaxClientIpHeaderBytes = 1024;

/// One trusted range (ADR-0029): an address and a prefix length. IPv4 is kept as the IPv4-mapped
/// IPv6 form (`::ffff:a.b.c.d`) with the prefix shifted by 96, so one comparison serves both
/// families and a dual-stack listener cannot be used to dodge the list.
struct IpNet {
    std::array<std::uint8_t, 16> address{};
    std::uint8_t prefix = 128;  ///< leading bits that must match, over the 16-byte form

    friend bool operator==(const IpNet&, const IpNet&) = default;
};

/// "10.0.0.0/8", "192.168.1.5" (a single host), "::1", "fd00::/8". No spaces, brackets, ports or
/// zone ids. @return Error{InvalidConfig} for anything else. Thread-affinity: pure, any-thread.
[[nodiscard]] Result<IpNet> parse_ip_net(std::string_view text);

/// True if `peer` is inside one of `trusted`. An empty list trusts nobody. Pure, any-thread.
[[nodiscard]] bool is_trusted_proxy(const std::vector<IpNet>& trusted,
                                    const Endpoint& peer) noexcept;

/// How a forwarded header is read: `X-Forwarded-For` is a comma separated list that every proxy
/// appends to; the other usual names (`X-Real-IP`, `CF-Connecting-IP`) carry one address.
enum class ForwardedStyle : std::uint8_t { List, Single };

/// `List` for X-Forwarded-For (any letter case), `Single` for every other name.
[[nodiscard]] ForwardedStyle forwarded_style(std::string_view header_name) noexcept;

/// The client address named by a forwarded header, or nothing when the header cannot be used
/// (caller then keeps the proxy's own address, which only makes the limits stricter).
/// `values` is every instance of the header in request order. Only call this for a peer that is
/// in the trusted list: the header of any other peer must never be believed (ADR-0029 section 2).
///
/// List: walk from the right; the first entry that is not a trusted proxy is the client; if all
/// are trusted the leftmost is. Single: exactly one value, which must be an address. The returned
/// endpoint has port 0. Pure, any-thread.
[[nodiscard]] std::optional<Endpoint> client_address(const std::vector<IpNet>& trusted,
                                                     ForwardedStyle style,
                                                     std::span<const std::string_view> values);

}  // namespace wsrv

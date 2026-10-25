// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/error.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include <sys/socket.h>

namespace wsrv {

/// An IP address and port, without any resolver involved: the server only ever deals in address
/// literals, so a hostname in a config can never trigger a blocking DNS lookup on the loop thread.
/// IPv4 uses the first 4 bytes of `address`; IPv6 all 16 (network order).
struct Endpoint {
    std::array<std::uint8_t, 16> address{};
    bool v6 = false;
    std::uint16_t port = 0;

    friend bool operator==(const Endpoint&, const Endpoint&) = default;
};

/// Parse an IPv4 or IPv6 address literal (no brackets, no port, no whitespace, no names).
/// An IPv4-mapped IPv6 literal such as "::ffff:10.1.2.3" stays an IPv6 endpoint.
/// @return Error{InvalidArgument} for anything else. Thread-affinity: pure, any-thread.
[[nodiscard]] Result<Endpoint> parse_endpoint(std::string_view ip, std::uint16_t port);

/// "1.2.3.4:80" or "[::1]:80". Thread-affinity: pure, any-thread.
[[nodiscard]] std::string format_endpoint(const Endpoint& endpoint);

/// The one place the POSIX `sockaddr_storage` -> `sockaddr` idiom is spelled out, so the rest of
/// the code never needs a reinterpret_cast.
[[nodiscard]] inline sockaddr* as_sockaddr(sockaddr_storage& storage) noexcept {
    // sockaddr_storage is defined to be castable to sockaddr.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<sockaddr*>(&storage);
}
[[nodiscard]] inline const sockaddr* as_sockaddr(const sockaddr_storage& storage) noexcept {
    // sockaddr_storage is defined to be castable to sockaddr.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<const sockaddr*>(&storage);
}

/// Fill `storage` with the socket address of `endpoint`; @return its length.
[[nodiscard]] socklen_t to_sockaddr(const Endpoint& endpoint, sockaddr_storage& storage);

/// Read an AF_INET / AF_INET6 socket address; Error{InvalidArgument} for any other family.
[[nodiscard]] Result<Endpoint> from_sockaddr(const sockaddr_storage& storage);

}  // namespace wsrv

// SPDX-License-Identifier: Apache-2.0
#include "net/endpoint.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>

namespace wsrv {

namespace {
constexpr std::size_t kMaxLiteral = 45;  // INET6_ADDRSTRLEN - 1: the longest textual address
}  // namespace

Result<Endpoint> parse_endpoint(std::string_view ip, std::uint16_t port) {
    if (ip.empty() || ip.size() > kMaxLiteral) {
        return fail(ErrorCode::InvalidArgument, "not an IP address literal");
    }
    std::array<char, kMaxLiteral + 1> text{};  // inet_pton needs a terminated string
    std::copy(ip.begin(), ip.end(), text.begin());
    Endpoint endpoint;
    endpoint.port = port;
    if (ip.find(':') != std::string_view::npos) {
        in6_addr address{};
        if (::inet_pton(AF_INET6, text.data(), &address) != 1) {
            return fail(ErrorCode::InvalidArgument, "not an IP address literal");
        }
        endpoint.v6 = true;
        std::memcpy(endpoint.address.data(), &address, 16);
    } else {
        in_addr address{};
        if (::inet_pton(AF_INET, text.data(), &address) != 1) {
            return fail(ErrorCode::InvalidArgument, "not an IP address literal");
        }
        std::memcpy(endpoint.address.data(), &address, 4);
    }
    return endpoint;
}

std::string format_endpoint(const Endpoint& endpoint) {
    std::array<char, INET6_ADDRSTRLEN> text{};
    if (endpoint.v6) {
        in6_addr address{};
        std::memcpy(&address, endpoint.address.data(), 16);
        ::inet_ntop(AF_INET6, &address, text.data(), text.size());
        return "[" + std::string{text.data()} + "]:" + std::to_string(endpoint.port);
    }
    in_addr address{};
    std::memcpy(&address, endpoint.address.data(), 4);
    ::inet_ntop(AF_INET, &address, text.data(), text.size());
    return std::string{text.data()} + ":" + std::to_string(endpoint.port);
}

socklen_t to_sockaddr(const Endpoint& endpoint, sockaddr_storage& storage) {
    std::memset(&storage, 0, sizeof(storage));
    if (endpoint.v6) {
        sockaddr_in6 v6{};
        v6.sin6_family = AF_INET6;
        v6.sin6_port = htons(endpoint.port);
        std::memcpy(&v6.sin6_addr, endpoint.address.data(), 16);
        std::memcpy(&storage, &v6, sizeof(v6));
        return sizeof(v6);
    }
    sockaddr_in v4{};
    v4.sin_family = AF_INET;
    v4.sin_port = htons(endpoint.port);
    std::memcpy(&v4.sin_addr, endpoint.address.data(), 4);
    std::memcpy(&storage, &v4, sizeof(v4));
    return sizeof(v4);
}

Result<Endpoint> from_sockaddr(const sockaddr_storage& storage) {
    Endpoint endpoint;
    if (storage.ss_family == AF_INET) {
        sockaddr_in v4{};
        std::memcpy(&v4, &storage, sizeof(v4));
        endpoint.port = ntohs(v4.sin_port);
        std::memcpy(endpoint.address.data(), &v4.sin_addr, 4);
        return endpoint;
    }
    if (storage.ss_family == AF_INET6) {
        sockaddr_in6 v6{};
        std::memcpy(&v6, &storage, sizeof(v6));
        endpoint.v6 = true;
        endpoint.port = ntohs(v6.sin6_port);
        std::memcpy(endpoint.address.data(), &v6.sin6_addr, 16);
        return endpoint;
    }
    return fail(ErrorCode::InvalidArgument, "not an IP socket address");
}

}  // namespace wsrv

// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "net/endpoint.hpp"
#include "net/fd.hpp"

#include <cstring>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace wsrv::test {

/// A blocking client socket connected to `endpoint` (loopback in tests). Returns an invalid Fd on
/// failure. The kernel completes the handshake for a listening socket without accept() being
/// called, so connecting from the loop thread before the loop runs is fine.
inline Fd connect_to(const Endpoint& endpoint) {
    const int family = endpoint.v6 ? AF_INET6 : AF_INET;
    Fd fd{::socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (!fd.valid()) {
        return Fd{};
    }
    sockaddr_storage storage{};
    socklen_t length = 0;
    if (endpoint.v6) {
        auto* v6 = reinterpret_cast<sockaddr_in6*>(&storage);
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(endpoint.port);
        std::memcpy(&v6->sin6_addr, endpoint.address.data(), 16);
        length = sizeof(sockaddr_in6);
    } else {
        auto* v4 = reinterpret_cast<sockaddr_in*>(&storage);
        v4->sin_family = AF_INET;
        v4->sin_port = htons(endpoint.port);
        std::memcpy(&v4->sin_addr, endpoint.address.data(), 4);
        length = sizeof(sockaddr_in);
    }
    if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&storage), length) != 0) {
        return Fd{};
    }
    return fd;
}

}  // namespace wsrv::test

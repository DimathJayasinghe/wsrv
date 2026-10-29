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
    const socklen_t length = to_sockaddr(endpoint, storage);
    if (::connect(fd.get(), as_sockaddr(storage), length) != 0) {
        return Fd{};
    }
    return fd;
}

}  // namespace wsrv::test

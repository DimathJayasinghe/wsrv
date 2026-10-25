// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "net/endpoint.hpp"
#include "net/fd.hpp"
#include "net/syscalls.hpp"

namespace wsrv {

struct ListenerConfig {
    Endpoint endpoint;     ///< port 0 asks the kernel for a free one (see local_endpoint())
    int backlog = 511;     ///< kernel accept queue; the effective value is capped by net.core.somaxconn
    bool reuse_port = false;  ///< SO_REUSEPORT: several listeners (one per loop) share the port
};

/// A bound, listening, non-blocking, close-on-exec TCP socket. SO_REUSEADDR is always set (a
/// restart does not wait out TIME_WAIT); IPv6 sockets are IPV6_V6ONLY so the behaviour does not
/// depend on a sysctl: bind "0.0.0.0" and "::" separately to serve both families.
/// Thread-affinity: loop-thread-only.
class TcpListener {
public:
    /// @return Error{AddressInUse} if the port is taken, Error{FdLimit} if out of descriptors,
    /// Error{Io} for other failures (a permission problem on a privileged port, for instance).
    [[nodiscard]] static Result<TcpListener> open(ISyscalls& sys, const ListenerConfig& config);

    [[nodiscard]] int fd() const noexcept {
        return fd_.get();
    }
    /// The address actually bound (with the real port when 0 was requested).
    [[nodiscard]] const Endpoint& local_endpoint() const noexcept {
        return local_;
    }

private:
    TcpListener(Fd fd, Endpoint local) noexcept : fd_{std::move(fd)}, local_{local} {}

    Fd fd_;
    Endpoint local_;
};

}  // namespace wsrv

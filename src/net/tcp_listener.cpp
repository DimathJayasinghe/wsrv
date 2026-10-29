// SPDX-License-Identifier: Apache-2.0
#include "net/tcp_listener.hpp"

#include <netinet/in.h>

namespace wsrv {

namespace {

[[nodiscard]] Result<void> set_flag(ISyscalls& sys, int fd, SockOpt option, const char* context) {
    const int on = 1;
    const Result<long> done =
        retrying([&] { return sys.setsockopt(fd, option, &on, sizeof(on)); }, context);
    if (!done) {
        return std::unexpected{done.error()};
    }
    return {};
}

/// SO_REUSEADDR always (restarts do not wait out TIME_WAIT), SO_REUSEPORT on request, and
/// IPV6_V6ONLY on IPv6 sockets so the behaviour does not depend on a sysctl.
[[nodiscard]] Result<void> configure(ISyscalls& sys, int fd, const ListenerConfig& config) {
    if (const Result<void> r = set_flag(sys, fd, {SOL_SOCKET, SO_REUSEADDR}, "SO_REUSEADDR"); !r) {
        return r;
    }
    if (config.reuse_port) {
        if (const Result<void> r = set_flag(sys, fd, {SOL_SOCKET, SO_REUSEPORT}, "SO_REUSEPORT");
            !r) {
            return r;
        }
    }
    if (config.endpoint.v6) {
        return set_flag(sys, fd, {IPPROTO_IPV6, IPV6_V6ONLY}, "IPV6_V6ONLY");
    }
    return {};
}

[[nodiscard]] Result<void> bind_and_listen(ISyscalls& sys, int fd, const ListenerConfig& config) {
    sockaddr_storage address{};
    const socklen_t length = to_sockaddr(config.endpoint, address);
    const Result<long> bound =
        retrying([&] { return sys.bind(fd, as_sockaddr(address), length); }, "bind");
    if (!bound) {
        return std::unexpected{bound.error()};
    }
    const Result<long> listening =
        retrying([&] { return sys.listen(fd, config.backlog); }, "listen");
    if (!listening) {
        return std::unexpected{listening.error()};
    }
    return {};
}

}  // namespace

Result<TcpListener> TcpListener::open(ISyscalls& sys, const ListenerConfig& config) {
    const int family = config.endpoint.v6 ? AF_INET6 : AF_INET;
    const Result<long> created =
        retrying([&] { return sys.socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0); },
                 "socket");
    if (!created) {
        return std::unexpected{created.error()};
    }
    Fd fd{static_cast<int>(*created)};  // from here on every early return closes it
    if (const Result<void> r = configure(sys, fd.get(), config); !r) {
        return std::unexpected{r.error()};
    }
    if (const Result<void> r = bind_and_listen(sys, fd.get(), config); !r) {
        return std::unexpected{r.error()};
    }
    sockaddr_storage actual{};
    socklen_t actual_length = sizeof(actual);
    const Result<long> named =
        retrying([&] { return sys.getsockname(fd.get(), as_sockaddr(actual), &actual_length); },
                 "getsockname");
    if (!named) {
        return std::unexpected{named.error()};
    }
    const Result<Endpoint> local = from_sockaddr(actual);
    if (!local) {
        return std::unexpected{local.error()};
    }
    return TcpListener{std::move(fd), *local};
}

}  // namespace wsrv

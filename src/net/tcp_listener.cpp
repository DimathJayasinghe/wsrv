// SPDX-License-Identifier: Apache-2.0
#include "net/tcp_listener.hpp"

#include <netinet/in.h>

namespace wsrv {

namespace {

[[nodiscard]] Result<void> set_flag(ISyscalls& sys, int fd, int level, int name, const char* context) {
    const int on = 1;
    const Result<long> done = retrying([&] { return sys.setsockopt(fd, level, name, &on, sizeof(on)); }, context);
    if (!done) {
        return std::unexpected{done.error()};
    }
    return {};
}

}  // namespace

Result<TcpListener> TcpListener::open(ISyscalls& sys, const ListenerConfig& config) {
    const int family = config.endpoint.v6 ? AF_INET6 : AF_INET;
    const Result<long> created = retrying(
        [&] { return sys.socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0); }, "socket");
    if (!created) {
        return std::unexpected{created.error()};
    }
    Fd fd{static_cast<int>(*created)};  // from here on every early return closes it

    if (const Result<void> r = set_flag(sys, fd.get(), SOL_SOCKET, SO_REUSEADDR, "SO_REUSEADDR"); !r) {
        return std::unexpected{r.error()};
    }
    if (config.reuse_port) {
        if (const Result<void> r = set_flag(sys, fd.get(), SOL_SOCKET, SO_REUSEPORT, "SO_REUSEPORT"); !r) {
            return std::unexpected{r.error()};
        }
    }
    if (config.endpoint.v6) {
        if (const Result<void> r = set_flag(sys, fd.get(), IPPROTO_IPV6, IPV6_V6ONLY, "IPV6_V6ONLY"); !r) {
            return std::unexpected{r.error()};
        }
    }
    sockaddr_storage address{};
    const socklen_t length = to_sockaddr(config.endpoint, address);
    const Result<long> bound = retrying(
        [&] { return sys.bind(fd.get(), reinterpret_cast<const sockaddr*>(&address), length); }, "bind");
    if (!bound) {
        return std::unexpected{bound.error()};
    }
    const Result<long> listening = retrying([&] { return sys.listen(fd.get(), config.backlog); }, "listen");
    if (!listening) {
        return std::unexpected{listening.error()};
    }
    sockaddr_storage actual{};
    socklen_t actual_length = sizeof(actual);
    const Result<long> named = retrying(
        [&] { return sys.getsockname(fd.get(), reinterpret_cast<sockaddr*>(&actual), &actual_length); },
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

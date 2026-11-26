// SPDX-License-Identifier: Apache-2.0
#include "wsrvd/healthcheck.hpp"

#include "net/endpoint.hpp"
#include "net/fd.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <string_view>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>

namespace wsrv::wsrvd {

namespace {

using Clock = std::chrono::steady_clock;

/// Wait until `fd` is ready for `events` or the deadline passes. @return true if ready.
[[nodiscard]] bool wait_for(int fd, short events, Clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    if (left.count() < 0) {
        return false;
    }
    pollfd entry{fd, events, 0};
    const int ready = ::poll(&entry, 1, static_cast<int>(left.count()));
    return ready > 0 && (entry.revents & (events | POLLERR | POLLHUP)) != 0;
}

/// Finish a non-blocking connect: true if it succeeded within the deadline.
[[nodiscard]] bool connected(Fd& socket, const sockaddr* address, socklen_t length, Clock::time_point deadline) {
    if (::connect(socket.get(), address, length) == 0) {
        return true;
    }
    if (errno != EINPROGRESS || !wait_for(socket.get(), POLLOUT, deadline)) {
        return false;
    }
    int error = 0;
    socklen_t size = sizeof(error);
    return ::getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0;
}

/// An unspecified listen address is probed on loopback of the same family.
[[nodiscard]] Endpoint probe_address(Endpoint endpoint) {
    const std::size_t used = endpoint.v6 ? 16 : 4;
    auto* const end = endpoint.address.data() + static_cast<std::ptrdiff_t>(used);
    if (std::all_of(endpoint.address.data(), end, [](std::uint8_t b) { return b == 0; })) {
        endpoint.address = {};
        if (endpoint.v6) {
            endpoint.address[15] = 1;
        } else {
            endpoint.address[0] = 127;
            endpoint.address[3] = 1;
        }
    }
    return endpoint;
}

[[nodiscard]] Fd open_tcp(const std::string& host, std::uint16_t port, Clock::time_point deadline) {
    const Result<Endpoint> parsed = parse_endpoint(host, port);
    if (!parsed) {
        return Fd{};
    }
    const Endpoint endpoint = probe_address(*parsed);
    sockaddr_storage storage{};
    const socklen_t length = to_sockaddr(endpoint, storage);
    Fd socket{::socket(endpoint.v6 ? AF_INET6 : AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
    if (!socket.valid() || !connected(socket, as_sockaddr(storage), length, deadline)) {
        return Fd{};
    }
    return socket;
}

[[nodiscard]] Fd open_unix(const std::string& path, Clock::time_point deadline) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof(address.sun_path)) {
        return Fd{};
    }
    std::memcpy(address.sun_path, path.data(), path.size());
    Fd socket{::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
    // sockaddr_un is defined to be passed as a sockaddr.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* generic = reinterpret_cast<const sockaddr*>(&address);
    if (!socket.valid() || !connected(socket, generic, sizeof(address), deadline)) {
        return Fd{};
    }
    return socket;
}

/// Send the whole (tiny) request on a fresh socket: its send buffer is empty, so it always fits.
[[nodiscard]] bool send_all(const Fd& socket, std::string_view bytes) {
    return ::send(socket.get(), bytes.data(), bytes.size(), MSG_NOSIGNAL) ==
           static_cast<ssize_t>(bytes.size());
}

/// The first bytes of the answer, up to `buffer.size()`, or fewer at EOF or the deadline.
[[nodiscard]] std::size_t read_prefix(const Fd& socket, std::array<char, 12>& buffer, Clock::time_point deadline) {
    std::size_t have = 0;
    while (have < buffer.size() && wait_for(socket.get(), POLLIN, deadline)) {
        const ssize_t got = ::recv(socket.get(), buffer.data() + have, buffer.size() - have, 0);
        if (got <= 0) {
            break;
        }
        have += static_cast<std::size_t>(got);
    }
    return have;
}

[[nodiscard]] Result<void> check_healthz(const HealthTarget& target, Clock::time_point deadline) {
    const Fd socket = open_tcp(target.admin_host, target.admin_port, deadline);
    constexpr std::string_view kRequest = "GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    if (!socket.valid() || !send_all(socket, kRequest)) {
        return fail(ErrorCode::Io, "healthcheck: the admin port does not answer");
    }
    std::array<char, 12> answer{};
    const std::size_t have = read_prefix(socket, answer, deadline);
    if (std::string_view{answer.data(), have} != "HTTP/1.1 200") {
        return fail(ErrorCode::Io, "healthcheck: /healthz did not answer 200");
    }
    return {};
}

[[nodiscard]] Result<void> check_ports(const HealthTarget& target, Clock::time_point deadline) {
    if (target.ws_port == 0) {
        return fail(ErrorCode::InvalidConfig, "healthcheck: set listen_port or admin_port, the kernel-chosen port is unknown");
    }
    if (!open_tcp(target.ws_host, target.ws_port, deadline).valid()) {
        return fail(ErrorCode::Io, "healthcheck: the websocket port does not accept connections");
    }
    if (!open_unix(target.ipc_socket, deadline).valid()) {
        return fail(ErrorCode::Io, "healthcheck: the ipc socket does not accept connections");
    }
    return {};
}

}  // namespace

HealthTarget health_target(const DaemonConfig& config) {
    const EmbeddedConfig& embedded = config.embedded();
    HealthTarget target;
    target.ws_host = embedded.host();
    target.ws_port = embedded.port();
    target.ipc_socket = config.socket_path();
    target.admin_host = embedded.admin_host();
    target.admin_port = embedded.admin_port();
    return target;
}

Result<void> check_health(const HealthTarget& target, std::chrono::milliseconds deadline) {
    const Clock::time_point limit = Clock::now() + deadline;
    if (target.admin_port != 0) {
        return check_healthz(target, limit);
    }
    return check_ports(target, limit);
}

}  // namespace wsrv::wsrvd

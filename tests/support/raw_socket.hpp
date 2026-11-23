// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace wsrv::test {

/// A plain blocking loopback client for hostile-input tests: it sends exactly the bytes it is told
/// to (no HTTP library to tidy them up) and reads what comes back.
class RawSocket {
public:
    explicit RawSocket(std::uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        connected_ = ::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    }
    RawSocket(const RawSocket&) = delete;
    RawSocket& operator=(const RawSocket&) = delete;
    ~RawSocket() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    [[nodiscard]] bool connected() const {
        return connected_;
    }
    void send(const std::string& bytes) const {
        static_cast<void>(::send(fd_, bytes.data(), bytes.size(), MSG_NOSIGNAL));
    }
    /// Everything until the server closes; `closed` tells whether it did within `limit`.
    std::string read_all(bool& closed,
                         std::chrono::milliseconds limit = std::chrono::seconds{3}) const {
        std::string out;
        const auto deadline = std::chrono::steady_clock::now() + limit;
        closed = false;
        while (std::chrono::steady_clock::now() < deadline) {
            pollfd entry{fd_, POLLIN, 0};
            if (::poll(&entry, 1, 50) <= 0) {
                continue;
            }
            std::array<char, 4096> buffer{};
            const ssize_t got = ::recv(fd_, buffer.data(), buffer.size(), 0);
            if (got <= 0) {
                closed = true;
                break;
            }
            out.append(buffer.data(), static_cast<std::size_t>(got));
        }
        return out;
    }

private:
    int fd_ = -1;
    bool connected_ = false;
};

}  // namespace wsrv::test

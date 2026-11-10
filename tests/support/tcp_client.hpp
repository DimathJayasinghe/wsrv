// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "net/endpoint.hpp"
#include "net/fd.hpp"
#include "protocol/frame.hpp"
#include "support/wire.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace wsrv::test {

/// A blocking TCP test client with read timeouts: just enough to talk to a real server over
/// loopback, handshake and frames included. Not part of the product.
class TcpClient {
public:
    static constexpr std::chrono::milliseconds kTimeout{5000};

    /// Connect to 127.0.0.1:`port`. valid() is false if it failed.
    explicit TcpClient(std::uint16_t port) {
        fd_.reset(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (!fd_.valid() ||
            ::connect(fd_.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
            fd_.reset();
        }
    }

    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;
    TcpClient(TcpClient&&) noexcept = default;
    TcpClient& operator=(TcpClient&&) noexcept = default;
    virtual ~TcpClient() = default;

    [[nodiscard]] bool valid() const {
        return fd_.valid();
    }

    bool send(std::string_view bytes) const {
        return send(as_bytes(bytes));
    }
    virtual bool send(ByteView bytes) const {
        std::size_t done = 0;
        while (done < bytes.size()) {
            const ssize_t n =
                ::send(fd_.get(), bytes.data() + done, bytes.size() - done, MSG_NOSIGNAL);
            if (n <= 0) {
                return false;
            }
            done += static_cast<std::size_t>(n);
        }
        return true;
    }
    /// `rsv1` marks a permessage-deflate message (first frame only); the payload is sent as given.
    bool
    send_frame(Opcode opcode, std::string_view payload, bool fin = true, bool rsv1 = false) const {
        const std::vector<std::byte> frame = client_frame(opcode, fin, payload, rsv1);
        return send(ByteView{frame});
    }

    /// Read until the end of the HTTP response head; "" on timeout or EOF before it.
    std::string read_http_head() {
        while (buffer_.find("\r\n\r\n") == std::string::npos) {
            if (!fill()) {
                return {};
            }
        }
        const std::size_t end = buffer_.find("\r\n\r\n") + 4;
        std::string head = buffer_.substr(0, end);
        buffer_.erase(0, end);
        return head;
    }

    /// True if nothing at all (data or EOF) arrives within `period`: used to show that a server
    /// is deliberately keeping quiet.
    [[nodiscard]] virtual bool silent_for(std::chrono::milliseconds period) const {
        return !readable(period);
    }

    /// The standard upgrade request, without sending it. `extra_headers` are whole header lines,
    /// each ending in CRLF (for example a Sec-WebSocket-Extensions offer).
    static std::string upgrade_request(std::string_view origin = "https://app.example.com",
                                       std::string_view extra_headers = {}) {
        std::string request =
            "GET /chat HTTP/1.1\r\nHost: localhost\r\nConnection: Upgrade\r\n"
            "Upgrade: websocket\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n";
        if (!origin.empty()) {
            request += "Origin: ";
            request += origin;
            request += "\r\n";
        }
        request += extra_headers;
        request += "\r\n";
        return request;
    }

    /// Handshake with a standard request; true if the answer is 101.
    bool upgrade(std::string_view origin = "https://app.example.com") {
        return send(upgrade_request(origin)) && read_http_head().rfind("HTTP/1.1 101 ", 0) == 0;
    }

    /// Handshake with extra request headers; the whole response head ("" if nothing came back).
    /// Tests assert on the status line and on headers such as Sec-WebSocket-Extensions.
    std::string upgrade_head(std::string_view extra_headers,
                             std::string_view origin = "https://app.example.com") {
        if (!send(upgrade_request(origin, extra_headers))) {
            return {};
        }
        return read_http_head();
    }

    /// One server frame (unmasked), or nullopt on timeout/EOF/garbage.
    std::optional<WireFrame> read_frame() {
        if (!fill_to(2)) {
            return std::nullopt;
        }
        const auto first = static_cast<std::uint8_t>(buffer_[0]);
        const auto second = static_cast<std::uint8_t>(buffer_[1]);
        std::size_t header = 2;
        std::uint64_t length = second & 0x7FU;
        if (length == 126 || length == 127) {
            const std::size_t extra = length == 126 ? 2 : 8;
            if (!fill_to(2 + extra)) {
                return std::nullopt;
            }
            length = 0;
            for (std::size_t i = 0; i < extra; ++i) {
                length = (length << 8U) | static_cast<std::uint8_t>(buffer_[2 + i]);
            }
            header += extra;
        }
        if (!fill_to(header + length)) {
            return std::nullopt;
        }
        WireFrame frame{static_cast<Opcode>(first & 0x0FU),
                        (first & 0x80U) != 0,
                        buffer_.substr(header, length),
                        (first & 0x40U) != 0};  // RSV1: a compressed message (RFC 7692)
        buffer_.erase(0, header + length);
        return frame;
    }

    /// True once the server has closed the connection (EOF), false on timeout.
    virtual bool wait_for_eof() {
        for (;;) {
            std::array<char, 4096> chunk{};
            if (!readable(kTimeout)) {
                return false;
            }
            const ssize_t n = ::recv(fd_.get(), chunk.data(), chunk.size(), 0);
            if (n == 0) {
                return true;
            }
            if (n < 0 && errno != EINTR) {
                return true;  // reset counts as closed
            }
        }
    }

    void close() {
        fd_.reset();
    }

protected:
    // The seams a TLS client (support/tls_client.hpp) overrides: how bytes leave and arrive.
    bool readable(std::chrono::milliseconds timeout) const {
        pollfd p{fd_.get(), POLLIN, 0};
        return ::poll(&p, 1, static_cast<int>(timeout.count())) > 0;
    }
    virtual bool fill() {
        if (!readable(kTimeout)) {
            return false;
        }
        std::array<char, 4096> chunk{};
        const ssize_t n = ::recv(fd_.get(), chunk.data(), chunk.size(), 0);
        if (n <= 0) {
            return false;
        }
        buffer_.append(chunk.data(), static_cast<std::size_t>(n));
        return true;
    }
    [[nodiscard]] int raw_fd() const {
        return fd_.get();
    }
    void append(const char* data, std::size_t size) {
        buffer_.append(data, size);
    }

private:
    bool fill_to(std::size_t size) {
        while (buffer_.size() < size) {
            if (!fill()) {
                return false;
            }
        }
        return true;
    }

    Fd fd_;
    std::string buffer_;
};

}  // namespace wsrv::test

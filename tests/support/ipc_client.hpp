// SPDX-License-Identifier: Apache-2.0
#pragma once

// A small, dependency-light C++ client for the wsrvd IPC protocol (docs/IPC_PROTOCOL.md): blocking,
// with a receive timeout so a misbehaving daemon fails a test instead of hanging it. It is the C++
// counterpart of bindings/php (Client) and is used by the wsrvd tests; copy it as the starting
// point for a test of your own application against a real daemon. Not part of the product.

#include "base/byte_buffer.hpp"
#include "base/bytes.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "ipc/frame_decoder.hpp"
#include "ipc/message.hpp"

namespace wsrv::test {

namespace fs = std::filesystem;
namespace ipc = wsrv::ipc;
/// A private directory removed with its contents.
class TempDir {
public:
    TempDir() {
        std::string pattern = (fs::temp_directory_path() / "wsrv-ipcd-XXXXXX").string();
        EXPECT_NE(::mkdtemp(pattern.data()), nullptr);
        path_ = pattern;
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    [[nodiscard]] std::string file(const std::string& name) const {
        return path_ + "/" + name;
    }

private:
    std::string path_;
};

/// What a test cares about in a reply.
struct Reply {
    enum class Kind : std::uint8_t { HelloOk, Status, Pong, Event, Other } kind = Kind::Other;
    std::uint32_t request = 0;
    std::uint8_t status = 0;
    std::uint32_t max_frame = 0;
    // EVENT frames
    ipc::EventKind event = ipc::EventKind::Open;
    std::uint64_t conn = 0;
    std::uint16_t close_code = 0;
    bool clean = false;
    std::string data;
};

inline const sockaddr* as_generic(const sockaddr_un& address) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<const sockaddr*>(&address);
}

/// A blocking client with a receive timeout, so a server that misbehaves fails a test instead of
/// hanging it.
class Client {
public:
    explicit Client(const std::string& path,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds{3000})
        : fd_{::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)}, decoder_{1U << 20U} {
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        connected_ = ::connect(fd_, as_generic(address), sizeof(address)) == 0;
        timeval tv{};
        tv.tv_sec = static_cast<time_t>(timeout.count() / 1000);
        tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    /// The same over TCP (ADR-0038): `host` is an IPv4 or IPv6 literal.
    struct Tcp {};
    Client(Tcp /*tag*/,
           const std::string& host,
           std::uint16_t port,
           std::chrono::milliseconds timeout = std::chrono::milliseconds{3000})
        : fd_{-1}, decoder_{1U << 20U} {
        sockaddr_storage storage{};
        socklen_t length = 0;
        const bool v6 = host.find(':') != std::string::npos;
        if (v6) {
            auto* address = reinterpret_cast<sockaddr_in6*>(&storage);  // NOLINT
            address->sin6_family = AF_INET6;
            address->sin6_port = htons(port);
            ::inet_pton(AF_INET6, host.c_str(), &address->sin6_addr);
            length = sizeof(sockaddr_in6);
        } else {
            auto* address = reinterpret_cast<sockaddr_in*>(&storage);  // NOLINT
            address->sin_family = AF_INET;
            address->sin_port = htons(port);
            ::inet_pton(AF_INET, host.c_str(), &address->sin_addr);
            length = sizeof(sockaddr_in);
        }
        fd_ = ::socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        connected_ =
            ::connect(fd_, reinterpret_cast<const sockaddr*>(&storage), length) == 0;  // NOLINT
        timeval tv{};
        tv.tv_sec = static_cast<time_t>(timeout.count() / 1000);
        tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    ~Client() {
        ::close(fd_);
    }
    [[nodiscard]] bool connected() const {
        return connected_;
    }

    void send_raw(const std::vector<std::byte>& bytes) const {
        ASSERT_EQ(::send(fd_, bytes.data(), bytes.size(), MSG_NOSIGNAL),
                  static_cast<ssize_t>(bytes.size()));
    }
    void send_message(const ipc::Message& message) const {
        send_raw(encode(message));
    }
    /// For a peer the server is expected to drop: it may already have closed by the time we send,
    /// and that is a valid outcome, so a failed send is not a test failure.
    void send_hello_if_still_open() const {
        const std::vector<std::byte> bytes = encode(ipc::Hello{});
        (void)::send(fd_, bytes.data(), bytes.size(), MSG_NOSIGNAL);
    }
    void send_hello(std::string_view token = "") const {
        ipc::Hello hello;
        hello.token = ByteView{reinterpret_cast<const std::byte*>(token.data()),  // NOLINT
                               token.size()};
        send_message(hello);
    }

    /// The next frame, or nothing on EOF / timeout.
    std::optional<Reply> receive() {
        for (;;) {
            ByteView view{pending_};
            const auto before = view.size();
            auto next = decoder_.next(view);
            const auto consumed = before - view.size();
            if (!next) {
                return std::nullopt;
            }
            if (next->has_value()) {
                const Reply reply = to_reply(**next);
                pending_.erase(pending_.begin(),
                               pending_.begin() + static_cast<std::ptrdiff_t>(consumed));
                return reply;
            }
            pending_.erase(pending_.begin(),
                           pending_.begin() + static_cast<std::ptrdiff_t>(consumed));
            std::array<std::byte, 512> buffer{};
            const ssize_t n = ::recv(fd_, buffer.data(), buffer.size(), 0);
            if (n <= 0) {
                return std::nullopt;
            }
            pending_.insert(pending_.end(), buffer.begin(), buffer.begin() + n);
        }
    }

    /// After the server has said its last word and half-closed, does it still hold the connection?
    /// A server that fully closed while our bytes were unread, or that gets ours after it closed,
    /// answers the first write with a reset, and the second write then fails (EPIPE). A server that
    /// lingers (reads and discards until we close) accepts both. Over TCP this is the difference
    /// between a status the peer can still read and one a reset may wipe (ADR-0038 section 5).
    [[nodiscard]] bool server_still_holds_connection(
        std::chrono::milliseconds settle = std::chrono::milliseconds{250}) const {
        const std::array<std::byte, 16> junk{};
        if (::send(fd_, junk.data(), junk.size(), MSG_NOSIGNAL) < 0) {
            return false;
        }
        std::this_thread::sleep_for(settle);  // time for a reset to come back, if one will
        return ::send(fd_, junk.data(), junk.size(), MSG_NOSIGNAL) >= 0;
    }

    /// True if the server closes the connection (EOF) without sending anything more.
    [[nodiscard]] bool closed_by_server() const {
        std::array<std::byte, 64> buffer{};
        for (;;) {
            const ssize_t n = ::recv(fd_, buffer.data(), buffer.size(), 0);
            if (n == 0) {
                return true;
            }
            if (n < 0) {
                return errno == ECONNRESET;  // a reset is a close too; a timeout is not
            }
            return false;  // data arrived: it was not silent
        }
    }

    static std::vector<std::byte> encode(const ipc::Message& message) {
        wsrv::ByteBuffer out{1U << 20U};
        EXPECT_TRUE(ipc::encode(message, 1U << 20U, out).has_value());
        const ByteView view = out.view();
        return {view.begin(), view.end()};
    }

private:
    static Reply to_reply(const ipc::Message& message) {
        Reply reply;
        if (const auto* ok = std::get_if<ipc::HelloOk>(&message)) {
            reply.kind = Reply::Kind::HelloOk;
            reply.max_frame = ok->max_frame;
        } else if (const auto* status = std::get_if<ipc::Status>(&message)) {
            reply.kind = Reply::Kind::Status;
            reply.request = status->request;
            reply.status = status->status;
        } else if (std::holds_alternative<ipc::Pong>(message)) {
            reply.kind = Reply::Kind::Pong;
        } else if (const auto* event = std::get_if<ipc::Event>(&message)) {
            reply.kind = Reply::Kind::Event;
            reply.event = event->kind;
            reply.conn = event->conn;
            reply.close_code = event->close_code;
            reply.clean = event->clean;
            reply.data = std::string{wsrv::as_chars(event->data)};
        }
        return reply;
    }

    int fd_;
    bool connected_ = false;
    ipc::FrameDecoder decoder_;
    std::vector<std::byte> pending_;
};

/// A frame header with an arbitrary declared length: `u32 length, u8 type, u8 flags`.
inline std::vector<std::byte>
header(std::uint32_t length, std::uint8_t type, std::uint8_t flags = 0) {
    return {std::byte(length >> 24U),
            std::byte((length >> 16U) & 0xFFU),
            std::byte((length >> 8U) & 0xFFU),
            std::byte(length & 0xFFU),
            std::byte(type),
            std::byte(flags)};
}

}  // namespace wsrv::test

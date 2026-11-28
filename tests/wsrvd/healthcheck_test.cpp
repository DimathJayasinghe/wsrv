// SPDX-License-Identifier: Apache-2.0
// wsrvd --healthcheck (ADR-0030): what "healthy" means and that every failure is a bounded,
// explained "no". Real loopback and Unix sockets; no daemon is started.
#include "wsrvd/healthcheck.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "capi/embedded_config.hpp"
#include "capi/embedded_server.hpp"

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using wsrv::wsrvd::check_health;
using wsrv::wsrvd::HealthTarget;

/// A listening loopback TCP socket on an ephemeral port. Optionally answers every connection with
/// `reply` (read the request first, then write, then close): a stand-in for the admin listener.
class TcpListener {
public:
    explicit TcpListener(std::string reply = {}) : reply_{std::move(reply)} {
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        EXPECT_EQ(::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
        EXPECT_EQ(::listen(fd_, 8), 0);
        socklen_t length = sizeof(addr);
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &length);
        port_ = ntohs(addr.sin_port);
        if (!reply_.empty()) {
            thread_ = std::thread{[this] { serve(); }};
        }
    }
    ~TcpListener() {
        stop_ = true;
        if (thread_.joinable()) {
            ::shutdown(fd_, SHUT_RDWR);
            thread_.join();
        }
        ::close(fd_);
    }
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;
    [[nodiscard]] std::uint16_t port() const {
        return port_;
    }

private:
    void serve() {
        while (!stop_) {
            const int client = ::accept(fd_, nullptr, nullptr);
            if (client < 0) {
                return;
            }
            char request[512];
            static_cast<void>(::recv(client, request, sizeof(request), 0));
            static_cast<void>(::send(client, reply_.data(), reply_.size(), MSG_NOSIGNAL));
            ::close(client);
        }
    }
    int fd_ = -1;
    std::uint16_t port_ = 0;
    std::string reply_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

/// A listening Unix socket at a fresh path (the daemon's IPC socket stand-in).
class UnixListener {
public:
    UnixListener() {
        std::string pattern = (fs::temp_directory_path() / "wsrv-hc-XXXXXX").string();
        EXPECT_NE(::mkdtemp(pattern.data()), nullptr);
        dir_ = pattern;
        path_ = dir_ + "/ipc.sock";
        fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
        EXPECT_EQ(::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
        EXPECT_EQ(::listen(fd_, 8), 0);
    }
    ~UnixListener() {
        ::close(fd_);
        std::error_code ignored;
        fs::remove_all(dir_, ignored);
    }
    UnixListener(const UnixListener&) = delete;
    UnixListener& operator=(const UnixListener&) = delete;
    [[nodiscard]] const std::string& path() const {
        return path_;
    }

private:
    int fd_ = -1;
    std::string dir_;
    std::string path_;
};

constexpr std::string_view kOk = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nok\n";
constexpr std::string_view kNotReady =
    "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n";

/// A port nothing listens on: bind, read the number, close.
std::uint16_t closed_port() {
    TcpListener probe;
    return probe.port();
}

TEST(Healthcheck, WithoutAdminItNeedsTheWebSocketPortAndTheIpcSocket) {
    TcpListener websocket;
    UnixListener ipc;
    HealthTarget target;
    target.ws_host = "127.0.0.1";
    target.ws_port = websocket.port();
    target.ipc_socket = ipc.path();
    EXPECT_TRUE(check_health(target, 2s).has_value());
}

TEST(Healthcheck, ADeadWebSocketPortOrAMissingIpcSocketIsUnhealthy) {
    TcpListener websocket;
    UnixListener ipc;
    HealthTarget no_ws;
    no_ws.ws_host = "127.0.0.1";
    no_ws.ws_port = closed_port();
    no_ws.ipc_socket = ipc.path();
    const auto ws_result = check_health(no_ws, 2s);
    ASSERT_FALSE(ws_result.has_value());
    EXPECT_NE(std::string{ws_result.error().context}.find("websocket"), std::string::npos);

    HealthTarget no_ipc;
    no_ipc.ws_host = "127.0.0.1";
    no_ipc.ws_port = websocket.port();
    no_ipc.ipc_socket = ipc.path() + ".gone";
    const auto ipc_result = check_health(no_ipc, 2s);
    ASSERT_FALSE(ipc_result.has_value());
    EXPECT_NE(std::string{ipc_result.error().context}.find("ipc"), std::string::npos);
}

// ADR-0038 section 6: the TCP IPC port is never probed (a probe would be refused as a second
// application and logged every interval), so a target without an IPC socket only needs the
// websocket port.
TEST(Healthcheck, TheTcpTransportHasNoIpcSocketToProbe) {
    TcpListener websocket;
    HealthTarget target;
    target.ws_host = "127.0.0.1";
    target.ws_port = websocket.port();
    EXPECT_TRUE(target.ipc_socket.empty());
    EXPECT_TRUE(check_health(target, 2s).has_value());
}

TEST(Healthcheck, ATcpConfigurationDescribesNoIpcSocket) {
    wsrv::wsrvd::DaemonConfig config;
    ASSERT_TRUE(config.set("ipc_port", "9300").has_value());
    ASSERT_TRUE(config.set("ipc_token_file", "/run/secrets/ipc.token").has_value());
    ASSERT_TRUE(config.finish().has_value());
    EXPECT_TRUE(wsrv::wsrvd::health_target(config).ipc_socket.empty());
}

TEST(Healthcheck, AnUnspecifiedListenAddressIsProbedOnLoopback) {
    TcpListener websocket;
    UnixListener ipc;
    HealthTarget target;
    target.ws_host = "0.0.0.0";  // what a container binds
    target.ws_port = websocket.port();
    target.ipc_socket = ipc.path();
    EXPECT_TRUE(check_health(target, 2s).has_value());
}

TEST(Healthcheck, APortOfZeroCannotBeChecked) {
    UnixListener ipc;
    HealthTarget target;
    target.ws_host = "127.0.0.1";
    target.ws_port = 0;  // the kernel chose: nobody knows where to look
    target.ipc_socket = ipc.path();
    const auto result = check_health(target, 2s);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, wsrv::ErrorCode::InvalidConfig);
}

TEST(Healthcheck, WithAdminItAsksHealthzAndWantsA200) {
    TcpListener admin{std::string{kOk}};
    HealthTarget target;
    target.admin_host = "127.0.0.1";
    target.admin_port = admin.port();
    EXPECT_TRUE(check_health(target, 2s).has_value());
}

// ADR-0041, the claim the token design rests on: with a token on /metrics, the probe the image and
// the liveness check run still passes, because it asks /healthz and sends no credential. A real
// server, the real probe.
TEST(Healthcheck, ARealAdminListenerThatGuardsMetricsStillAnswersTheProbe) {
    wsrv::EmbeddedConfig config;
    ASSERT_TRUE(config.set("allow_origin", "https://app.example.com").has_value());
    ASSERT_TRUE(config.set("shutdown_grace_ms", "2000").has_value());
    ASSERT_TRUE(config.set("admin_port", "0").has_value());
    ASSERT_TRUE(config.set_admin_token(std::string(48, 't')).has_value());
    auto server = wsrv::EmbeddedServer::create(config);
    ASSERT_TRUE(server.has_value());
    ASSERT_TRUE((*server)->start().has_value());
    HealthTarget target;
    target.admin_host = "127.0.0.1";
    target.admin_port = (*server)->admin_port();
    ASSERT_NE(target.admin_port, 0);
    EXPECT_TRUE(check_health(target, 2s).has_value());
}

TEST(Healthcheck, AnAdminThatSaysNotOkOrNothingIsUnhealthy) {
    {
        TcpListener admin{std::string{kNotReady}};
        HealthTarget target;
        target.admin_host = "127.0.0.1";
        target.admin_port = admin.port();
        const auto result = check_health(target, 2s);
        ASSERT_FALSE(result.has_value());
        EXPECT_NE(std::string{result.error().context}.find("healthz"), std::string::npos);
    }
    HealthTarget gone;
    gone.admin_host = "127.0.0.1";
    gone.admin_port = closed_port();
    EXPECT_FALSE(check_health(gone, 2s).has_value());
}

TEST(Healthcheck, ASilentPeerIsGivenUpOnAtTheDeadline) {
    TcpListener silent;  // accepts (kernel backlog) but never answers
    HealthTarget target;
    target.admin_host = "127.0.0.1";
    target.admin_port = silent.port();
    const auto started = std::chrono::steady_clock::now();
    const auto result = check_health(target, 300ms);
    const auto took = std::chrono::steady_clock::now() - started;
    EXPECT_FALSE(result.has_value());
    EXPECT_LT(took, 3s) << "bounded by the deadline, not by the peer";
}

TEST(Healthcheck, AGarbageAnswerIsUnhealthyAndAHugeOneIsNotBuffered) {
    TcpListener garbage{std::string(1 << 16, 'x')};
    HealthTarget target;
    target.admin_host = "127.0.0.1";
    target.admin_port = garbage.port();
    EXPECT_FALSE(check_health(target, 2s).has_value());
}

}  // namespace

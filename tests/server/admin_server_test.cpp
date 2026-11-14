// SPDX-License-Identifier: Apache-2.0
// The admin listener on a running server (ADR-0023 section 3): the three pages over real
// sockets, and the bounds that keep it from being a weak spot: a few clients at once, a deadline
// for a request that never ends, a size limit for its head, and off unless asked for.
#include "server/server_builder.hpp"
#include "support/running_server.hpp"
#include "support/tcp_client.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;
using wsrv::test::RunningServer;
using wsrv::test::TcpClient;

class SilentFactory final : public wsrv::IHandlerFactory {
public:
    std::unique_ptr<wsrv::IWebSocketHandler> create(const wsrv::ConnectionInfo& /*info*/) override {
        return std::make_unique<wsrv::IWebSocketHandler>();
    }
};

wsrv::ServerBuilder with_admin(SilentFactory& factory) {
    wsrv::ServerBuilder builder;
    builder.listen("127.0.0.1", 0).handler_factory(factory).admin("127.0.0.1", 0);
    builder.config().handshake.allowed_origins = {"https://app.example.com"};
    builder.config().max_connections_per_ip = 0;
    builder.config().max_new_connections_per_ip_per_second = 0;
    return builder;
}

/// A plain blocking client: connect, optionally send, read until the server closes (or `limit`).
class Raw {
public:
    explicit Raw(std::uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        connected_ = ::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    }
    Raw(const Raw&) = delete;
    Raw& operator=(const Raw&) = delete;
    ~Raw() {
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
    std::string read_all(bool& closed, std::chrono::milliseconds limit = 3s) const {
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

std::string get(std::uint16_t port, const std::string& request) {
    Raw client{port};
    EXPECT_TRUE(client.connected());
    client.send(request);
    bool closed = false;
    const std::string response = client.read_all(closed);
    EXPECT_TRUE(closed) << "the admin answer ends with a close";
    return response;
}

TEST(AdminServer, MetricsHealthzAndReadyzAnswerOverHttp) {
    SilentFactory factory;
    RunningServer running{with_admin(factory)};
    const std::uint16_t port = running.server().admin_endpoint()->port;
    ASSERT_NE(port, 0);
    TcpClient websocket{running.port()};
    ASSERT_TRUE(websocket.upgrade());

    const std::string metrics = get(port, "GET /metrics HTTP/1.1\r\nHost: x\r\n\r\n");
    EXPECT_EQ(metrics.rfind("HTTP/1.1 200 OK\r\n", 0), 0U) << metrics;
    EXPECT_NE(metrics.find("wsrv_connections_current 1\n"), std::string::npos) << metrics;
    EXPECT_NE(metrics.find("wsrv_connections_total{result=\"accepted\"} 1\n"), std::string::npos);
    EXPECT_EQ(get(port, "GET /healthz HTTP/1.1\r\n\r\n").rfind("HTTP/1.1 200 OK\r\n", 0), 0U);
    EXPECT_EQ(get(port, "GET /readyz HTTP/1.1\r\n\r\n").rfind("HTTP/1.1 200 OK\r\n", 0), 0U);
    EXPECT_EQ(get(port, "GET /nope HTTP/1.1\r\n\r\n").rfind("HTTP/1.1 404 ", 0), 0U);
}

TEST(AdminServer, TheAdminPortDoesNotSpeakWebSocketAndTheWebSocketPortDoesNotServeMetrics) {
    SilentFactory factory;
    RunningServer running{with_admin(factory)};
    const std::uint16_t admin_port = running.server().admin_endpoint()->port;
    const std::string upgrade = get(admin_port,
                                    "GET /metrics HTTP/1.1\r\nUpgrade: websocket\r\n"
                                    "Connection: Upgrade\r\n\r\n");
    EXPECT_EQ(upgrade.rfind("HTTP/1.1 200 OK\r\n", 0), 0U) << "plain page, no 101";
    EXPECT_EQ(upgrade.find("101"), std::string::npos);
    Raw other{running.port()};
    other.send("GET /metrics HTTP/1.1\r\nHost: x\r\n\r\n");
    bool closed = false;
    const std::string answer = other.read_all(closed);
    EXPECT_EQ(answer.find("wsrv_connections"), std::string::npos)
        << "metrics only on the admin port";
}

TEST(AdminServer, ItIsOffUnlessAskedFor) {
    SilentFactory factory;
    wsrv::ServerBuilder builder;
    builder.listen("127.0.0.1", 0).handler_factory(factory);
    RunningServer running{builder};
    EXPECT_FALSE(running.server().admin_endpoint().has_value());
}

TEST(AdminServer, ReadyzGoesTo503WhenShutdownBeginsButLivenessStays) {
    SilentFactory factory;
    wsrv::ServerBuilder builder = with_admin(factory);
    builder.config().shutdown_grace = 3s;
    RunningServer running{builder};
    const std::uint16_t port = running.server().admin_endpoint()->port;
    TcpClient websocket{running.port()};
    ASSERT_TRUE(websocket.upgrade());  // keeps the server in its grace period
    ASSERT_TRUE(running.server().request_shutdown().has_value());
    bool not_ready = false;
    for (int i = 0; i < 100 && !not_ready; ++i) {
        not_ready = get(port, "GET /readyz HTTP/1.1\r\n\r\n").rfind("HTTP/1.1 503 ", 0) == 0;
        if (!not_ready) {
            std::this_thread::sleep_for(20ms);
        }
    }
    EXPECT_TRUE(not_ready);
    EXPECT_EQ(get(port, "GET /healthz HTTP/1.1\r\n\r\n").rfind("HTTP/1.1 200 OK\r\n", 0), 0U);
}

TEST(AdminServer, ARequestThatNeverEndsIsDroppedAtTheDeadline) {  // T2 on the admin port
    SilentFactory factory;
    wsrv::ServerBuilder builder = with_admin(factory);
    builder.config().admin_request_timeout = 300ms;
    RunningServer running{builder};
    Raw slow{running.server().admin_endpoint()->port};
    ASSERT_TRUE(slow.connected());
    slow.send("GET /metrics HTTP/1.1\r\nHost: x\r\n");  // no blank line, ever
    bool closed = false;
    const std::string rest = slow.read_all(closed, 3s);
    EXPECT_TRUE(closed);
    EXPECT_TRUE(rest.empty()) << "no answer for half a request";
}

TEST(AdminServer, AnOversizedHeadIsAnswered400AndClosed) {
    SilentFactory factory;
    RunningServer running{with_admin(factory)};
    Raw big{running.server().admin_endpoint()->port};
    ASSERT_TRUE(big.connected());
    big.send("GET /metrics HTTP/1.1\r\nX-Pad: " + std::string(6000, 'a'));  // never ends either
    bool closed = false;
    const std::string answer = big.read_all(closed);
    EXPECT_TRUE(closed);
    EXPECT_EQ(answer.rfind("HTTP/1.1 400 ", 0), 0U) << answer;
}

TEST(AdminServer, OnlyAFewClientsAreServedAtOnceTheRestAreClosed) {
    SilentFactory factory;
    wsrv::ServerBuilder builder = with_admin(factory);
    builder.config().admin_max_connections = 2;
    RunningServer running{builder};
    const std::uint16_t port = running.server().admin_endpoint()->port;
    Raw first{port};
    Raw second{port};
    ASSERT_TRUE(first.connected());
    ASSERT_TRUE(second.connected());
    std::this_thread::sleep_for(100ms);  // both are registered with the loop
    Raw third{port};
    ASSERT_TRUE(third.connected());
    bool closed = false;
    const std::string nothing = third.read_all(closed, 2s);
    EXPECT_TRUE(closed) << "over the limit: closed at once";
    EXPECT_TRUE(nothing.empty());

    first.send("GET /healthz HTTP/1.1\r\n\r\n");  // the two that were admitted still work
    const std::string served = first.read_all(closed);
    EXPECT_EQ(served.rfind("HTTP/1.1 200 OK\r\n", 0), 0U);
}

TEST(AdminServer, AFinishedRequestFreesItsSlot) {
    SilentFactory factory;
    wsrv::ServerBuilder builder = with_admin(factory);
    builder.config().admin_max_connections = 1;
    RunningServer running{builder};
    const std::uint16_t port = running.server().admin_endpoint()->port;
    for (int i = 0; i < 25; ++i) {  // far more requests than slots, one after the other
        ASSERT_EQ(get(port, "GET /healthz HTTP/1.1\r\n\r\n").rfind("HTTP/1.1 200 OK\r\n", 0), 0U)
            << i;
    }
}

TEST(AdminServer, AnAdminPortThatIsAlreadyTakenIsAStartupError) {  // EC-N20
    SilentFactory factory;
    RunningServer running{with_admin(factory)};
    wsrv::ServerBuilder clash;
    clash.listen("127.0.0.1", 0).handler_factory(factory).admin("127.0.0.1", running.port());
    EXPECT_THROW(static_cast<void>(clash.build()), wsrv::ConfigError);
}

TEST(AdminServer, TheConfigurationBoundsAreChecked) {
    SilentFactory factory;
    wsrv::ServerBuilder zero = with_admin(factory);
    zero.config().admin_max_connections = 0;
    EXPECT_THROW(static_cast<void>(zero.build()), wsrv::ConfigError);
    wsrv::ServerBuilder many = with_admin(factory);
    many.config().admin_max_connections = 1000;
    EXPECT_THROW(static_cast<void>(many.build()), wsrv::ConfigError);
    wsrv::ServerBuilder never = with_admin(factory);
    never.config().admin_request_timeout = 0ms;
    EXPECT_THROW(static_cast<void>(never.build()), wsrv::ConfigError);
}

}  // namespace

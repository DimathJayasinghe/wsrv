// SPDX-License-Identifier: Apache-2.0
// What a running server counts (ADR-0023 section 4): accepts and every kind of refusal, messages
// and bytes in both directions, how connections ended, across all loops. Real loopback sockets.
#include "base/metrics.hpp"
#include "server/server_builder.hpp"
#include "support/running_server.hpp"
#include "support/tcp_client.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using wsrv::Metric;
using wsrv::Opcode;
using wsrv::test::RunningServer;
using wsrv::test::TcpClient;

class EchoHandler final : public wsrv::IWebSocketHandler {
public:
    void on_text(wsrv::Session& session, std::string_view text) override {
        static_cast<void>(session.send_text(text).has_value());
    }
    void on_binary(wsrv::Session& session, wsrv::ByteView data) override {
        static_cast<void>(session.send_binary(data).has_value());
    }
};

class EchoFactory final : public wsrv::IHandlerFactory {
public:
    std::unique_ptr<wsrv::IWebSocketHandler> create(const wsrv::ConnectionInfo& /*info*/) override {
        return std::make_unique<EchoHandler>();
    }
};

wsrv::ServerBuilder base(EchoFactory& factory, std::size_t threads = 1) {
    wsrv::ServerBuilder builder;
    builder.listen("127.0.0.1", 0).handler_factory(factory);
    builder.config().threads = threads;
    builder.config().handshake.allowed_origins = {"https://app.example.com"};
    builder.config().max_connections_per_ip = 0;  // every client is 127.0.0.1
    builder.config().max_new_connections_per_ip_per_second = 0;
    return builder;
}

template <class Condition>
bool eventually(Condition&& condition, std::chrono::milliseconds limit = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return condition();
}

std::uint64_t value(RunningServer& running, Metric metric) {
    return running.server().metrics().value(metric);
}

TEST(ServerMetrics, ANormalConversationIsCountedInBothDirections) {
    EchoFactory factory;
    RunningServer running{base(factory)};
    {
        TcpClient client{running.port()};
        ASSERT_TRUE(client.upgrade());
        ASSERT_TRUE(client.send_frame(Opcode::Text, "hello"));
        const auto echoed = client.read_frame();
        ASSERT_TRUE(echoed.has_value());
        ASSERT_TRUE(client.send_frame(Opcode::Binary, std::string(300, 'x')));
        ASSERT_TRUE(client.read_frame().has_value());
        EXPECT_EQ(value(running, Metric::ConnectionsAccepted), 1U);
        EXPECT_TRUE(eventually([&] { return value(running, Metric::MessagesOutBinary) == 1; }));
        EXPECT_EQ(value(running, Metric::MessagesInText), 1U);
        EXPECT_EQ(value(running, Metric::MessagesInBinary), 1U);
        EXPECT_EQ(value(running, Metric::MessagesOutText), 1U);
        EXPECT_GT(value(running, Metric::BytesIn), 300U) << "the request head and both frames";
        EXPECT_GT(value(running, Metric::BytesOut), 300U) << "the 101 and both echoes";
        ASSERT_TRUE(client.send_frame(Opcode::Close, std::string("\x03\xE8", 2)));  // 1000
        ASSERT_TRUE(client.read_frame().has_value());  // our Close back
    }
    EXPECT_TRUE(eventually([&] { return value(running, Metric::CloseNormal) == 1; }));
    EXPECT_EQ(value(running, Metric::CloseAbnormal), 0U);
}

TEST(ServerMetrics, AClientThatVanishesIsAnAbnormalClose) {
    EchoFactory factory;
    RunningServer running{base(factory)};
    {
        TcpClient client{running.port()};
        ASSERT_TRUE(client.upgrade());
    }  // no Close frame
    EXPECT_TRUE(eventually([&] { return value(running, Metric::CloseAbnormal) == 1; }));
    EXPECT_EQ(value(running, Metric::CloseNormal), 0U);
}

TEST(ServerMetrics, AProtocolViolationIsCountedAsAProtocolErrorClose) {
    EchoFactory factory;
    RunningServer running{base(factory)};
    TcpClient client{running.port()};
    ASSERT_TRUE(client.upgrade());
    ASSERT_TRUE(client.send(std::string("\xF0\x00", 2)));  // RSV bits set without an extension
    const auto answer = client.read_frame();               // the server's Close 1002
    ASSERT_TRUE(answer.has_value());
    EXPECT_EQ(answer->opcode, Opcode::Close);
    client.close();
    EXPECT_TRUE(eventually([&] { return value(running, Metric::CloseProtocolError) == 1; }));
}

TEST(ServerMetrics, EveryKindOfRefusalHasItsOwnCounter) {
    EchoFactory factory;
    wsrv::ServerBuilder builder = base(factory);
    builder.config().max_connections = 2;
    builder.config().max_connections_per_ip = 0;
    RunningServer running{builder};
    TcpClient first{running.port()};
    TcpClient second{running.port()};
    ASSERT_TRUE(first.upgrade());
    ASSERT_TRUE(second.upgrade());
    TcpClient third{running.port()};  // over the global cap: closed without an answer
    EXPECT_TRUE(eventually([&] { return value(running, Metric::RejectedGlobalCap) == 1; }));
    EXPECT_EQ(value(running, Metric::ConnectionsAccepted), 2U) << "a refusal is not an accept";
    EXPECT_EQ(value(running, Metric::RejectedIpConnections), 0U);
}

TEST(ServerMetrics, PerIpRefusalsAreCountedByReason) {
    EchoFactory factory;
    wsrv::ServerBuilder builder = base(factory);
    builder.config().max_connections_per_ip = 1;
    builder.config().max_new_connections_per_ip_per_second = 0;
    RunningServer running{builder};
    TcpClient first{running.port()};
    ASSERT_TRUE(first.upgrade());
    TcpClient over{running.port()};
    EXPECT_TRUE(eventually([&] { return value(running, Metric::RejectedIpConnections) == 1; }));
    EXPECT_EQ(value(running, Metric::RejectedIpRate), 0U);
    EXPECT_EQ(running.server().ip_limited_connections(), 1U) << "the older accessor still works";

    wsrv::ServerBuilder rate = base(factory);
    rate.config().max_connections_per_ip = 0;
    rate.config().max_new_connections_per_ip_per_second = 1;
    RunningServer fast{rate};
    TcpClient one{fast.port()};
    ASSERT_TRUE(one.upgrade());
    TcpClient two{fast.port()};
    EXPECT_TRUE(eventually([&] { return value(fast, Metric::RejectedIpRate) == 1; }));
}

TEST(ServerMetrics, ARefusedHandshakeIsCounted) {
    EchoFactory factory;
    RunningServer running{base(factory)};
    TcpClient client{running.port()};
    EXPECT_FALSE(client.upgrade("https://evil.example.com"));  // 403: origin not allowed
    EXPECT_TRUE(eventually([&] { return value(running, Metric::HandshakeFailed) == 1; }));
    EXPECT_EQ(value(running, Metric::ConnectionsAccepted), 1U) << "it was accepted, then refused";
}

TEST(ServerMetrics, TheCountsAddUpAcrossLoops) {
    EchoFactory factory;
    RunningServer running{base(factory, 3)};
    std::vector<std::unique_ptr<TcpClient>> clients;
    for (int i = 0; i < 6; ++i) {  // round-robin over three loops
        clients.push_back(std::make_unique<TcpClient>(running.port()));
        ASSERT_TRUE(clients.back()->upgrade());
        ASSERT_TRUE(clients.back()->send_frame(Opcode::Text, "x"));
        ASSERT_TRUE(clients.back()->read_frame().has_value());
    }
    EXPECT_TRUE(eventually([&] { return value(running, Metric::MessagesInText) == 6; }));
    EXPECT_EQ(value(running, Metric::ConnectionsAccepted), 6U);
    EXPECT_EQ(running.server().metrics_gauges().connections_current, 6U);
}

TEST(ServerMetrics, TheTextPageShowsTheLiveNumbers) {
    EchoFactory factory;
    RunningServer running{base(factory)};
    TcpClient client{running.port()};
    ASSERT_TRUE(client.upgrade());
    const std::string text = running.server().metrics_text();
    EXPECT_NE(text.find("wsrv_connections_current 1\n"), std::string::npos) << text;
    EXPECT_NE(text.find("wsrv_connections_total{result=\"accepted\"} 1\n"), std::string::npos)
        << text;
}

}  // namespace

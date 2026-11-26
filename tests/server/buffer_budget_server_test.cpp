// SPDX-License-Identifier: Apache-2.0
// The global buffered-bytes budget on a real server (ADR-0040): when the connections together
// hold more than `max_buffered_bytes`, new clients get a cheap 503 and the ones already connected
// are left alone, and the number goes back to exactly zero however connections end, on every loop.
// Real loopback sockets.
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
constexpr std::size_t kKiB = 1024;
using wsrv::Opcode;
using wsrv::test::RunningServer;
using wsrv::test::TcpClient;

class SilentFactory final : public wsrv::IHandlerFactory {
public:
    std::unique_ptr<wsrv::IWebSocketHandler> create(const wsrv::ConnectionInfo& /*info*/) override {
        return std::make_unique<wsrv::IWebSocketHandler>();
    }
};

wsrv::ServerBuilder
budgeted(SilentFactory& factory, std::size_t budget_bytes, std::size_t threads = 1) {
    wsrv::ServerBuilder builder;
    builder.listen("127.0.0.1", 0).handler_factory(factory);
    builder.config().threads = threads;
    builder.config().handshake.allowed_origins = {"https://app.example.com"};
    builder.config().max_buffered_bytes = budget_bytes;
    builder.config().max_connections_per_ip = 0;  // every client here is 127.0.0.1
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

std::uint64_t buffered(RunningServer& running) {
    return running.server().metrics_gauges().buffered_bytes;
}

/// What a shed client sees: a complete 503 with Retry-After, then the end of the stream.
void expect_503(TcpClient& client) {
    const std::string head = client.read_http_head();
    EXPECT_EQ(head.rfind("HTTP/1.1 503 ", 0), 0U) << head;
    EXPECT_NE(head.find("Retry-After: 1"), std::string::npos) << head;
    EXPECT_TRUE(client.wait_for_eof());
}

TEST(BufferBudgetServer, AServerOverItsBudgetShedsNewConnectionsAndLeavesTheOnesItHas) {
    SilentFactory factory;
    RunningServer running{budgeted(factory, 100 * kKiB, /*threads=*/2)};
    TcpClient first{running.port()};
    TcpClient second{running.port()};
    ASSERT_TRUE(first.upgrade());
    ASSERT_TRUE(second.upgrade());
    // Two clients, two loops, each half way through a 64 KiB message: 128 KiB held, 100 KiB
    // allowed.
    ASSERT_TRUE(first.send_frame(Opcode::Binary, std::string(64 * kKiB, 'a'), false));
    ASSERT_TRUE(second.send_frame(Opcode::Binary, std::string(64 * kKiB, 'b'), false));
    ASSERT_TRUE(eventually([&] { return buffered(running) == 128 * kKiB; }))
        << "the sum over both loops: " << buffered(running);

    TcpClient refused{running.port()};
    expect_503(refused);
    EXPECT_GE(running.server().metrics().value(wsrv::Metric::RejectedBufferBudget), 1U);
    EXPECT_FALSE(running.finished());

    // A soft cap: the connection that is already here is not touched. It still answers a Ping.
    ASSERT_TRUE(first.send_frame(Opcode::Ping, "still here"));
    const auto pong = first.read_frame();
    ASSERT_TRUE(pong.has_value());
    EXPECT_EQ(pong->opcode, Opcode::Pong);

    // Finishing the messages empties the budget, and the next client is welcome again.
    ASSERT_TRUE(first.send_frame(Opcode::Continuation, "", true));
    ASSERT_TRUE(second.send_frame(Opcode::Continuation, "", true));
    ASSERT_TRUE(eventually([&] { return buffered(running) == 0; }));
    TcpClient welcome{running.port()};
    EXPECT_TRUE(welcome.upgrade());
}

TEST(BufferBudgetServer, ABudgetThatIsNotUsedUpShedsNobody) {
    SilentFactory factory;
    RunningServer running{budgeted(factory, 1024 * kKiB, 2)};
    std::vector<std::unique_ptr<TcpClient>> clients;
    for (int i = 0; i < 8; ++i) {
        clients.push_back(std::make_unique<TcpClient>(running.port()));
        ASSERT_TRUE(clients.back()->upgrade()) << i;
        ASSERT_TRUE(clients.back()->send_frame(Opcode::Binary, std::string(8 * kKiB, 'a'), false));
    }
    ASSERT_TRUE(eventually(
        [&] { return buffered(running) == std::size_t{8} * 8 * kKiB; }));  // 64 KiB of 1 MiB
    EXPECT_EQ(running.server().metrics().value(wsrv::Metric::RejectedBufferBudget), 0U);
}

// The property that keeps the budget from becoming an outage: whatever way a connection ends, on
// whatever loop, what it held is given back and the number returns to exactly zero.
TEST(BufferBudgetServer, TheGaugeReturnsToZeroWhateverWayTheConnectionsEnd) {
    SilentFactory factory;
    RunningServer running{budgeted(factory, std::size_t{1} << 30, /*threads=*/3)};
    constexpr std::size_t kPartial = 10 * kKiB;
    std::vector<std::unique_ptr<TcpClient>> clients;
    for (int i = 0; i < 5; ++i) {  // round-robin over the three loops
        clients.push_back(std::make_unique<TcpClient>(running.port()));
        ASSERT_TRUE(clients.back()->upgrade()) << i;
        ASSERT_TRUE(clients.back()->send_frame(Opcode::Binary, std::string(kPartial, 'a'), false));
    }
    const TcpClient mute{running.port()};  // connects and says nothing: holds nothing
    ASSERT_TRUE(mute.valid());
    ASSERT_TRUE(eventually([&] { return buffered(running) == 5 * kPartial; })) << buffered(running);

    clients[0]->close();  // a vanished peer
    ASSERT_TRUE(
        clients[1]->send_frame(Opcode::Close, std::string("\x03\xe8", 2)));   // a clean close
    ASSERT_TRUE(clients[2]->send_frame(Opcode::Text, "not a continuation"));  // a protocol error
    clients[3]->close();
    ASSERT_TRUE(clients[4]->send_frame(Opcode::Close, std::string("\x03\xe8", 2)));
    EXPECT_TRUE(clients[1]->wait_for_eof());
    EXPECT_TRUE(clients[2]->wait_for_eof());
    EXPECT_TRUE(clients[4]->wait_for_eof());

    EXPECT_TRUE(eventually([&] { return buffered(running) == 0; }))
        << "left over: " << buffered(running);
}

}  // namespace

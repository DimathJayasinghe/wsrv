// SPDX-License-Identifier: Apache-2.0
// Per-IP limits on a real server (ADR-0021 section 5): the accept path refuses an address that
// holds its cap or knocks too fast with a cheap 429, and every way a connection can end gives its
// slot back, also from another loop. Real loopback sockets; every client here is 127.0.0.1.
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
using wsrv::test::RunningServer;
using wsrv::test::TcpClient;

class SilentFactory final : public wsrv::IHandlerFactory {
public:
    std::unique_ptr<wsrv::IWebSocketHandler> create(const wsrv::ConnectionInfo& /*info*/) override {
        return std::make_unique<wsrv::IWebSocketHandler>();
    }
};

wsrv::ServerBuilder limited(SilentFactory& factory,
                            std::size_t per_ip,
                            std::uint32_t per_second,
                            std::size_t threads = 1) {
    wsrv::ServerBuilder builder;
    builder.listen("127.0.0.1", 0).handler_factory(factory);
    builder.config().threads = threads;
    builder.config().handshake.allowed_origins = {"https://app.example.com"};
    builder.config().max_connections_per_ip = per_ip;
    builder.config().max_new_connections_per_ip_per_second = per_second;
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

/// What a refused client sees: a complete HTTP answer, then the end of the stream.
void expect_429(TcpClient& client) {
    const std::string head = client.read_http_head();
    EXPECT_EQ(head.rfind("HTTP/1.1 429 ", 0), 0U) << head;
    EXPECT_NE(head.find("Retry-After: 1"), std::string::npos) << head;
    EXPECT_TRUE(client.wait_for_eof());
}

TEST(IpLimitsServer, AnAddressOverItsCapGetsA429AndTheSlotComesBackWhenOneEnds) {
    SilentFactory factory;
    RunningServer running{limited(factory, 2, 0)};
    TcpClient first{running.port()};
    TcpClient second{running.port()};
    ASSERT_TRUE(first.upgrade());
    ASSERT_TRUE(second.upgrade());

    TcpClient third{running.port()};
    expect_429(third);
    EXPECT_FALSE(running.finished());  // and the server carries on

    first.close();
    EXPECT_TRUE(eventually([&] {
        TcpClient again{running.port()};
        return again.upgrade();
    })) << "the slot of the connection that ended is free again";
}

TEST(IpLimitsServer, TheCapHoldsAcrossLoopsAndASlotComesBackFromAnyLoop) {
    SilentFactory factory;
    RunningServer running{limited(factory, 3, 0, /*threads=*/3)};
    std::vector<std::unique_ptr<TcpClient>> clients;
    for (int i = 0; i < 3; ++i) {  // round-robin: one per loop
        clients.push_back(std::make_unique<TcpClient>(running.port()));
        ASSERT_TRUE(clients.back()->upgrade()) << i;
    }
    TcpClient over{running.port()};
    expect_429(over);
    for (std::size_t i = 0; i < clients.size(); ++i) {  // each loop gives its slot back in turn
        clients[i]->close();
        EXPECT_TRUE(eventually([&] {
            TcpClient again{running.port()};
            return again.upgrade();
        })) << "after closing connection "
            << i;
        // the probe above holds a slot only while it lives; it was destroyed at the lambda's end
    }
}

TEST(IpLimitsServer, AConnectionThatNeverFinishedItsHandshakeStillGivesItsSlotBack) {
    SilentFactory factory;
    RunningServer running{limited(factory, 1, 0)};
    {
        TcpClient idle{running.port()};  // connects, says nothing, leaves
        ASSERT_TRUE(idle.valid());
    }
    EXPECT_TRUE(eventually([&] {
        TcpClient next{running.port()};
        return next.upgrade();
    }));
}

TEST(IpLimitsServer, AFastNewcomerIsRefusedAndForgivenAfterASecond) {
    SilentFactory factory;
    RunningServer running{limited(factory, 0, 3)};  // no cap, three new connections per second
    for (int i = 0; i < 3; ++i) {
        TcpClient client{running.port()};  // each one ends before the next: only the rate counts
        ASSERT_TRUE(client.upgrade()) << i;
    }
    TcpClient fourth{running.port()};
    expect_429(fourth);

    std::this_thread::sleep_for(1100ms);
    TcpClient later{running.port()};
    EXPECT_TRUE(later.upgrade());
}

TEST(IpLimitsServer, WithBothLimitsOffNothingIsRefused) {
    SilentFactory factory;
    RunningServer running{limited(factory, 0, 0)};
    for (int i = 0; i < 120; ++i) {  // more than the default burst of 50
        TcpClient client{running.port()};
        ASSERT_TRUE(client.upgrade()) << i;
    }
}

TEST(IpLimitsServer, TheDefaultsAreTheDocumentedOnes) {  // REQUIREMENTS section 4
    const wsrv::ServerConfig config;
    EXPECT_EQ(config.max_connections_per_ip, 100U);
    EXPECT_EQ(config.max_new_connections_per_ip_per_second, 50U);
}

TEST(IpLimitsServer, ARefusedConnectionDoesNotUseUpTheGlobalCap) {
    SilentFactory factory;
    wsrv::ServerBuilder builder = limited(factory, 1, 0);
    builder.config().max_connections = 2;
    RunningServer running{builder};
    TcpClient first{running.port()};
    ASSERT_TRUE(first.upgrade());
    for (int i = 0; i < 5; ++i) {  // five refusals by the per-IP cap
        TcpClient over{running.port()};
        expect_429(over);
    }
    EXPECT_EQ(running.server().rejected_connections(), 0U) << "those were not the global cap";
}

}  // namespace

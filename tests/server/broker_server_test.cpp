// SPDX-License-Identifier: Apache-2.0
// The broker on a real server with three loops (ADR-0024): a message published on one loop reaches
// subscribers that live on the others, the publisher can be left out, a connection that ends gives
// every subscription back (BrokerMembership), and the counters reach the metrics page. Real
// loopback sockets, a chat-room handler as the application.
#include "server/broker.hpp"
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
using wsrv::Broker;
using wsrv::BrokerMembership;
using wsrv::Metric;
using wsrv::test::RunningServer;
using wsrv::test::TcpClient;

/// Everyone who connects joins "lobby"; every text message goes to the others in it.
class LobbyHandler final : public wsrv::IWebSocketHandler {
public:
    explicit LobbyHandler(Broker& broker) : broker_{broker}, membership_{broker} {}
    void on_open(wsrv::Session& session) override {
        static_cast<void>(membership_.subscribe(session.handle(), "lobby").has_value());
    }
    void on_text(wsrv::Session& session, std::string_view text) override {
        static_cast<void>(broker_.publish_text("lobby", text, session.id()).has_value());
    }

private:
    Broker& broker_;
    BrokerMembership membership_;  // destroyed with the handler, i.e. with the connection
};

class LobbyFactory final : public wsrv::IHandlerFactory {
public:
    explicit LobbyFactory(Broker& broker) : broker_{broker} {}
    std::unique_ptr<wsrv::IWebSocketHandler> create(const wsrv::ConnectionInfo& /*info*/) override {
        return std::make_unique<LobbyHandler>(broker_);
    }

private:
    Broker& broker_;
};

wsrv::ServerBuilder lobby_server(LobbyFactory& factory, std::size_t threads) {
    wsrv::ServerBuilder builder;
    builder.listen("127.0.0.1", 0).handler_factory(factory);
    builder.config().threads = threads;
    builder.config().handshake.allowed_origins = {"https://app.example.com"};
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

/// The text of the next frame, or "<none>".
std::string next_text(TcpClient& client) {
    const auto frame = client.read_frame();
    return frame ? frame->payload : std::string{"<none>"};
}

TEST(BrokerServer, AMessageReachesEveryoneElseOnEveryLoopAndNotItsSender) {
    Broker broker;
    LobbyFactory factory{broker};
    RunningServer running{lobby_server(factory, 3)};
    broker.use_metrics(&running.server().shared_metrics());
    std::vector<std::unique_ptr<TcpClient>> clients;
    for (int i = 0; i < 6; ++i) {  // round-robin: two per loop
        clients.push_back(std::make_unique<TcpClient>(running.port()));
        ASSERT_TRUE(clients.back()->upgrade()) << i;
    }
    ASSERT_TRUE(eventually([&] { return broker.subscriber_count("lobby") == 6; }));

    ASSERT_TRUE(clients[0]->send_frame(wsrv::Opcode::Text, "hello room"));
    for (std::size_t i = 1; i < clients.size(); ++i) {
        EXPECT_EQ(next_text(*clients[i]), "hello room") << "client " << i;
    }
    // The sender is not echoed: its next frame is the reply to what it sends next, not "hello
    // room".
    ASSERT_TRUE(clients[1]->send_frame(wsrv::Opcode::Text, "second"));
    EXPECT_EQ(next_text(*clients[0]), "second");
}

TEST(BrokerServer, AConnectionThatEndsGivesItsSubscriptionBackWhateverHowItEnds) {
    Broker broker;
    LobbyFactory factory{broker};
    RunningServer running{lobby_server(factory, 3)};
    std::vector<std::unique_ptr<TcpClient>> clients;
    for (int i = 0; i < 6; ++i) {
        clients.push_back(std::make_unique<TcpClient>(running.port()));
        ASSERT_TRUE(clients.back()->upgrade());
    }
    ASSERT_TRUE(eventually([&] { return broker.subscriber_count("lobby") == 6; }));
    clients[0]->close();  // a plain disconnect, no Close frame
    EXPECT_TRUE(eventually([&] { return broker.subscriber_count("lobby") == 5; }));
    for (std::size_t i = 1; i < clients.size(); ++i) {
        clients[i]->close();
    }
    EXPECT_TRUE(eventually([&] { return broker.topic_count() == 0; }))
        << "nothing is left behind once every connection is gone";
}

TEST(BrokerServer, ASubscriberThatNeverReadsDoesNotHoldBackTheOthers) {
    Broker broker;
    LobbyFactory factory{broker};
    RunningServer running{lobby_server(factory, 3)};
    TcpClient sender{running.port()};
    TcpClient stalled{running.port()};  // subscribed, never reads
    TcpClient reader{running.port()};
    ASSERT_TRUE(sender.upgrade());
    ASSERT_TRUE(stalled.upgrade());
    ASSERT_TRUE(reader.upgrade());
    ASSERT_TRUE(eventually([&] { return broker.subscriber_count("lobby") == 3; }));
    for (int i = 0; i < 50; ++i) {
        ASSERT_TRUE(
            sender.send_frame(wsrv::Opcode::Text, std::string(1000, 'x') + std::to_string(i)));
    }
    for (int i = 0; i < 50; ++i) {
        EXPECT_EQ(next_text(reader), std::string(1000, 'x') + std::to_string(i)) << i;
    }
}

TEST(BrokerServer, TheBrokersCountersAppearInTheServersMetrics) {  // ADR-0024 section 6
    Broker broker;
    LobbyFactory factory{broker};
    RunningServer running{lobby_server(factory, 2)};
    broker.use_metrics(&running.server().shared_metrics());
    TcpClient a{running.port()};
    TcpClient b{running.port()};
    ASSERT_TRUE(a.upgrade());
    ASSERT_TRUE(b.upgrade());
    ASSERT_TRUE(eventually([&] { return broker.subscriber_count("lobby") == 2; }));
    ASSERT_TRUE(a.send_frame(wsrv::Opcode::Text, "x"));
    EXPECT_EQ(next_text(b), "x");
    // The publisher counts after its sends return, so the subscriber can have the message first.
    ASSERT_TRUE(eventually(
        [&] { return running.server().metrics().value(Metric::BrokerPublishes) == 1U; }));
    const wsrv::MetricsSnapshot now = running.server().metrics();
    EXPECT_EQ(now.value(Metric::BrokerPublishes), 1U);
    EXPECT_EQ(now.value(Metric::BrokerDeliveriesAccepted), 1U);
    EXPECT_EQ(now.value(Metric::BrokerDeliveriesRefused), 0U);
    const std::string page = running.server().metrics_text();
    EXPECT_NE(page.find("wsrv_broker_publishes_total 1\n"), std::string::npos) << page;
    EXPECT_NE(page.find("wsrv_broker_deliveries_total{result=\"accepted\"} 1\n"),
              std::string::npos);
}

}  // namespace

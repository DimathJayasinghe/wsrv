// SPDX-License-Identifier: Apache-2.0
// The client address behind a trusted reverse proxy on a real server (ADR-0029): a header from a
// trusted peer names the client, the per-IP limits then work per client and not per proxy, the slot
// comes back, and a header from anyone else is ignored. Every client here connects from 127.0.0.1.
#include "net/endpoint.hpp"
#include "server/client_ip.hpp"
#include "server/server_builder.hpp"
#include "support/running_server.hpp"
#include "support/tcp_client.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using wsrv::test::RunningServer;
using wsrv::test::TcpClient;

/// Remembers the peer address every connection was announced with.
class PeerRecorder final : public wsrv::IHandlerFactory {
public:
    std::unique_ptr<wsrv::IWebSocketHandler> create(const wsrv::ConnectionInfo& info) override {
        const std::lock_guard<std::mutex> lock{mutex_};
        peers_.push_back(wsrv::format_endpoint(info.peer));
        return std::make_unique<wsrv::IWebSocketHandler>();
    }
    std::vector<std::string> peers() {
        const std::lock_guard<std::mutex> lock{mutex_};
        return peers_;
    }

private:
    std::mutex mutex_;
    std::vector<std::string> peers_;
};

wsrv::ServerBuilder behind_proxy(PeerRecorder& factory,
                                 std::size_t per_ip,
                                 std::vector<std::string> trusted = {"127.0.0.0/8"},
                                 std::size_t threads = 1) {
    wsrv::ServerBuilder builder;
    builder.listen("127.0.0.1", 0).handler_factory(factory);
    wsrv::ServerConfig& config = builder.config();
    config.threads = threads;
    config.handshake.allowed_origins = {"https://app.example.com"};
    config.max_connections_per_ip = per_ip;
    config.max_new_connections_per_ip_per_second = 0;
    for (const std::string& text : trusted) {
        config.trusted_proxies.push_back(*wsrv::parse_ip_net(text));
    }
    return builder;
}

std::string forwarded(std::string_view value) {
    return "X-Forwarded-For: " + std::string{value} + "\r\n";
}

bool upgrade_as(TcpClient& client, std::string_view xff) {
    return client.upgrade_head(forwarded(xff)).rfind("HTTP/1.1 101 ", 0) == 0;
}

std::string status_line(const std::string& head) {
    return head.substr(0, head.find("\r\n"));
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

TEST(ClientIpServer, TheLimitCountsClientsBehindTheProxyNotTheProxy) {
    PeerRecorder factory;
    RunningServer running{behind_proxy(factory, 1)};
    TcpClient alice{running.port()};
    TcpClient bob{running.port()};
    ASSERT_TRUE(upgrade_as(alice, "203.0.113.1"));
    EXPECT_TRUE(upgrade_as(bob, "203.0.113.2"))
        << "another client behind the same proxy has its own budget";

    TcpClient alice_again{running.port()};
    const std::string head = alice_again.upgrade_head(forwarded("203.0.113.1"));
    EXPECT_EQ(status_line(head), "HTTP/1.1 429 Too Many Requests") << head;
    EXPECT_NE(head.find("Retry-After: 5"), std::string::npos)
        << "a handshake refusal, like any other";
    EXPECT_TRUE(alice_again.wait_for_eof());
}

TEST(ClientIpServer, ASlotBehindTheProxyComesBackWhenTheConnectionEnds) {
    PeerRecorder factory;
    RunningServer running{behind_proxy(factory, 1)};
    {
        TcpClient first{running.port()};
        ASSERT_TRUE(upgrade_as(first, "203.0.113.1"));
    }  // closed
    EXPECT_TRUE(eventually([&] {
        TcpClient next{running.port()};
        return upgrade_as(next, "203.0.113.1");
    }));
}

TEST(ClientIpServer, ARefusedHandshakeBehindTheProxyHoldsNoSlot) {
    PeerRecorder factory;
    RunningServer running{behind_proxy(factory, 1)};
    TcpClient holder{running.port()};
    ASSERT_TRUE(upgrade_as(holder, "203.0.113.1"));
    for (int i = 0; i < 3; ++i) {  // each is refused with 429; none may leak or take the slot
        TcpClient over{running.port()};
        EXPECT_NE(status_line(over.upgrade_head(forwarded("203.0.113.1"))).find(" 429 "),
                  std::string::npos)
            << i;
    }
    holder.close();
    EXPECT_TRUE(eventually([&] {
        TcpClient next{running.port()};
        return upgrade_as(next, "203.0.113.1");
    })) << "the one slot is free again, so none was leaked by the refusals";
}

TEST(ClientIpServer, AConnectionFromTheProxyTakesNoSlotBeforeItsHandshake) {
    PeerRecorder factory;
    RunningServer running{behind_proxy(factory, 1)};
    TcpClient idle_one{
        running.port()};  // connected, silent: the proxy opened it for a client not yet known
    TcpClient idle_two{running.port()};
    ASSERT_TRUE(idle_one.valid());
    ASSERT_TRUE(idle_two.valid());
    EXPECT_TRUE(upgrade_as(idle_two, "203.0.113.9"))
        << "accept did not charge the proxy's own budget";
}

TEST(ClientIpServer, TheApplicationIsToldTheClientNotTheProxy) {
    PeerRecorder factory;
    RunningServer running{behind_proxy(factory, 0)};
    TcpClient client{running.port()};
    ASSERT_TRUE(upgrade_as(client, "198.51.100.7, 127.0.0.2"));
    ASSERT_TRUE(eventually([&] { return !factory.peers().empty(); }));
    EXPECT_EQ(factory.peers().front(), "198.51.100.7:0")
        << "rightmost untrusted entry, proxy hops skipped";
}

TEST(ClientIpServer, AHeaderFromAnUntrustedPeerIsIgnored) {
    PeerRecorder factory;
    RunningServer running{
        behind_proxy(factory, 1, /*trusted=*/{"10.0.0.0/8"})};  // 127.0.0.1 is not trusted
    TcpClient first{running.port()};
    ASSERT_TRUE(upgrade_as(first, "203.0.113.1"));
    ASSERT_TRUE(eventually([&] { return !factory.peers().empty(); }));
    EXPECT_EQ(factory.peers().front().rfind("127.0.0.1:", 0), 0U) << "the TCP peer, not the claim";

    TcpClient second{running.port()};  // a different claimed address does not buy a second slot
    EXPECT_NE(status_line(second.upgrade_head(forwarded("203.0.113.2"))).find(" 429 "),
              std::string::npos)
        << "refused at accept: the same address as far as the server can tell";
}

TEST(ClientIpServer, WithoutTrustedProxiesTheFeatureIsOff) {
    PeerRecorder factory;
    RunningServer running{behind_proxy(factory, 1, /*trusted=*/{})};
    TcpClient first{running.port()};
    ASSERT_TRUE(upgrade_as(first, "203.0.113.1"));
    TcpClient second{running.port()};
    EXPECT_FALSE(upgrade_as(second, "203.0.113.2"));
}

TEST(ClientIpServer, AMissingOrUnusableHeaderFromATrustedProxyFallsBackToTheProxyAddress) {
    PeerRecorder factory;
    RunningServer running{behind_proxy(factory, 1)};
    TcpClient first{running.port()};
    ASSERT_TRUE(first.upgrade());  // trusted proxy, no header at all
    ASSERT_TRUE(eventually([&] { return !factory.peers().empty(); }));
    EXPECT_EQ(factory.peers().front().rfind("127.0.0.1:", 0), 0U);

    TcpClient junk{running.port()};  // a header that is not an address: same, the stricter answer
    EXPECT_NE(status_line(junk.upgrade_head(forwarded("not-an-address"))).find(" 429 "),
              std::string::npos)
        << "both were keyed to the proxy, whose single slot is taken";
}

TEST(ClientIpServer, TheBudgetBehindTheProxyHoldsAcrossLoops) {
    PeerRecorder factory;
    RunningServer running{behind_proxy(factory, 2, {"127.0.0.0/8"}, /*threads=*/3)};
    std::vector<std::unique_ptr<TcpClient>> clients;
    for (int i = 0; i < 2; ++i) {
        clients.push_back(std::make_unique<TcpClient>(running.port()));
        ASSERT_TRUE(upgrade_as(*clients.back(), "203.0.113.1")) << i;
    }
    TcpClient over{running.port()};
    EXPECT_NE(status_line(over.upgrade_head(forwarded("203.0.113.1"))).find(" 429 "),
              std::string::npos);
    clients.front()->close();
    EXPECT_TRUE(eventually([&] {
        TcpClient again{running.port()};
        return upgrade_as(again, "203.0.113.1");
    }));
}

}  // namespace

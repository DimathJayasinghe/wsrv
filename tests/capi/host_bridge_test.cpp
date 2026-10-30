// SPDX-License-Identifier: Apache-2.0
#include "capi/host_bridge.hpp"

#include "support/running_server.hpp"
#include "support/tcp_client.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>

namespace {

using namespace wsrv;
using namespace wsrv::test;
using namespace std::chrono_literals;

std::string as_string(const std::vector<std::byte>& bytes) {
    std::string out;
    for (const std::byte b : bytes) {
        out.push_back(static_cast<char>(b));
    }
    return out;
}

/// A bridge, a real server using it, and helpers that play the host's part.
class HostBridgeTest : public ::testing::Test {
protected:
    void start(EventQueueConfig queue = {}) {
        auto made = HostBridge::make(sys_, queue);
        ASSERT_TRUE(made.has_value());
        bridge_ = std::move(*made);
        ServerBuilder builder;
        builder.listen("127.0.0.1", 0).handler_factory(*bridge_);
        builder.config().handshake.allowed_origins = {"https://app.example.com"};
        server_ = std::make_unique<RunningServer>(std::move(builder));
    }

    /// The next event, waiting on the queue's descriptor like a real host would.
    std::optional<HostEvent> next(std::chrono::milliseconds timeout = 3000ms) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            if (auto popped = bridge_->events().try_pop()) {
                return std::move(popped->event);
            }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) {
                return std::nullopt;
            }
            pollfd p{bridge_->events().event_fd(), POLLIN, 0};
            ::poll(&p, 1, static_cast<int>(left.count()));
        }
    }

    LinuxSyscalls sys_;
    std::unique_ptr<HostBridge> bridge_;
    std::unique_ptr<RunningServer> server_;  // declared after the bridge: stopped first
};

TEST_F(HostBridgeTest, OpenMessagesAndCloseArriveInOrderWithTheirContent) {
    start();
    TcpClient client{server_->port()};
    ASSERT_TRUE(client.upgrade());
    const auto open = next();
    ASSERT_TRUE(open.has_value());
    EXPECT_EQ(open->type, HostEventType::Open);
    EXPECT_NE(open->conn, 0U);
    EXPECT_EQ(as_string(open->data).rfind("127.0.0.1:", 0), 0U);  // the peer address

    ASSERT_TRUE(client.send_frame(Opcode::Text, "h\xC3\xA9llo"));
    ASSERT_TRUE(client.send_frame(Opcode::Binary, std::string{"\x00\x01\x02", 3}));
    const auto text = next();
    const auto binary = next();
    ASSERT_TRUE(text && binary);
    EXPECT_EQ(text->type, HostEventType::Text);
    EXPECT_EQ(text->conn, open->conn);
    EXPECT_EQ(as_string(text->data), "h\xC3\xA9llo");
    EXPECT_EQ(binary->type, HostEventType::Binary);
    EXPECT_EQ(binary->data.size(), 3U);

    ASSERT_TRUE(client.send_frame(Opcode::Close,
                                  std::string{"\x03\xE8"
                                              "bye",
                                              5}));
    const auto close = next();
    ASSERT_TRUE(close.has_value());
    EXPECT_EQ(close->type, HostEventType::Close);
    EXPECT_EQ(close->conn, open->conn);
    EXPECT_EQ(close->close_code, 1000);
    EXPECT_TRUE(close->clean);
    EXPECT_EQ(as_string(close->data), "bye");
}

TEST_F(HostBridgeTest, TheHostSendsAndClosesByConnectionIdFromAnotherThread) {
    start();
    TcpClient client{server_->port()};
    ASSERT_TRUE(client.upgrade());
    const auto open = next();
    ASSERT_TRUE(open.has_value());
    std::thread host([&] {
        EXPECT_TRUE(bridge_->send_text(open->conn, "to the client").has_value());
        const std::vector<std::byte> bytes{std::byte{1}, std::byte{2}};
        EXPECT_TRUE(bridge_->send_binary(open->conn, ByteView{bytes}).has_value());
        EXPECT_TRUE(bridge_->close(open->conn, CloseCode::Normal, "done").has_value());
    });
    host.join();
    const auto first = client.read_frame();
    const auto second = client.read_frame();
    const auto third = client.read_frame();
    ASSERT_TRUE(first && second && third);
    EXPECT_EQ(first->payload, "to the client");
    EXPECT_EQ(second->payload.size(), 2U);
    EXPECT_EQ(third->opcode, Opcode::Close);
}

TEST_F(HostBridgeTest, AnUnknownOrFinishedConnectionIdIsRefusedWithClosing) {
    start();
    const auto unknown = bridge_->send_text(12345, "x");
    ASSERT_FALSE(unknown.has_value());
    EXPECT_EQ(unknown.error().code, ErrorCode::Closing);

    TcpClient client{server_->port()};
    ASSERT_TRUE(client.upgrade());
    const auto open = next();
    ASSERT_TRUE(open.has_value());
    client.close();
    const auto close = next();
    ASSERT_TRUE(close.has_value());
    EXPECT_EQ(close->type, HostEventType::Close);
    EXPECT_FALSE(close->clean);
    EXPECT_EQ(close->close_code, 1006);  // no Close frame was seen
    EXPECT_EQ(bridge_->send_text(open->conn, "late").error().code, ErrorCode::Closing);
    EXPECT_EQ(bridge_->close(open->conn, CloseCode::Normal, "").error().code, ErrorCode::Closing);
}

TEST_F(HostBridgeTest, AConnectionWhoseOpenDoesNotFitIsClosedWith1013AndNeverAnnounced) {
    start({.max_events = 2, .max_bytes = 4096});  // room for one connection: Open + reserved Close
    TcpClient first{server_->port()};
    ASSERT_TRUE(first.upgrade());
    const auto open = next();
    ASSERT_TRUE(open.has_value());

    TcpClient second{server_->port()};
    ASSERT_TRUE(second.upgrade());
    const auto frame = second.read_frame();  // the server closes it at once
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->opcode, Opcode::Close);
    ASSERT_GE(frame->payload.size(), 2U);
    EXPECT_EQ((static_cast<unsigned>(static_cast<unsigned char>(frame->payload[0])) << 8U) |
                  static_cast<unsigned char>(frame->payload[1]),
              1013U);

    first.close();
    const auto close = next();
    ASSERT_TRUE(close.has_value());
    EXPECT_EQ(close->conn, open->conn);  // nothing was queued for the second connection
    EXPECT_FALSE(next(200ms).has_value());
}

TEST_F(HostBridgeTest, AHostThatDoesNotPollGetsTheConnectionClosedWith1013AtTheLimit) {
    start({.max_events = 8, .max_bytes = 4096});
    TcpClient client{server_->port()};
    ASSERT_TRUE(client.upgrade());
    for (int i = 0; i < 40; ++i) {  // far more than the queue can hold; the host never polls
        if (!client.send_frame(Opcode::Text, "m")) {
            break;
        }
    }
    std::optional<WireFrame> frame;
    do {
        frame = client.read_frame();
    } while (frame && frame->opcode != Opcode::Close);
    ASSERT_TRUE(frame.has_value());
    ASSERT_GE(frame->payload.size(), 2U);
    EXPECT_EQ(static_cast<unsigned char>(frame->payload[1]), 1013U & 0xFFU);
    ASSERT_TRUE(
        client.send_frame(Opcode::Close, frame->payload.substr(0, 2)));  // finish the handshake

    // Whatever was kept is bounded by the limit and the Close is still delivered last.
    std::size_t events = 0;
    std::optional<HostEvent> last;
    while (auto event = next(1000ms)) {
        ++events;
        last = std::move(event);
    }
    EXPECT_LE(events, 8U);
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(last->type, HostEventType::Close);
    EXPECT_GT(bridge_->events().dropped(), 0U);
}

TEST_F(HostBridgeTest, EveryOpenIsFollowedByExactlyOneCloseAcrossChurn) {
    start();
    constexpr int kClients = 25;
    for (int i = 0; i < kClients; ++i) {
        TcpClient client{server_->port()};
        ASSERT_TRUE(client.upgrade());
        ASSERT_TRUE(client.send_frame(Opcode::Text, "hi"));
        if (i % 2 == 0) {
            ASSERT_TRUE(client.send_frame(Opcode::Close, std::string{"\x03\xE8", 2}));
            static_cast<void>(client.read_frame());
        }
    }  // the odd ones just drop the socket
    std::map<ConnectionId, std::vector<HostEventType>> seen;
    std::size_t closes = 0;
    while (closes < kClients) {
        const auto event = next();
        ASSERT_TRUE(event.has_value()) << "timed out after " << closes << " closes";
        seen[event->conn].push_back(event->type);
        closes += event->type == HostEventType::Close ? 1U : 0U;
    }
    EXPECT_EQ(seen.size(), static_cast<std::size_t>(kClients));
    for (const auto& [id, types] : seen) {
        ASSERT_EQ(types.size(), 3U) << "connection " << id;
        EXPECT_EQ(types.front(), HostEventType::Open);
        EXPECT_EQ(types[1], HostEventType::Text);
        EXPECT_EQ(types.back(), HostEventType::Close);
    }
}

TEST_F(HostBridgeTest, ManyHostThreadsSendingToOneConnectionLoseNothing) {
    start();
    TcpClient client{server_->port()};
    ASSERT_TRUE(client.upgrade());
    const auto open = next();
    ASSERT_TRUE(open.has_value());
    constexpr int kThreads = 4;
    constexpr int kEach = 50;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kEach; ++i) {
                EXPECT_TRUE(bridge_->send_text(open->conn, "ping").has_value());
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    for (int i = 0; i < kThreads * kEach; ++i) {
        const auto frame = client.read_frame();
        ASSERT_TRUE(frame.has_value()) << "frame " << i;
        EXPECT_EQ(frame->payload, "ping");
    }
}

}  // namespace

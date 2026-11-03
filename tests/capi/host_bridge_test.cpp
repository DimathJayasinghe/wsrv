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

#include "capi/handshake_blob.hpp"

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
            if (auto event = bridge_->poll()) {
                return event;
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

/// Send `count` one-byte texts, one write each.
void send_texts(TcpClient& client, int count) {
    for (int i = 0; i < count; ++i) {
        ASSERT_TRUE(client.send_frame(Opcode::Text, "m"));
    }
}

TEST_F(HostBridgeTest, ASlowHostPausesTheConnectionAndDrainingItResumesWithoutLoss) {
    start({.max_events = 16, .max_bytes = 4096});  // high water 12, low water 8, hard cap 16
    TcpClient client{server_->port()};
    ASSERT_TRUE(client.upgrade());
    send_texts(client, 11);  // Open + reserved Close + 11 = 13: past high water, under the cap
    for (int i = 0; i < 300 && !bridge_->events().pressured(); ++i) {
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_TRUE(bridge_->events().pressured());
    std::this_thread::sleep_for(100ms);  // let the pause take effect on the loop

    send_texts(client, 3);  // sent while paused: they wait in the kernel, not in the queue
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(bridge_->events().pending(), 12U);  // the Open and the first 11 texts, no more

    std::size_t texts = 0;
    for (int total = 0; total < 15; ++total) {  // draining through poll() resumes the connection
        const auto event = next();
        ASSERT_TRUE(event.has_value()) << "stuck after " << total << " events";
        texts += event->type == HostEventType::Text ? 1U : 0U;
    }
    EXPECT_EQ(texts, 14U);  // nothing lost
    EXPECT_EQ(bridge_->events().dropped(), 0U);
    EXPECT_FALSE(next(200ms).has_value());  // and nothing closed the connection
}

TEST_F(HostBridgeTest, ThePauseAndResumeCycleRepeats) {
    start({.max_events = 16, .max_bytes = 4096});
    TcpClient client{server_->port()};
    ASSERT_TRUE(client.upgrade());
    std::size_t received = 0;
    for (int round = 0; round < 5; ++round) {
        send_texts(client, 11);
        for (int i = 0; i < 300 && !bridge_->events().pressured(); ++i) {
            std::this_thread::sleep_for(10ms);
        }
        std::this_thread::sleep_for(50ms);
        while (auto event = next(400ms)) {  // poll() resumes the connection as it drains
            received += event->type == HostEventType::Text ? 1U : 0U;
        }
    }
    EXPECT_EQ(received, 55U);
    EXPECT_EQ(bridge_->events().dropped(), 0U);
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

// ---- the asynchronous handshake decision (ADR-0018) -------------------------------------------

/// A bridge that is also the server's decider, as the embedded server wires it.
class HostBridgeHookTest : public HostBridgeTest {
protected:
    void start_hook(EventQueueConfig queue = {},
                    std::chrono::milliseconds decision_timeout = 5000ms,
                    std::size_t max_pending = 256) {
        auto made = HostBridge::make(sys_, queue);
        ASSERT_TRUE(made.has_value());
        bridge_ = std::move(*made);
        ServerBuilder builder;
        builder.listen("127.0.0.1", 0).handler_factory(*bridge_).handshake_decider(*bridge_);
        builder.config().handshake.allowed_origins = {"https://app.example.com"};
        builder.config().handshake.subprotocols = {"chat", "v2"};
        builder.config().handshake_forward_headers = {"Cookie"};
        builder.config().handshake_decision_timeout = decision_timeout;
        builder.config().max_pending_handshakes = max_pending;
        server_ = std::make_unique<RunningServer>(std::move(builder));
    }

    static std::string upgrade_with_cookie() {
        std::string request = TcpClient::upgrade_request();
        request.insert(request.size() - 2,
                       "Cookie: session=abc\r\nSec-WebSocket-Protocol: chat, v2\r\n");
        return request;
    }
};

TEST_F(HostBridgeHookTest, TheHostIsToldWhoAsksAndItsAcceptLetsTheClientIn) {
    start_hook();
    TcpClient client{server_->port()};
    ASSERT_TRUE(client.send(upgrade_with_cookie()));
    const auto event = next();
    ASSERT_TRUE(event.has_value());
    ASSERT_EQ(event->type, HostEventType::Handshake);
    const auto view = decode_handshake(ByteView{event->data});
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->path, "/chat");
    EXPECT_EQ(view->origin, "https://app.example.com");
    ASSERT_EQ(view->headers.size(), 1U);
    EXPECT_EQ(view->headers[0].second, "session=abc");
    EXPECT_EQ(view->offered.size(), 2U);
    EXPECT_EQ(view->peer.rfind("127.0.0.1:", 0), 0U);

    ASSERT_TRUE(bridge_->accept_handshake(event->conn, "v2").has_value());
    EXPECT_NE(client.read_http_head().find("Sec-WebSocket-Protocol: v2"), std::string::npos);
    const auto open = next();
    ASSERT_TRUE(open.has_value());
    EXPECT_EQ(open->type, HostEventType::Open);
    EXPECT_EQ(open->conn, event->conn) << "the same id from the question to the connection";
}

TEST_F(HostBridgeHookTest, RejectAnswersWithThatStatusAndTheConnectionNeverOpens) {
    start_hook();
    TcpClient client{server_->port()};
    ASSERT_TRUE(client.send(upgrade_with_cookie()));
    const auto event = next();
    ASSERT_TRUE(event.has_value());
    ASSERT_TRUE(bridge_->reject_handshake(event->conn, 401).has_value());
    EXPECT_EQ(client.read_http_head().rfind("HTTP/1.1 401 ", 0), 0U);
    EXPECT_FALSE(next(200ms).has_value()) << "no Open and no Close: it never was a connection";
}

TEST_F(HostBridgeHookTest, AnAnswerIsAcceptedOnceAndTheSecondOneIsNoSuchConnection) {
    start_hook();
    TcpClient client{server_->port()};
    ASSERT_TRUE(client.send(upgrade_with_cookie()));
    const auto event = next();
    ASSERT_TRUE(event.has_value());
    ASSERT_TRUE(bridge_->accept_handshake(event->conn, std::nullopt).has_value());
    const auto again = bridge_->reject_handshake(event->conn, 403);
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error().code, ErrorCode::Closing);
    EXPECT_EQ(bridge_->accept_handshake(999999, std::nullopt).error().code, ErrorCode::Closing);
}

TEST_F(HostBridgeHookTest, AMistakeIsReportedAndTheWaitGoesOn) {
    start_hook();
    TcpClient client{server_->port()};
    ASSERT_TRUE(client.send(upgrade_with_cookie()));
    const auto event = next();
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(bridge_->accept_handshake(event->conn, "nope").error().code,
              ErrorCode::InvalidArgument);
    EXPECT_EQ(bridge_->reject_handshake(event->conn, 302).error().code, ErrorCode::InvalidArgument);
    EXPECT_TRUE(bridge_->accept_handshake(event->conn, "chat").has_value()) << "still waiting";
    EXPECT_EQ(client.read_http_head().rfind("HTTP/1.1 101 ", 0), 0U);
}

TEST_F(HostBridgeHookTest, AfterTheServerGaveUpTheAnswerIsNoSuchConnectionNotSilentSuccess) {
    start_hook({}, 200ms);
    TcpClient client{server_->port()};
    ASSERT_TRUE(client.send(upgrade_with_cookie()));
    const auto event = next();
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(client.read_http_head().rfind("HTTP/1.1 503 ", 0), 0U);  // timed out
    for (int i = 0; i < 100 && bridge_->waiting_handshakes() != 0; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_EQ(bridge_->waiting_handshakes(), 0U) << "withdrawn() cleans the entry";
    // The withdrawal runs on the loop thread just after the 503 was written.
    Result<void> late = bridge_->accept_handshake(event->conn, std::nullopt);
    for (int i = 0; i < 100 && late.has_value(); ++i) {
        std::this_thread::sleep_for(10ms);
        late = bridge_->accept_handshake(event->conn, std::nullopt);
    }
    ASSERT_FALSE(late.has_value());
    EXPECT_EQ(late.error().code, ErrorCode::Closing);
}

TEST_F(HostBridgeHookTest, AFullEventQueueMeansA503NotAnAcceptance) {
    EventQueueConfig tiny;
    tiny.max_events = 2;  // the smallest queue there is
    start_hook(tiny);
    TcpClient first{server_->port()};
    TcpClient second{server_->port()};
    ASSERT_TRUE(first.send(upgrade_with_cookie()));
    ASSERT_TRUE(second.send(upgrade_with_cookie()));
    for (int i = 0; i < 200 && bridge_->events().pending() < 2; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_EQ(bridge_->events().pending(), 2U);  // the host has not polled yet: the queue is full
    TcpClient third{server_->port()};
    ASSERT_TRUE(third.send(upgrade_with_cookie()));
    EXPECT_EQ(third.read_http_head().rfind("HTTP/1.1 503 ", 0), 0U);
    EXPECT_EQ(bridge_->waiting_handshakes(), 2U) << "the refused one left nothing behind";
    const auto event = next();  // the first two are still waiting and still answerable
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(bridge_->accept_handshake(event->conn, std::nullopt).has_value());
}

}  // namespace

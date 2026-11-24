// SPDX-License-Identifier: Apache-2.0
#include "capi/host_bridge.hpp"

#include "base/clock.hpp"
#include "net/event_loop.hpp"
#include "net/syscalls.hpp"
#include "support/recording_session.hpp"
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
    void start(EventQueueConfig queue = {}, std::optional<BrokerConfig> broker = std::nullopt) {
        auto made = HostBridge::make(
            sys_, queue, broker ? std::make_shared<Broker>(*broker) : std::shared_ptr<Broker>{});
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
// ---- a resume the loop could not take
// --------------------------------------------------------------

/// A Session that only counts pauses and resumes, and whose handle reaches a loop that can be made
/// to refuse tasks (its queue is full). No server, no sockets: the bridge's own bookkeeping.
class PausableSession final : public Session {
public:
    PausableSession(ConnectionId id, std::shared_ptr<LoopLink> link)
        : id_{id}, link_{std::move(link)} {}
    [[nodiscard]] ConnectionId id() const noexcept override {
        return id_;
    }
    [[nodiscard]] const Endpoint& peer() const noexcept override {
        return peer_;
    }
    [[nodiscard]] Result<void> send_text(std::string_view /*text*/) override {
        return {};
    }
    [[nodiscard]] Result<void> send_binary(ByteView /*data*/) override {
        return {};
    }
    [[nodiscard]] Result<MessageWriter> begin_text() override {
        return fail(ErrorCode::Closing, "stub");
    }
    [[nodiscard]] Result<MessageWriter> begin_binary() override {
        return fail(ErrorCode::Closing, "stub");
    }
    void pause_reads() override {
        ++pauses;
    }
    void resume_reads() override {
        ++resumes;
    }
    void close(CloseCode /*code*/, std::string_view /*reason*/) override {}
    [[nodiscard]] SessionHandle handle() const override {
        return SessionHandle{link_, id_};
    }

    int pauses = 0;
    int resumes = 0;

private:
    ConnectionId id_;
    Endpoint peer_;
    std::shared_ptr<LoopLink> link_;
};

class OneSessionDirectory final : public ISessionDirectory {
public:
    void run(ConnectionId /*id*/, const SessionCommand& command) override {
        command(*session);
    }
    void decide(ConnectionId /*id*/, const HandshakeVerdict& /*verdict*/) override {}
    Session* session = nullptr;
};

// The reviewer's claim (read from the code): resume_parked() forgot a connection whose resume the
// loop refused (task queue full), so it stayed read-paused until the idle or pong timer ended it.
TEST(HostBridgeResume, AResumeTheLoopRefusedIsRetriedByTheNextPoll) {
    LinuxSyscalls sys;
    SteadyClock clock;
    EventLoopConfig loop_config;
    loop_config.max_queued_tasks = 1;
    auto loop = EventLoop::create(sys, clock, loop_config);
    ASSERT_TRUE(loop.has_value());
    OneSessionDirectory directory;
    auto link = std::make_shared<LoopLink>(**loop, directory);
    auto bridge = HostBridge::make(sys, {.max_events = 16, .max_bytes = 4096});
    ASSERT_TRUE(bridge.has_value());
    PausableSession session{7, link};
    directory.session = &session;

    auto handler = (*bridge)->create(
        ConnectionInfo{.id = 7, .peer = {}, .subprotocol = {}, .annotations = {}});
    handler->on_open(session);
    for (int i = 0; i < 11; ++i) {
        handler->on_text(session, "m");
    }
    ASSERT_TRUE((*bridge)->events().pressured());
    ASSERT_GE(session.pauses, 1) << "the slow host paused the connection";

    ASSERT_TRUE(
        (*loop)->post([] {}).has_value());  // the loop's only slot is taken: a resume is refused
    while ((*bridge)->poll()) {
        // the host drains the queue; the resume it triggers is refused with BufferFull
    }
    EXPECT_EQ(session.resumes, 0);

    ASSERT_TRUE((*loop)->run_once(10ms).has_value());  // the loop catches up: the slot is free
    EXPECT_FALSE((*bridge)->poll().has_value());  // the host asks again; the safety net must retry
    ASSERT_TRUE((*loop)->run_once(10ms).has_value());
    EXPECT_EQ(session.resumes, 1) << "the connection must not stay paused";

    handler->on_close(session, CloseInfo{});
}

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

TEST_F(HostBridgeHookTest, WhileTheIntakeIsClosedUpgradesGet503AtOnceAndNothingIsQueued) {
    start_hook();
    bridge_->set_handshake_intake(false);  // no application is listening (wsrvd, ADR-0018)
    TcpClient client{server_->port()};
    ASSERT_TRUE(client.send(upgrade_with_cookie()));
    EXPECT_EQ(client.read_http_head().rfind("HTTP/1.1 503 ", 0), 0U);
    EXPECT_FALSE(next(100ms).has_value()) << "no question was queued for a host that is not there";
    EXPECT_EQ(bridge_->waiting_handshakes(), 0U);

    bridge_->set_handshake_intake(true);  // the application is back: questions flow again
    TcpClient again{server_->port()};
    ASSERT_TRUE(again.send(upgrade_with_cookie()));
    const auto event = next();
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, HostEventType::Handshake);
}

TEST_F(HostBridgeHookTest, ClosingTheIntakeRefusesWhatIsWaitingWith503AndSaysSoToLateAnswers) {
    start_hook();
    TcpClient client{server_->port()};
    ASSERT_TRUE(client.send(upgrade_with_cookie()));
    const auto event = next();
    ASSERT_TRUE(event.has_value());
    bridge_->set_handshake_intake(false);  // the application that was asked is gone
    EXPECT_EQ(client.read_http_head().rfind("HTTP/1.1 503 ", 0), 0U) << "fail closed, at once";
    EXPECT_EQ(bridge_->waiting_handshakes(), 0U);
    EXPECT_EQ(bridge_->accept_handshake(event->conn, std::nullopt).error().code,
              ErrorCode::Closing);
}

// --- the broker through the bridge (ADR-0027 section 4) ---

TEST_F(HostBridgeTest, ASubscribedConnectionReceivesWhatIsPublishedToItsTopic) {
    start({}, BrokerConfig{});
    TcpClient subscribed{server_->port()};
    TcpClient bystander{server_->port()};
    ASSERT_TRUE(subscribed.upgrade());
    const auto open = next();
    ASSERT_TRUE(open.has_value());
    ASSERT_TRUE(bystander.upgrade());
    ASSERT_TRUE(next().has_value());  // the bystander's Open

    ASSERT_TRUE(bridge_->subscribe(open->conn, "room.1").has_value());
    EXPECT_EQ(bridge_->topic_subscribers("room.1"), 1U);
    const auto report = bridge_->publish_text("room.1", "hello room");
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->subscribers, 1U);
    EXPECT_EQ(report->accepted, 1U);
    const auto frame = subscribed.read_frame();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->payload, "hello room");
    EXPECT_TRUE(bystander.silent_for(200ms));  // not subscribed, nothing arrives
}

TEST_F(HostBridgeTest, ThePublisherCanBeLeftOutAndAConnectionCanLeaveATopic) {
    start({}, BrokerConfig{});
    TcpClient a{server_->port()};
    TcpClient b{server_->port()};
    ASSERT_TRUE(a.upgrade());
    const auto open_a = next();
    ASSERT_TRUE(b.upgrade());
    const auto open_b = next();
    ASSERT_TRUE(open_a && open_b);
    ASSERT_TRUE(bridge_->subscribe(open_a->conn, "t").has_value());
    ASSERT_TRUE(bridge_->subscribe(open_b->conn, "t").has_value());

    const auto report = bridge_->publish_text("t", "not for a", open_a->conn);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->subscribers, 1U);
    ASSERT_TRUE(b.read_frame().has_value());
    EXPECT_TRUE(a.silent_for(200ms));

    ASSERT_TRUE(bridge_->unsubscribe(open_b->conn, "t").has_value());
    EXPECT_EQ(bridge_->topic_subscribers("t"), 1U);
}

TEST_F(HostBridgeTest, ACommandForAnUnknownConnectionIsRefusedAndSubscribesNothing) {
    start({}, BrokerConfig{});
    const auto result = bridge_->subscribe(424242, "room");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::Closing);
    EXPECT_EQ(bridge_->topic_subscribers("room"), 0U);
}

TEST_F(HostBridgeTest, ClosingAConnectionEndsItsSubscriptionsWithoutHelpFromTheHost) {
    start({}, BrokerConfig{});
    TcpClient client{server_->port()};
    ASSERT_TRUE(client.upgrade());
    const auto open = next();
    ASSERT_TRUE(open.has_value());
    ASSERT_TRUE(bridge_->subscribe(open->conn, "a").has_value());
    ASSERT_TRUE(bridge_->subscribe(open->conn, "b").has_value());
    client.close();
    const auto close = next();
    ASSERT_TRUE(close.has_value());
    EXPECT_EQ(close->type, HostEventType::Close);
    EXPECT_EQ(bridge_->topic_subscribers("a"), 0U);
    EXPECT_EQ(bridge_->topic_subscribers("b"), 0U);
}

TEST_F(HostBridgeTest, AnOverBoundSubscriptionIsALimitAndChangesNothing) {
    BrokerConfig tiny;
    tiny.max_topics_per_connection = 1;
    start({}, tiny);
    TcpClient client{server_->port()};
    ASSERT_TRUE(client.upgrade());
    const auto open = next();
    ASSERT_TRUE(open.has_value());
    ASSERT_TRUE(bridge_->subscribe(open->conn, "one").has_value());
    const auto second = bridge_->subscribe(open->conn, "two");
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code, ErrorCode::BufferFull);
    EXPECT_EQ(bridge_->topic_subscribers("two"), 0U);
}

TEST_F(HostBridgeTest, WithTheBrokerOffEveryBrokerCommandIsInvalidArgument) {
    start();
    TcpClient client{server_->port()};
    ASSERT_TRUE(client.upgrade());
    const auto open = next();
    ASSERT_TRUE(open.has_value());
    const auto sub = bridge_->subscribe(open->conn, "x");
    ASSERT_FALSE(sub.has_value());
    EXPECT_EQ(sub.error().code, ErrorCode::InvalidArgument);
    const auto pub = bridge_->publish_text("x", "y");
    ASSERT_FALSE(pub.has_value());
    EXPECT_EQ(pub.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(bridge_->topic_subscribers("x"), 0U);
}

// The race of ADR-0027 section 4: a host thread subscribes while the connection closes. Either the
// subscription is complete before the close cleans up, or the subscribe finds no connection; no
// subscription may outlive its connection. Run under TSan in CI.
TEST_F(HostBridgeTest, SubscribingWhileTheConnectionClosesNeverLeaksASubscription) {
    start({}, BrokerConfig{});
    for (int round = 0; round < 40; ++round) {
        TcpClient client{server_->port()};
        ASSERT_TRUE(client.upgrade());
        const auto open = next();
        ASSERT_TRUE(open.has_value());
        std::thread host([&] {
            for (int i = 0; i < 20; ++i) {
                static_cast<void>(bridge_->subscribe(open->conn, "race").has_value());
            }
        });
        client.close();
        host.join();
        const auto close = next();
        ASSERT_TRUE(close.has_value());
        EXPECT_EQ(close->type, HostEventType::Close);
        EXPECT_EQ(bridge_->topic_subscribers("race"), 0U) << "round " << round;
    }
}

// A host that waits on the descriptor alone, as wsrvd does, must be woken by the loop's own end of
// iteration. next() would hide a missing hook: it falls back to polling the queue after its
// timeout.
TEST_F(HostBridgeTest, ARealLoopWakesAHostThatOnlyWaitsOnTheDescriptor) {
    start();
    TcpClient client{server_->port()};
    ASSERT_TRUE(client.upgrade());
    ASSERT_TRUE(next().has_value());  // the Open
    ASSERT_TRUE(client.send_frame(Opcode::Text, "ping"));
    pollfd p{bridge_->events().event_fd(), POLLIN, 0};
    ASSERT_EQ(::poll(&p, 1, 1500), 1) << "no wake-up: the loop never paid the owed one";
    const auto text = bridge_->poll();
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(text->type, HostEventType::Text);
}

// ---- the wake-up of the host is paid once per loop iteration (ADR-0035)
// --------------------------

/// A bridge driven by hand, without a server: the handler is called the way a loop would.
class HostBridgeWakeTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto made = HostBridge::make(sys_, {}, std::shared_ptr<Broker>{});
        ASSERT_TRUE(made.has_value());
        bridge_ = std::move(*made);
        ConnectionInfo info;
        info.id = 1;
        handler_ = bridge_->create(info);
        ASSERT_NE(handler_, nullptr);
    }

    [[nodiscard]] bool readable() const {
        pollfd p{bridge_->events().event_fd(), POLLIN, 0};
        return ::poll(&p, 1, 0) == 1 && (p.revents & POLLIN) != 0;
    }

    LinuxSyscalls sys_;
    std::unique_ptr<HostBridge> bridge_;
    std::unique_ptr<IWebSocketHandler> handler_;
    RecordingSession session_{1};
};

TEST_F(HostBridgeWakeTest, AnOpenWakesTheHostAtOnce) {
    handler_->on_open(session_);
    EXPECT_TRUE(readable()) << "rare events are not held back";
    ASSERT_TRUE(bridge_->poll().has_value());
}

TEST_F(HostBridgeWakeTest, MessagesWaitForTheEndOfTheIterationAndThenWakeTheHostOnce) {
    handler_->on_open(session_);
    ASSERT_TRUE(bridge_->poll().has_value());  // the Open; the queue is empty again
    ASSERT_FALSE(readable());
    handler_->on_text(session_, "one");
    const std::byte payload[] = {std::byte{1}, std::byte{2}};
    handler_->on_binary(session_, ByteView{payload});
    EXPECT_EQ(bridge_->events().pending(), 2U);
    EXPECT_FALSE(readable()) << "a message does not wake the host by itself";
    bridge_->on_loop_iteration_end();
    EXPECT_TRUE(readable()) << "the end of the iteration does, once for the whole burst";
    const auto first = bridge_->poll();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->type, HostEventType::Text);
    const auto second = bridge_->poll();
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->type, HostEventType::Binary);
    EXPECT_FALSE(readable());
}

TEST_F(HostBridgeWakeTest, ACloseAndAnErrorWakeTheHostAtOnceLikeAnOpen) {
    handler_->on_open(session_);
    ASSERT_TRUE(bridge_->poll().has_value());
    ASSERT_FALSE(readable());
    handler_->on_error(session_, Error{ErrorCode::Io, "peer reset"});
    EXPECT_TRUE(readable()) << "an Error is a rare event and is not deferred";
    ASSERT_TRUE(bridge_->poll().has_value());
    ASSERT_FALSE(readable());
    handler_->on_close(session_, CloseInfo{CloseCode::GoingAway, "bye", true});
    EXPECT_TRUE(readable()) << "a Close is never held back, it may come outside an iteration";
    const auto closed = bridge_->poll();
    ASSERT_TRUE(closed.has_value());
    EXPECT_EQ(closed->type, HostEventType::Close);
}

TEST_F(HostBridgeWakeTest, ACloseQueuedBehindAnOwedMessageIsPaidByThatMessagesFlush) {
    handler_->on_open(session_);
    ASSERT_TRUE(bridge_->poll().has_value());
    handler_->on_text(session_, "owes the wake");
    // The queue is not empty, so the Close writes nothing of its own.
    handler_->on_close(session_, CloseInfo{CloseCode::GoingAway, "", true});
    EXPECT_FALSE(readable());
    bridge_->on_loop_iteration_end();
    EXPECT_TRUE(readable()) << "the loop that owes the wake-up pays it for the Close too";
    EXPECT_EQ(bridge_->events().pending(), 2U);
}

TEST_F(HostBridgeWakeTest, AHostThatPollsBeforeTheEndOfTheIterationLosesNothing) {
    handler_->on_open(session_);
    ASSERT_TRUE(bridge_->poll().has_value());
    handler_->on_text(session_, "early");
    ASSERT_TRUE(bridge_->poll().has_value());  // the host was awake and took it
    bridge_->on_loop_iteration_end();
    EXPECT_FALSE(readable()) << "no wake-up for an empty queue";
    handler_->on_text(session_, "next");
    bridge_->on_loop_iteration_end();
    EXPECT_TRUE(readable());
}
}  // namespace

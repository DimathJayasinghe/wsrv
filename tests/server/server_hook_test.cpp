// SPDX-License-Identifier: Apache-2.0
// The asynchronous handshake decision on a real server and real sockets (ADR-0018): the
// application answers from another thread, and every way the wait can end is checked.
#include "server/handshake_decider.hpp"
#include "server/server_builder.hpp"
#include "support/running_server.hpp"
#include "support/tcp_client.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using wsrv::ErrorCode;
using wsrv::Session;
using wsrv::test::RunningServer;
using wsrv::test::TcpClient;

class EchoHandler final : public wsrv::IWebSocketHandler {
public:
    void on_text(Session& session, std::string_view text) override {
        static_cast<void>(session.send_text(text).has_value());
    }
};

class EchoFactory final : public wsrv::IHandlerFactory {
public:
    std::atomic<int> created{0};
    std::unique_ptr<wsrv::IWebSocketHandler> create(const wsrv::ConnectionInfo&) override {
        ++created;
        return std::make_unique<EchoHandler>();
    }
};

/// The application's side, as the capi bridge will be: keeps what it was asked, answers later.
/// Thread-safe, because the server asks from its loop thread and the test answers from another.
class ThreadedDecider final : public wsrv::IHandshakeDecider {
public:
    wsrv::Result<void> request(wsrv::HandshakeRequest request,
                               wsrv::PendingHandle handle) override {
        const std::lock_guard lock{mutex_};
        if (full_) {
            return wsrv::fail(ErrorCode::BufferFull, "test decider: full");
        }
        requests_.push_back(std::move(request));
        handles_.push_back(std::move(handle));
        return {};
    }
    void set_full(bool full) {
        const std::lock_guard lock{mutex_};
        full_ = full;
    }
    std::size_t asked() {
        const std::lock_guard lock{mutex_};
        return requests_.size();
    }
    wsrv::HandshakeRequest request_at(std::size_t index) {
        const std::lock_guard lock{mutex_};
        return requests_.at(index);
    }
    wsrv::PendingHandle handle_at(std::size_t index) {
        const std::lock_guard lock{mutex_};
        return handles_.at(index);
    }
    bool wait_asked(std::size_t count) {
        for (int i = 0; i < 500; ++i) {
            if (asked() >= count) {
                return true;
            }
            std::this_thread::sleep_for(10ms);
        }
        return false;
    }

private:
    std::mutex mutex_;
    std::vector<wsrv::HandshakeRequest> requests_;
    std::vector<wsrv::PendingHandle> handles_;
    bool full_ = false;
};

struct HookServerTest : public ::testing::Test {
    EchoFactory factory;
    ThreadedDecider decider;

    wsrv::ServerBuilder builder(bool hook = true) {
        wsrv::ServerBuilder b;
        b.listen("127.0.0.1", 0).handler_factory(factory);
        if (hook) {
            b.handshake_decider(decider);
        }
        b.config().handshake.allowed_origins = {"https://app.example.com"};
        return b;
    }
};

TEST_F(HookServerTest, TheApplicationAcceptsFromAnotherThreadAndTheConnectionWorks) {
    RunningServer server{builder()};
    TcpClient client{server.port()};
    ASSERT_TRUE(client.send(TcpClient::upgrade_request()));
    ASSERT_TRUE(decider.wait_asked(1));
    EXPECT_TRUE(client.silent_for(200ms)) << "no 101 until the application agrees";
    EXPECT_EQ(factory.created.load(), 0);
    EXPECT_EQ(decider.request_at(0).path, "/chat");

    std::thread application([&] { EXPECT_TRUE(decider.handle_at(0).accept().has_value()); });
    application.join();
    EXPECT_EQ(client.read_http_head().rfind("HTTP/1.1 101 ", 0), 0U);
    ASSERT_TRUE(client.send_frame(wsrv::Opcode::Text, "hello"));
    const auto frame = client.read_frame();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->payload, "hello");
}

TEST_F(HookServerTest, TheApplicationRefusesAndTheClientGetsThatStatus) {
    RunningServer server{builder()};
    TcpClient client{server.port()};
    ASSERT_TRUE(client.send(TcpClient::upgrade_request()));
    ASSERT_TRUE(decider.wait_asked(1));
    ASSERT_TRUE(decider.handle_at(0).reject(401).has_value());
    EXPECT_EQ(client.read_http_head().rfind("HTTP/1.1 401 ", 0), 0U);
    EXPECT_TRUE(client.wait_for_eof());
    EXPECT_EQ(factory.created.load(), 0);
}

TEST_F(HookServerTest, NoAnswerInTimeIsA503AndALateAnswerIsHarmless) {  // EC-H29, EC-H30
    wsrv::ServerBuilder b = builder();
    b.config().handshake_decision_timeout = 200ms;
    RunningServer server{std::move(b)};
    TcpClient client{server.port()};
    ASSERT_TRUE(client.send(TcpClient::upgrade_request()));
    ASSERT_TRUE(decider.wait_asked(1));
    const std::string head = client.read_http_head();
    EXPECT_EQ(head.rfind("HTTP/1.1 503 ", 0), 0U);
    EXPECT_NE(head.find("Retry-After: 5"), std::string::npos);
    EXPECT_TRUE(client.wait_for_eof());
    EXPECT_TRUE(decider.handle_at(0).accept().has_value())
        << "accepted for delivery, dropped there";
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(factory.created.load(), 0);

    TcpClient next{server.port()};  // ids are never reused: the late answer cannot hit this one
    ASSERT_TRUE(next.send(TcpClient::upgrade_request()));
    ASSERT_TRUE(decider.wait_asked(2));
    EXPECT_TRUE(next.silent_for(100ms));
}

TEST_F(HookServerTest, UpgradesOverTheBoundAreRefusedAtOnceAndTheSlotComesBack) {  // EC-H31
    wsrv::ServerBuilder b = builder();
    b.config().max_pending_handshakes = 2;
    RunningServer server{std::move(b)};
    TcpClient first{server.port()};
    TcpClient second{server.port()};
    ASSERT_TRUE(first.send(TcpClient::upgrade_request()));
    ASSERT_TRUE(second.send(TcpClient::upgrade_request()));
    ASSERT_TRUE(decider.wait_asked(2));

    TcpClient third{server.port()};
    ASSERT_TRUE(third.send(TcpClient::upgrade_request()));
    EXPECT_EQ(third.read_http_head().rfind("HTTP/1.1 503 ", 0), 0U);
    EXPECT_EQ(decider.asked(), 2U) << "the third was never queued for the application";

    ASSERT_TRUE(decider.handle_at(0).reject(403).has_value());
    EXPECT_EQ(first.read_http_head().rfind("HTTP/1.1 403 ", 0), 0U);
    TcpClient fourth{server.port()};  // a place is free again
    ASSERT_TRUE(fourth.send(TcpClient::upgrade_request()));
    ASSERT_TRUE(decider.wait_asked(3));
}

TEST_F(HookServerTest, ADeciderThatCannotTakeTheRequestMeansA503NotAnAcceptance) {
    decider.set_full(true);
    RunningServer server{builder()};
    TcpClient client{server.port()};
    ASSERT_TRUE(client.send(TcpClient::upgrade_request()));
    EXPECT_EQ(client.read_http_head().rfind("HTTP/1.1 503 ", 0), 0U);
    EXPECT_EQ(factory.created.load(), 0);
}

TEST_F(HookServerTest, AClientThatLeavesWhileWaitingFreesItsPlaceLongBeforeTheTimeout) {  // EC-H33
    wsrv::ServerBuilder b = builder();
    b.config().max_pending_handshakes = 1;
    b.config().handshake_decision_timeout = 30s;  // so the timeout cannot be what freed the place
    RunningServer server{std::move(b)};
    {
        TcpClient leaver{server.port()};
        ASSERT_TRUE(leaver.send(TcpClient::upgrade_request()));
        ASSERT_TRUE(decider.wait_asked(1));
    }  // closed: a FIN arrives while the application thinks
    // The server notices the FIN on its loop thread a moment later; until then a newcomer is told
    // 503. Try again until one is admitted (it then stays silent, waiting for the application).
    std::vector<std::unique_ptr<TcpClient>> probes;
    bool admitted = false;
    for (int i = 0; i < 50 && !admitted; ++i) {
        probes.push_back(std::make_unique<TcpClient>(server.port()));
        ASSERT_TRUE(probes.back()->send(TcpClient::upgrade_request()));
        admitted = probes.back()->silent_for(100ms) && decider.asked() >= 2;
    }
    EXPECT_TRUE(admitted) << "the leaver's place must come back without waiting for the timeout";
}

TEST_F(HookServerTest, ShuttingDownWhileWaitingAnswers503) {  // EC-H32
    RunningServer server{builder()};
    TcpClient client{server.port()};
    ASSERT_TRUE(client.send(TcpClient::upgrade_request()));
    ASSERT_TRUE(decider.wait_asked(1));
    ASSERT_TRUE(server.server().request_shutdown().has_value());  // the graceful path (FR-S5)
    EXPECT_EQ(client.read_http_head().rfind("HTTP/1.1 503 ", 0), 0U);
}

TEST_F(HookServerTest, AnAnswerAfterTheServerIsGoneReportsClosing) {
    wsrv::PendingHandle handle;
    {
        RunningServer server{builder()};
        TcpClient client{server.port()};
        ASSERT_TRUE(client.send(TcpClient::upgrade_request()));
        ASSERT_TRUE(decider.wait_asked(1));
        handle = decider.handle_at(0);
    }
    const auto result = handle.accept();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::Closing);
}

TEST_F(HookServerTest, WithoutADeciderTheServerAcceptsAtOnceAsBefore) {  // the hook is opt-in
    RunningServer server{builder(false)};
    TcpClient client{server.port()};
    EXPECT_TRUE(client.upgrade());
    EXPECT_EQ(decider.asked(), 0U);
}

}  // namespace

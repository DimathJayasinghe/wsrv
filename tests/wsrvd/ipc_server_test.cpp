// SPDX-License-Identifier: Apache-2.0
#include "wsrvd/ipc_server.hpp"

#include "base/byte_buffer.hpp"
#include "base/clock.hpp"
#include "support/fake_backend.hpp"
#include "support/fake_syscalls.hpp"
#include "support/ipc_client.hpp"
#include "support/tcp_client.hpp"
#include "support/wire.hpp"
#include "wsrv/wsrv.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "capi/embedded_server.hpp"
#include "capi/handshake_blob.hpp"
#include "ipc/frame_decoder.hpp"
#include "ipc/message.hpp"
#include "wsrvd/embedded_backend.hpp"

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using wsrv::ByteView;
using wsrv::ErrorCode;
using wsrv::EventLoop;
using wsrv::SteadyClock;
using wsrv::test::Call;
using wsrv::test::Client;
using wsrv::test::FakeSyscalls;
using wsrv::test::header;
using wsrv::test::Reply;
using wsrv::test::TempDir;
using wsrv::wsrvd::IpcServer;
using wsrv::wsrvd::IpcServerConfig;
namespace ipc = wsrv::ipc;

constexpr std::uint32_t kMaxFrame = 8192;

/// The server running on its own thread, as EmbeddedServer does: the loop and the server are
/// created, run and destroyed on that thread; the test thread only talks to the socket.
class RunningServer {
public:
    RunningServer(FakeSyscalls& sys, wsrv::wsrvd::IBackend& backend, IpcServerConfig config) {
        std::promise<std::optional<ErrorCode>> started;
        auto started_future = started.get_future();
        thread_ =
            std::thread([this, &sys, &backend, config = std::move(config), &started]() mutable {
                const SteadyClock clock;
                auto loop = EventLoop::create(sys, clock);
                if (!loop) {
                    started.set_value(loop.error().code);
                    return;
                }
                auto server = IpcServer::create(**loop, sys, backend, std::move(config));
                if (!server) {
                    started.set_value(server.error().code);
                    return;
                }
                {
                    const std::lock_guard<std::mutex> lock{mutex_};
                    loop_ = loop->get();
                    endpoint_ = (*server)->tcp_endpoint();
                }
                started.set_value(std::nullopt);
                const wsrv::Result<void> ran = (*loop)->run();
                static_cast<void>(ran);
                {
                    // stop() calls the loop while holding this lock, so once we hold it no other
                    // thread is inside the loop and none can start: only then may it be destroyed.
                    const std::lock_guard<std::mutex> lock{mutex_};
                    loop_ = nullptr;
                }
                server->reset();  // before the loop, as the daemon does
            });
        failure_ = started_future.get();
    }
    RunningServer(const RunningServer&) = delete;
    RunningServer& operator=(const RunningServer&) = delete;
    ~RunningServer() {
        stop();
    }
    void stop() {
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            if (loop_ != nullptr) {
                loop_->stop();
            }
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    [[nodiscard]] const std::optional<ErrorCode>& failure() const {
        return failure_;
    }
    /// The TCP address actually bound (the real port when 0 was asked); nothing for a Unix socket.
    [[nodiscard]] std::optional<wsrv::Endpoint> endpoint() const {
        const std::lock_guard<std::mutex> lock{mutex_};
        return endpoint_;
    }

private:
    std::thread thread_;
    mutable std::mutex mutex_;
    EventLoop* loop_ = nullptr;               // guarded by mutex_
    std::optional<wsrv::Endpoint> endpoint_;  // guarded by mutex_
    std::optional<ErrorCode> failure_;
};

/// A logger the loop thread writes and the test thread reads (the recording logger of the other
/// tests is single-threaded).
class SharedLogger final : public wsrv::ILogger {
public:
    using ILogger::log;
    void log(const wsrv::LogRecord& record) override {
        const std::lock_guard lock{mutex_};
        lines_.push_back(std::string{record.event} + "|" +
                         std::to_string(static_cast<int>(record.level)) + "|" +
                         std::string{record.message});
    }
    /// Wait (the server runs on its own thread) until a line of `event` exists; returns it or "".
    [[nodiscard]] std::string wait_for(std::string_view event) {
        for (int attempt = 0; attempt < 300; ++attempt) {
            {
                const std::lock_guard lock{mutex_};
                for (const std::string& line : lines_) {
                    if (line.rfind(std::string{event} + "|", 0) == 0) {
                        return line;
                    }
                }
            }
            std::this_thread::sleep_for(10ms);
        }
        return {};
    }
    [[nodiscard]] std::string everything() {
        const std::lock_guard lock{mutex_};
        std::string all;
        for (const std::string& line : lines_) {
            all += line + "\n";
        }
        return all;
    }

private:
    std::mutex mutex_;
    std::vector<std::string> lines_;
};

struct IpcServerTest : public ::testing::Test {
    FakeSyscalls sys;
    wsrv::testing::FakeBackend backend;
    TempDir dir;
    SharedLogger logs;

    [[nodiscard]] IpcServerConfig config(std::string token = "") const {
        IpcServerConfig c;
        c.socket_path = dir.file("ipc.sock");
        c.token = std::move(token);
        c.max_frame = kMaxFrame;
        c.hello_timeout = 5000ms;
        c.logger = &const_cast<SharedLogger&>(logs);
        return c;
    }
    [[nodiscard]] std::string path() const {
        return dir.file("ipc.sock");
    }
};

constexpr auto kInvalidArgument = static_cast<std::uint8_t>(WSRV_ERR_INVALID_ARGUMENT);
constexpr auto kBackpressure = static_cast<std::uint8_t>(WSRV_ERR_BACKPRESSURE);

// ---- the handshake ----------------------------------------------------------------------------

TEST_F(IpcServerTest, HelloIsAnsweredWithHelloOkCarryingTheFrameLimit) {
    const RunningServer server{sys, backend, config("s3cret")};
    ASSERT_FALSE(server.failure());
    Client client{path()};
    ASSERT_TRUE(client.connected());
    client.send_hello("s3cret");
    const auto reply = client.receive();
    ASSERT_TRUE(reply);
    EXPECT_EQ(reply->kind, Reply::Kind::HelloOk);
    EXPECT_EQ(reply->max_frame, kMaxFrame);
}

TEST_F(IpcServerTest, AWrongTokenGetsOneStatusAndThenTheConnectionIsClosed) {
    const RunningServer server{sys, backend, config("s3cret")};
    Client client{path()};
    client.send_hello("wrong");
    const auto reply = client.receive();
    ASSERT_TRUE(reply);
    EXPECT_EQ(reply->kind, Reply::Kind::Status);
    EXPECT_EQ(reply->request, 0U);
    EXPECT_EQ(reply->status, kInvalidArgument);
    EXPECT_TRUE(client.closed_by_server());
}

TEST_F(IpcServerTest, AWrongMajorVersionIsRefusedTheSameWay) {
    const RunningServer server{sys, backend, config()};
    Client client{path()};
    ipc::Hello hello;
    hello.major = ipc::kVersionMajor + 1;
    client.send_message(hello);
    const auto reply = client.receive();
    ASSERT_TRUE(reply);
    EXPECT_EQ(reply->kind, Reply::Kind::Status);
    EXPECT_EQ(reply->status, kInvalidArgument);
    EXPECT_TRUE(client.closed_by_server());
}

TEST_F(IpcServerTest, AFirstFrameThatIsNotHelloIsRefused) {
    const RunningServer server{sys, backend, config()};
    Client client{path()};
    client.send_message(ipc::Ping{});
    const auto reply = client.receive();
    ASSERT_TRUE(reply);
    EXPECT_EQ(reply->kind, Reply::Kind::Status);
    EXPECT_EQ(reply->status, kInvalidArgument);
    EXPECT_TRUE(client.closed_by_server());
}

TEST_F(IpcServerTest, AHelloSentOneByteAtATimeStillWorks) {
    const RunningServer server{sys, backend, config()};
    Client client{path()};
    for (const std::byte b : Client::encode(ipc::Hello{})) {
        client.send_raw({b});
        std::this_thread::sleep_for(2ms);
    }
    const auto reply = client.receive();
    ASSERT_TRUE(reply);
    EXPECT_EQ(reply->kind, Reply::Kind::HelloOk);
}

TEST_F(IpcServerTest, FramesBehindTheHelloInTheSameWriteAreNotLost) {
    const RunningServer server{sys, backend, config()};
    Client client{path()};
    std::vector<std::byte> both = Client::encode(ipc::Hello{});
    const std::vector<std::byte> ping = Client::encode(ipc::Ping{});
    both.insert(both.end(), ping.begin(), ping.end());
    client.send_raw(both);  // the widened decoder must start with the bytes after HELLO
    EXPECT_EQ(client.receive()->kind, Reply::Kind::HelloOk);
    EXPECT_EQ(client.receive()->kind, Reply::Kind::Pong);
}

TEST_F(IpcServerTest, PingIsAnsweredWithPongOnceConnected) {
    const RunningServer server{sys, backend, config()};
    Client client{path()};
    client.send_hello();
    ASSERT_EQ(client.receive()->kind, Reply::Kind::HelloOk);
    client.send_message(ipc::Ping{});
    EXPECT_EQ(client.receive()->kind, Reply::Kind::Pong);
}

// ---- one consumer -----------------------------------------------------------------------------

TEST_F(IpcServerTest, ASecondApplicationIsToldTheSlotIsTakenAndTheFirstIsUndisturbed) {
    const RunningServer server{sys, backend, config()};
    Client first{path()};
    first.send_hello();
    ASSERT_EQ(first.receive()->kind, Reply::Kind::HelloOk);

    Client second{path()};
    ASSERT_TRUE(second.connected());
    const auto reply = second.receive();
    ASSERT_TRUE(reply);
    EXPECT_EQ(reply->kind, Reply::Kind::Status);
    EXPECT_EQ(reply->request, 0U);
    EXPECT_EQ(reply->status, kBackpressure);
    EXPECT_TRUE(second.closed_by_server());

    first.send_message(ipc::Ping{});
    EXPECT_EQ(first.receive()->kind, Reply::Kind::Pong);
}

TEST_F(IpcServerTest, TheSlotIsFreeAgainAfterTheApplicationDisconnects) {
    const RunningServer server{sys, backend, config()};
    {
        Client first{path()};
        first.send_hello();
        ASSERT_EQ(first.receive()->kind, Reply::Kind::HelloOk);
    }  // disconnects
    // The server notices on its own thread; a new application is accepted as soon as it has.
    bool accepted = false;
    for (int attempt = 0; attempt < 100 && !accepted; ++attempt) {
        Client next{path()};
        next.send_hello();
        const auto reply = next.receive();
        accepted = reply && reply->kind == Reply::Kind::HelloOk;
        if (!accepted) {
            std::this_thread::sleep_for(20ms);
        }
    }
    EXPECT_TRUE(accepted);
}

// ---- what the operator is told (ADR-0037) -----------------------------------------------------

constexpr int kInfo = static_cast<int>(wsrv::LogLevel::Info);
constexpr int kWarn = static_cast<int>(wsrv::LogLevel::Warn);

TEST_F(IpcServerTest, AnApplicationConnectingAndLeavingIsLoggedAsInfo) {
    const RunningServer server{sys, backend, config()};
    {
        Client client{path()};
        client.send_hello();
        ASSERT_EQ(client.receive()->kind, Reply::Kind::HelloOk);
        EXPECT_TRUE(logs.wait_for("app_connected")
                        .starts_with("app_connected|" + std::to_string(kInfo) + "|"));
    }
    EXPECT_TRUE(logs.wait_for("app_disconnected")
                    .starts_with("app_disconnected|" + std::to_string(kInfo) + "|"));
}

TEST_F(IpcServerTest, AWrongTokenIsALoggedWarningThatNeverRepeatsTheToken) {
    const RunningServer server{sys, backend, config("s3cret")};
    Client client{path()};
    client.send_hello("wrong-token-value");
    ASSERT_TRUE(client.receive());
    const std::string line = logs.wait_for("app_rejected");
    EXPECT_TRUE(line.starts_with("app_rejected|" + std::to_string(kWarn) + "|")) << line;
    const std::string all = logs.everything();
    EXPECT_EQ(all.find("wrong-token-value"), std::string::npos)
        << "a secret must never reach a log";
    EXPECT_EQ(all.find("s3cret"), std::string::npos);
}

TEST_F(IpcServerTest, ASecondApplicationAndAStrangerAreLogged) {
    const RunningServer server{sys, backend, config()};
    Client first{path()};
    first.send_hello();
    ASSERT_EQ(first.receive()->kind, Reply::Kind::HelloOk);
    Client second{path()};
    ASSERT_TRUE(second.receive());
    EXPECT_NE(logs.wait_for("app_rejected").find("second application"), std::string::npos);
}

TEST_F(IpcServerTest, APeerWithAnotherUidIsLogged) {
    const RunningServer server{sys, backend, config()};
    sys.succeed(Call::PeerUid, 4242);
    const Client stranger{path()};
    ASSERT_TRUE(stranger.connected());
    EXPECT_NE(logs.wait_for("app_rejected").find("another user"), std::string::npos);
}

TEST_F(IpcServerTest, ASilentPeerIsLoggedWhenItsDeadlinePasses) {
    IpcServerConfig c = config();
    c.hello_timeout = 100ms;
    const RunningServer server{sys, backend, c};
    const Client silent{path()};
    ASSERT_TRUE(silent.connected());
    EXPECT_NE(logs.wait_for("app_rejected").find("no HELLO"), std::string::npos);
}

// ---- deadlines and bounds ---------------------------------------------------------------------

TEST_F(IpcServerTest, APeerThatNeverSaysHelloIsClosedAndDoesNotKeepTheSlot) {
    IpcServerConfig c = config();
    c.hello_timeout = 100ms;
    const RunningServer server{sys, backend, c};
    const Client silent{path()};
    ASSERT_TRUE(silent.connected());
    EXPECT_TRUE(silent.closed_by_server());  // within the client's 3 s receive timeout

    Client next{path()};
    next.send_hello();
    EXPECT_EQ(next.receive()->kind, Reply::Kind::HelloOk);
}

TEST_F(IpcServerTest, AHelloSentJustBeforeTheDeadlineStillCounts) {
    IpcServerConfig c = config();
    c.hello_timeout = 400ms;
    const RunningServer server{sys, backend, c};
    Client client{path()};
    std::this_thread::sleep_for(100ms);
    client.send_hello();
    EXPECT_EQ(client.receive()->kind, Reply::Kind::HelloOk);
    std::this_thread::sleep_for(600ms);  // the deadline is over: it must not fire any more
    client.send_message(ipc::Ping{});
    EXPECT_EQ(client.receive()->kind, Reply::Kind::Pong);
}

TEST_F(IpcServerTest, AHeaderLargerThanAnyHelloClosesTheConnectionWithoutReadingTheBody) {
    const RunningServer server{sys, backend, config()};
    const Client client{path()};
    client.send_raw(header(1U << 20U, static_cast<std::uint8_t>(ipc::FrameType::Hello)));
    EXPECT_TRUE(client.closed_by_server());  // the 1 MiB body was never awaited
}

TEST_F(IpcServerTest, AHeaderAboveTheFrameLimitAfterHelloClosesTheConnection) {
    const RunningServer server{sys, backend, config()};
    Client client{path()};
    client.send_hello();
    ASSERT_EQ(client.receive()->kind, Reply::Kind::HelloOk);
    client.send_raw(header(kMaxFrame + 1, static_cast<std::uint8_t>(ipc::FrameType::SendText)));
    EXPECT_TRUE(client.closed_by_server());
}

TEST_F(IpcServerTest, MalformedFramesCloseTheConnectionWithoutAReply) {
    const RunningServer server{sys, backend, config()};
    {
        const Client unknown_type{path()};
        unknown_type.send_raw(header(2, 200));
        EXPECT_TRUE(unknown_type.closed_by_server());
    }
    {
        const Client bad_flags{path()};
        bad_flags.send_raw(header(2, static_cast<std::uint8_t>(ipc::FrameType::Hello), 1));
        EXPECT_TRUE(bad_flags.closed_by_server());
    }
    {
        const Client short_length{path()};
        short_length.send_raw(header(1, static_cast<std::uint8_t>(ipc::FrameType::Hello)));
        EXPECT_TRUE(short_length.closed_by_server());
    }
}

TEST_F(IpcServerTest, FramesOnlyTheDaemonMaySendCloseTheConnection) {
    // HELLO_OK, EVENT, STATUS and PONG travel daemon to application. An application that sends one
    // is confused or hostile; either way the link is not worth keeping.
    ipc::Event event;
    event.conn = 1;
    const std::vector<ipc::Message> wrong_way{ipc::HelloOk{}, event, ipc::Status{}, ipc::Pong{}};
    for (const ipc::Message& frame : wrong_way) {
        const RunningServer server{sys, backend, config()};
        Client client{path()};
        client.send_hello();
        ASSERT_EQ(client.receive()->kind, Reply::Kind::HelloOk);
        client.send_message(frame);
        EXPECT_TRUE(client.closed_by_server());
    }
}

TEST_F(IpcServerTest, AHelloAfterTheHandshakeClosesTheConnection) {
    const RunningServer server{sys, backend, config()};
    Client client{path()};
    client.send_hello();
    ASSERT_EQ(client.receive()->kind, Reply::Kind::HelloOk);
    client.send_hello();
    EXPECT_TRUE(client.closed_by_server());
}

// ---- who may connect --------------------------------------------------------------------------

TEST_F(IpcServerTest, APeerWithAnotherUidIsDroppedWithoutAReply) {
    const RunningServer server{sys, backend, config()};
    sys.succeed(Call::PeerUid, 4242);  // the kernel says the next peer is someone else
    const Client stranger{path()};
    ASSERT_TRUE(stranger.connected());
    stranger.send_hello_if_still_open();
    EXPECT_TRUE(stranger.closed_by_server()) << "no HELLO_OK, no STATUS: nothing for a stranger";

    Client owner{path()};  // the script is used up: the real uid matches
    owner.send_hello();
    EXPECT_EQ(owner.receive()->kind, Reply::Kind::HelloOk);
}

TEST_F(IpcServerTest, ACredentialsFailureDropsThePeer) {
    const RunningServer server{sys, backend, config()};
    sys.fail(Call::PeerUid, EINVAL);
    const Client client{path()};
    client.send_hello_if_still_open();
    EXPECT_TRUE(client.closed_by_server());
}

// ---- events out, commands in ------------------------------------------------------------------

using wsrv::HostEventType;
using wsrv::testing::RecordedCommand;

constexpr auto kOk = static_cast<std::uint8_t>(WSRV_OK);
constexpr auto kNoSuchConn = static_cast<std::uint8_t>(WSRV_ERR_NO_SUCH_CONN);

ipc::SendData text_command(std::uint32_t request, std::uint64_t conn, const std::string& text) {
    ipc::SendData command;
    command.request = request;
    command.conn = conn;
    command.text = true;
    command.data =
        ByteView{reinterpret_cast<const std::byte*>(text.data()), text.size()};  // NOLINT
    return command;
}

/// Connect and complete HELLO.
std::unique_ptr<Client> connect_ready(const std::string& path) {
    auto client = std::make_unique<Client>(path);
    client->send_hello();
    const auto reply = client->receive();
    EXPECT_TRUE(reply && reply->kind == Reply::Kind::HelloOk);
    return client;
}

TEST_F(IpcServerTest, EventsQueuedBeforeTheApplicationConnectedArriveInOrderAfterHelloOk) {
    // No application yet: the daemon holds the events (the queue pauses the WebSocket clients).
    ASSERT_TRUE(backend.push(HostEventType::Open, 1, "10.0.0.1:5000").has_value());
    ASSERT_TRUE(backend.push(HostEventType::Text, 1, "hello").has_value());
    const RunningServer server{sys, backend, config()};
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(backend.queue().pending(), 2U) << "nothing may be taken while nobody listens";

    Client client{path()};
    client.send_hello();
    EXPECT_EQ(client.receive()->kind, Reply::Kind::HelloOk) << "HELLO_OK comes before any event";
    const auto open = client.receive();
    const auto text = client.receive();
    ASSERT_TRUE(open && text);
    EXPECT_EQ(open->kind, Reply::Kind::Event);
    EXPECT_EQ(open->event, ipc::EventKind::Open);
    EXPECT_EQ(open->conn, 1U);
    EXPECT_EQ(open->data, "10.0.0.1:5000");
    EXPECT_EQ(text->event, ipc::EventKind::Text);
    EXPECT_EQ(text->data, "hello");
}

TEST_F(IpcServerTest, EventsThatArriveWhileTheApplicationIsConnectedAreDelivered) {
    const RunningServer server{sys, backend, config()};
    const auto client = connect_ready(path());
    ASSERT_TRUE(backend.push(HostEventType::Open, 7, "a").has_value());
    ASSERT_TRUE(backend.push(HostEventType::Binary, 7, "bin").has_value());
    ASSERT_TRUE(backend.push(HostEventType::Close, 7, "bye", 1005, false).has_value());
    EXPECT_EQ(client->receive()->event, ipc::EventKind::Open);
    EXPECT_EQ(client->receive()->event, ipc::EventKind::Binary);
    const auto close = client->receive();
    ASSERT_TRUE(close);
    EXPECT_EQ(close->event, ipc::EventKind::Close);
    EXPECT_EQ(close->close_code, 1005U);
    EXPECT_FALSE(close->clean);
    EXPECT_EQ(close->data, "bye");
}

TEST_F(IpcServerTest, ACommandIsForwardedToTheBackendAndAnsweredWithItsRequestId) {
    const RunningServer server{sys, backend, config()};
    const auto client = connect_ready(path());
    ASSERT_TRUE(backend.push(HostEventType::Open, 3, "a").has_value());
    ASSERT_EQ(client->receive()->event, ipc::EventKind::Open);

    const std::string payload = "to the browser";
    client->send_message(text_command(41, 3, payload));
    const auto answer = client->receive();
    ASSERT_TRUE(answer);
    EXPECT_EQ(answer->kind, Reply::Kind::Status);
    EXPECT_EQ(answer->request, 41U);
    EXPECT_EQ(answer->status, kOk);
    const auto commands = backend.commands();
    ASSERT_EQ(commands.size(), 1U);
    EXPECT_EQ(commands[0].conn, 3U);
    EXPECT_EQ(commands[0].data, payload);
}

TEST_F(IpcServerTest, BrokerCommandsAreForwardedAndAnsweredLikeAnyOther) {  // IPC 0.3, ADR-0028
    const RunningServer server{sys, backend, config()};
    const auto client = connect_ready(path());
    ASSERT_TRUE(backend.push(HostEventType::Open, 3, "a").has_value());
    ASSERT_EQ(client->receive()->event, ipc::EventKind::Open);

    const std::string topic = "room.1";
    const std::string payload = "hello room";
    ipc::TopicCommand subscribe;
    subscribe.request = 51;
    subscribe.conn = 3;
    subscribe.topic = wsrv::as_bytes(std::string_view{topic});
    client->send_message(subscribe);
    ipc::Publish publish;
    publish.request = 52;
    publish.topic = wsrv::as_bytes(std::string_view{topic});
    publish.payload = wsrv::as_bytes(std::string_view{payload});
    client->send_message(publish);

    const auto first = client->receive();
    const auto second = client->receive();
    ASSERT_TRUE(first && second);
    EXPECT_EQ(first->request, 51U);
    EXPECT_EQ(first->status, kOk);
    EXPECT_EQ(second->request, 52U);
    EXPECT_EQ(second->status, kOk);
    const auto subscribes = backend.commands_of(RecordedCommand::Kind::Subscribe);
    ASSERT_EQ(subscribes.size(), 1U);
    EXPECT_EQ(subscribes[0].conn, 3U);
    EXPECT_EQ(subscribes[0].topic, topic);
    const auto publishes = backend.commands_of(RecordedCommand::Kind::PublishText);
    ASSERT_EQ(publishes.size(), 1U);
    EXPECT_EQ(publishes[0].data, payload);
}

TEST_F(IpcServerTest, AnApplicationThatSaidMinor2CannotKnowBrokerFramesAndIsCutOff) {
    const RunningServer server{sys, backend, config()};
    Client client{path()};
    ipc::Hello hello;
    hello.minor = 2;  // before the broker existed
    client.send_message(hello);
    ASSERT_EQ(client.receive()->kind, Reply::Kind::HelloOk);
    ipc::Publish publish;
    publish.request = 1;
    client.send_message(publish);
    EXPECT_TRUE(client.closed_by_server());
    EXPECT_TRUE(backend.commands().empty());
}

TEST_F(IpcServerTest, BadCommandsAreAnsweredAndTheLinkStaysOpen) {
    const RunningServer server{sys, backend, config()};
    const auto client = connect_ready(path());
    ASSERT_TRUE(backend.push(HostEventType::Open, 3, "a").has_value());
    ASSERT_EQ(client->receive()->event, ipc::EventKind::Open);

    client->send_message(text_command(5, 999, "x"));  // a connection nobody told the app about
    const auto unknown = client->receive();
    ASSERT_TRUE(unknown);
    EXPECT_EQ(unknown->request, 5U);
    EXPECT_EQ(unknown->status, kNoSuchConn);

    client->send_message(text_command(0, 3, "x"));  // request id 0 is reserved for the handshake
    const auto reserved = client->receive();
    ASSERT_TRUE(reserved);
    EXPECT_EQ(reserved->status, kInvalidArgument);

    EXPECT_TRUE(backend.commands().empty()) << "neither command may reach the backend";
    client->send_message(ipc::Ping{});
    EXPECT_EQ(client->receive()->kind, Reply::Kind::Pong);
}

TEST_F(IpcServerTest, CommandsSplitIntoSingleBytesAreStillAnswered) {
    const RunningServer server{sys, backend, config()};
    const auto client = connect_ready(path());
    ASSERT_TRUE(backend.push(HostEventType::Open, 3, "a").has_value());
    ASSERT_EQ(client->receive()->event, ipc::EventKind::Open);
    for (const std::byte b : Client::encode(text_command(8, 3, "slow"))) {
        client->send_raw({b});
        std::this_thread::sleep_for(1ms);
    }
    const auto answer = client->receive();
    ASSERT_TRUE(answer);
    EXPECT_EQ(answer->request, 8U);
    EXPECT_EQ(answer->status, kOk);
}

// ---- backpressure ----------------------------------------------------------------------------

/// The payload of test event `index`: its number, padded to `size`.
std::string numbered(std::size_t index, std::size_t size) {
    std::string text = std::to_string(index) + ":";
    text.resize(size, '.');
    return text;
}

constexpr std::size_t kBurst = 1500;
constexpr std::size_t kBurstPayload = 4000;  // 6 MB in all: inside the queue's 8 MB, far over the
                                             // socket buffers

void push_burst(wsrv::testing::FakeBackend& backend) {
    ASSERT_TRUE(backend.push(HostEventType::Open, 1, "a").has_value());
    for (std::size_t i = 0; i < kBurst; ++i) {
        ASSERT_TRUE(backend.push(HostEventType::Text, 1, numbered(i, kBurstPayload)).has_value());
    }
}

TEST_F(IpcServerTest, ASlowApplicationHoldsEventsInTheQueueInsteadOfLosingThem) {
    const RunningServer server{sys, backend, config()};
    const auto client = connect_ready(path());
    push_burst(backend);  // the application is connected but reads nothing yet

    std::this_thread::sleep_for(300ms);
    const std::size_t held = backend.queue().pending();
    EXPECT_GT(held, kBurst / 2) << "the daemon must stop taking events when its output is full";
    const std::size_t polls = backend.polls();
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(backend.polls(), polls) << "a full daemon must not spin on the backend";

    // Now the application reads. EVERY event arrives, in order: nothing was dropped, and the
    // daemon resumed by itself after its output drained (the wake-up edge was long spent).
    ASSERT_EQ(client->receive()->event, ipc::EventKind::Open);
    for (std::size_t i = 0; i < kBurst; ++i) {
        const auto event = client->receive();
        ASSERT_TRUE(event) << "event " << i << " never came";
        ASSERT_EQ(event->data, numbered(i, kBurstPayload)) << "event " << i;
    }
    EXPECT_EQ(backend.queue().pending(), 0U);
    EXPECT_EQ(backend.queue().dropped(), 0U);
}

TEST_F(IpcServerTest, AFullOutputPausesCommandReadingAndResumesWithoutLosingAnswers) {
    const RunningServer server{sys, backend, config()};
    const auto client = connect_ready(path());
    push_burst(backend);
    std::this_thread::sleep_for(300ms);  // output is full now

    // Commands sent while the daemon cannot answer wait in the kernel, not in daemon memory.
    for (std::uint32_t request = 1; request <= 50; ++request) {
        client->send_message(text_command(request, 1, "x"));
    }
    std::size_t statuses = 0;
    std::size_t events = 0;
    while (statuses < 50 || events < kBurst + 1) {
        const auto reply = client->receive();
        ASSERT_TRUE(reply) << "link died with " << statuses << " statuses, " << events << " events";
        if (reply->kind == Reply::Kind::Status) {
            ++statuses;
            EXPECT_EQ(reply->status, kOk);
        } else if (reply->kind == Reply::Kind::Event) {
            ++events;
        }
    }
    EXPECT_EQ(backend.commands().size(), 50U);
}

TEST_F(IpcServerTest, AnApplicationThatStopsReadingForTooLongIsDisconnected) {
    IpcServerConfig c = config();
    c.stall_timeout = 200ms;
    const RunningServer server{sys, backend, c};
    const auto client = connect_ready(path());
    push_burst(backend);

    std::this_thread::sleep_for(1500ms);  // far longer than the stall timeout (TSan margin)
    std::size_t received = 0;
    while (client->receive()) {
        ++received;
    }
    EXPECT_LT(received, kBurst) << "the link must have been cut before everything was delivered";
    EXPECT_TRUE(client->closed_by_server());
}

TEST_F(IpcServerTest, AnApplicationThatKeepsReadingIsNeverDisconnectedHoweverLongItTakes) {
    // The burst is queued BEFORE the application connects, so the daemon fills the socket the
    // moment HELLO_OK is out and the stall clock starts then: the test thread cannot be late
    // starting to read (pushing 6 MB while the daemon already waits was slow enough, under a
    // sanitizer on a busy machine, to outlast the timeout before the first read).
    push_burst(backend);
    IpcServerConfig c = config();
    c.stall_timeout = 1000ms;
    const RunningServer server{sys, backend, c};
    const auto client = connect_ready(path());
    // Read in steps with short pauses: the whole transfer takes longer than the stall timeout, but
    // no single pause comes near it, so progress must restart the clock every time.
    constexpr std::size_t kStep = 150;
    for (std::size_t i = 0; i < kBurst + 1; ++i) {
        ASSERT_TRUE(client->receive()) << "cut off at event " << i;
        if (i % kStep == kStep - 1) {
            std::this_thread::sleep_for(100ms);
        }
    }
    client->send_message(ipc::Ping{});
    EXPECT_EQ(client->receive()->kind, Reply::Kind::Pong);
}

TEST_F(IpcServerTest, AnIdleApplicationIsNotAStall) {
    IpcServerConfig c = config();
    c.stall_timeout = 100ms;
    const RunningServer server{sys, backend, c};
    const auto client = connect_ready(path());
    std::this_thread::sleep_for(600ms);  // nothing is waiting to be sent: there is nothing to stall
    client->send_message(ipc::Ping{});
    EXPECT_EQ(client->receive()->kind, Reply::Kind::Pong);
}

// ---- the application goes away ----------------------------------------------------------------

/// Wait (bounded) until `done()` holds; the daemon works on its own thread.
template <typename Predicate>
bool eventually(Predicate done, std::chrono::milliseconds limit = 5000ms) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

/// Connect a new application, retrying until the daemon has noticed that the previous one left.
std::unique_ptr<Client> connect_when_free(const std::string& path) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        auto client = std::make_unique<Client>(path);
        client->send_hello();
        const auto reply = client->receive();
        if (reply && reply->kind == Reply::Kind::HelloOk) {
            return client;
        }
        std::this_thread::sleep_for(20ms);
    }
    return nullptr;
}

TEST_F(IpcServerTest, ConnectionsOfADisconnectedApplicationAreClosedWith1012AfterTheGrace) {
    IpcServerConfig c = config();
    c.restart_grace = 600ms;
    const RunningServer server{sys, backend, c};
    {
        const auto app = connect_ready(path());
        ASSERT_TRUE(backend.push(HostEventType::Open, 5, "a").has_value());
        ASSERT_EQ(app->receive()->event, ipc::EventKind::Open);
    }  // the application disconnects, knowing connection 5
    std::this_thread::sleep_for(200ms);
    EXPECT_TRUE(backend.commands_of(RecordedCommand::Kind::Close).empty())
        << "the grace period has not passed yet";
    ASSERT_TRUE(
        eventually([&] { return !backend.commands_of(RecordedCommand::Kind::Close).empty(); }));
    const auto commands = backend.commands_of(RecordedCommand::Kind::Close);
    ASSERT_EQ(commands.size(), 1U);
    EXPECT_EQ(commands[0].kind, RecordedCommand::Kind::Close);
    EXPECT_EQ(commands[0].conn, 5U);
    EXPECT_EQ(commands[0].code, 1012U);
}

// ADR-0034: a close that a full loop queue refuses leaves the connection open. It used to be given
// up on; now it is asked again every few milliseconds until a loop takes it, and then no more.
TEST_F(IpcServerTest, A1012CloseTheLoopRefusesIsRetriedUntilItIsAccepted) {
    IpcServerConfig c = config();
    c.restart_grace = 100ms;
    const RunningServer server{sys, backend, c};
    backend.fail_commands_with(ErrorCode::BufferFull);
    {
        const auto app = connect_ready(path());
        ASSERT_TRUE(backend.push(HostEventType::Open, 5, "a").has_value());
        ASSERT_EQ(app->receive()->event, ipc::EventKind::Open);
    }
    ASSERT_TRUE(eventually(
        [&] { return backend.commands_of(RecordedCommand::Kind::Close).size() >= 3; }, 10000ms))
        << "while the loop keeps refusing, the daemon keeps asking";
    backend.fail_commands_with(std::nullopt);  // the loop has room again
    std::this_thread::sleep_for(300ms);
    const std::size_t settled = backend.commands_of(RecordedCommand::Kind::Close).size();
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(backend.commands_of(RecordedCommand::Kind::Close).size(), settled)
        << "once a loop has taken the close, it is not asked again";
    for (const auto& command : backend.commands_of(RecordedCommand::Kind::Close)) {
        EXPECT_EQ(command.conn, 5U);
        EXPECT_EQ(command.code, 1012U);
    }
}

TEST_F(IpcServerTest, WhenTheApplicationDisconnectsItsConnectionsLoseTheirSubscriptionsAtOnce) {
    IpcServerConfig c = config();
    c.restart_grace = 5000ms;  // the 1012 close is far away; the drop must not wait for it
    const RunningServer server{sys, backend, c};
    {
        const auto app = connect_ready(path());
        ASSERT_TRUE(backend.push(HostEventType::Open, 7, "a").has_value());
        ASSERT_EQ(app->receive()->event, ipc::EventKind::Open);
    }
    ASSERT_TRUE(eventually(
        [&] { return !backend.commands_of(RecordedCommand::Kind::DropSubscriptions).empty(); }));
    EXPECT_EQ(backend.commands_of(RecordedCommand::Kind::DropSubscriptions)[0].conn, 7U);
    EXPECT_TRUE(backend.commands_of(RecordedCommand::Kind::Close).empty());
}

TEST_F(IpcServerTest, ANewApplicationNeverSeesTheTrafficOfConnectionsItWasNotToldAbout) {
    IpcServerConfig c = config();
    c.restart_grace = 60s;  // out of the way
    const RunningServer server{sys, backend, c};
    {
        const auto first = connect_ready(path());
        ASSERT_TRUE(backend.push(HostEventType::Open, 5, "a").has_value());
        ASSERT_EQ(first->receive()->event, ipc::EventKind::Open);
    }
    const auto second = connect_when_free(path());
    ASSERT_TRUE(second);
    ASSERT_TRUE(backend.push(HostEventType::Text, 5, "for the old application").has_value());
    ASSERT_TRUE(backend.push(HostEventType::Open, 6, "b").has_value());
    const auto event = second->receive();
    ASSERT_TRUE(event);
    EXPECT_EQ(event->event, ipc::EventKind::Open);
    EXPECT_EQ(event->conn, 6U) << "connection 5's Text was for the previous application";

    second->send_message(text_command(1, 5, "x"));  // an id from the previous life
    EXPECT_EQ(second->receive()->status, kNoSuchConn);
}

TEST_F(IpcServerTest, AnApplicationThatKnewNoConnectionsLeavesNothingToClose) {
    IpcServerConfig c = config();
    c.restart_grace = 100ms;
    const RunningServer server{sys, backend, c};
    { const auto app = connect_ready(path()); }
    std::this_thread::sleep_for(600ms);
    EXPECT_TRUE(backend.commands().empty());
}

TEST_F(IpcServerTest, ACutOffApplicationsConnectionsAreClosedWith1001WithoutWaitingForTheGrace) {
    IpcServerConfig c = config();
    c.stall_timeout = 200ms;
    c.restart_grace = 60s;  // the 1001 must not depend on it
    const RunningServer server{sys, backend, c};
    const auto app = connect_ready(path());
    ASSERT_TRUE(backend.push(HostEventType::Open, 1, "a").has_value());
    ASSERT_EQ(app->receive()->event, ipc::EventKind::Open);
    for (std::size_t i = 0; i < kBurst; ++i) {  // the application now stops reading
        ASSERT_TRUE(backend.push(HostEventType::Text, 1, numbered(i, kBurstPayload)).has_value());
    }
    ASSERT_TRUE(eventually(
        [&] { return !backend.commands_of(RecordedCommand::Kind::Close).empty(); }, 10000ms));
    const auto commands = backend.commands_of(RecordedCommand::Kind::Close);
    EXPECT_EQ(commands[0].kind, RecordedCommand::Kind::Close);
    EXPECT_EQ(commands[0].conn, 1U);
    EXPECT_EQ(commands[0].code, 1001U);
}

// ---- end to end: a real browser-side client, the real WebSocket server, an IPC application ----

/// The whole chain: TcpClient (a WebSocket client) <-> EmbeddedServer <-> EmbeddedBackend <->
/// IpcServer <-> Client (an IPC application). Nothing is faked except the syscalls of the IPC side.
struct EndToEnd : public ::testing::Test {
    /// Created and started before `backend`, which keeps a reference to it (member order).
    static std::unique_ptr<wsrv::EmbeddedServer> start_web_server() {
        wsrv::EmbeddedConfig web;
        EXPECT_TRUE(web.set("allow_origin", "https://app.example.com").has_value());
        EXPECT_TRUE(web.set("shutdown_grace_ms", "1000").has_value());
        auto created = wsrv::EmbeddedServer::create(web);
        EXPECT_TRUE(created.has_value());
        EXPECT_TRUE((*created)->start().has_value());
        return std::move(*created);
    }

    [[nodiscard]] IpcServerConfig config() const {
        IpcServerConfig c;
        c.socket_path = dir.file("ipc.sock");
        c.max_frame = 1U << 20U;  // 1 MiB default message size + room
        c.restart_grace = 400ms;
        return c;
    }
    [[nodiscard]] std::string path() const {
        return dir.file("ipc.sock");
    }

    FakeSyscalls sys;
    TempDir dir;
    std::unique_ptr<wsrv::EmbeddedServer> embedded = start_web_server();
    wsrv::wsrvd::EmbeddedBackend backend{*embedded};
};

TEST_F(EndToEnd, AMessageTravelsFromTheBrowserToTheApplicationAndBack) {
    const RunningServer server{sys, backend, config()};
    ASSERT_FALSE(server.failure());
    const auto app = connect_ready(path());

    wsrv::test::TcpClient browser{embedded->port()};
    ASSERT_TRUE(browser.upgrade());
    const auto open = app->receive();
    ASSERT_TRUE(open);
    ASSERT_EQ(open->event, ipc::EventKind::Open);
    const std::uint64_t conn = open->conn;

    ASSERT_TRUE(browser.send_frame(wsrv::Opcode::Text, "hello from the browser"));
    const auto text = app->receive();
    ASSERT_TRUE(text);
    EXPECT_EQ(text->event, ipc::EventKind::Text);
    EXPECT_EQ(text->conn, conn);
    EXPECT_EQ(text->data, "hello from the browser");

    app->send_message(text_command(1, conn, "hello from the application"));
    const auto status = app->receive();
    ASSERT_TRUE(status);
    EXPECT_EQ(status->kind, Reply::Kind::Status);
    EXPECT_EQ(status->status, kOk);
    const auto frame = browser.read_frame();
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->payload, "hello from the application");
}

TEST_F(EndToEnd, TheApplicationCanCloseAConnectionAndSeesItsCloseEvent) {
    const RunningServer server{sys, backend, config()};
    const auto app = connect_ready(path());
    wsrv::test::TcpClient browser{embedded->port()};
    ASSERT_TRUE(browser.upgrade());
    const auto open = app->receive();
    ASSERT_TRUE(open);

    ipc::CloseCommand close;
    close.request = 2;
    close.conn = open->conn;
    close.code = 4001;
    app->send_message(close);
    EXPECT_EQ(app->receive()->status, kOk);

    const auto frame = browser.read_frame();
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->opcode, wsrv::Opcode::Close);
    ASSERT_GE(frame->payload.size(), 2U);
    EXPECT_EQ((static_cast<std::uint8_t>(frame->payload[0]) << 8U) |
                  static_cast<std::uint8_t>(frame->payload[1]),
              4001);
    ASSERT_TRUE(browser.send_frame(wsrv::Opcode::Close, wsrv::test::close_payload(4001)));

    const auto closed = app->receive();
    ASSERT_TRUE(closed);
    EXPECT_EQ(closed->event, ipc::EventKind::Close);
    EXPECT_EQ(closed->conn, open->conn);
    EXPECT_EQ(closed->close_code, 4001U);
}

TEST_F(EndToEnd, WhenTheApplicationDisappearsItsBrowsersAreToldToComeBackLater) {
    const RunningServer server{sys, backend, config()};
    wsrv::test::TcpClient browser{embedded->port()};
    {
        const auto app = connect_ready(path());
        ASSERT_TRUE(browser.upgrade());
        ASSERT_EQ(app->receive()->event, ipc::EventKind::Open);
    }  // the application dies
    const auto frame = browser.read_frame();  // after restart_grace: 1012 Service Restart
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->opcode, wsrv::Opcode::Close);
    ASSERT_GE(frame->payload.size(), 2U);
    EXPECT_EQ((static_cast<std::uint8_t>(frame->payload[0]) << 8U) |
                  static_cast<std::uint8_t>(frame->payload[1]),
              1012);
}

TEST_F(EndToEnd, ABrowserThatConnectsBeforeAnyApplicationIsHeldNotLost) {
    const RunningServer server{sys, backend, config()};
    wsrv::test::TcpClient browser{embedded->port()};
    ASSERT_TRUE(browser.upgrade());  // the WebSocket handshake itself completes
    ASSERT_TRUE(browser.send_frame(wsrv::Opcode::Text, "early"));
    std::this_thread::sleep_for(200ms);

    const auto app = connect_ready(path());  // the application arrives late
    const auto open = app->receive();
    const auto text = app->receive();
    ASSERT_TRUE(open && text);
    EXPECT_EQ(open->event, ipc::EventKind::Open);
    EXPECT_EQ(text->data, "early");
}

// ---- lifecycle --------------------------------------------------------------------------------

// ---- the TCP transport (ADR-0038) --------------------------------------------------------------

/// A TCP server on a port the kernel chose, with a token of the length the daemon insists on.
struct TcpIpcTest : public IpcServerTest {
    static inline const std::string kToken = std::string(32, 't');

    [[nodiscard]] IpcServerConfig tcp_config(std::string_view host = "127.0.0.1") const {
        IpcServerConfig c = config(kToken);
        c.socket_path.clear();
        c.tcp = wsrv::parse_endpoint(host, 0).value();
        return c;
    }
    /// A connected client for the server's bound endpoint.
    static std::unique_ptr<Client> connect(const RunningServer& server) {
        const std::optional<wsrv::Endpoint> bound = server.endpoint();
        EXPECT_TRUE(bound.has_value());
        const std::string host = bound ? wsrv::format_endpoint(*bound) : "127.0.0.1:0";
        return std::make_unique<Client>(Client::Tcp{},
                                        bound && bound->v6 ? "::1" : "127.0.0.1",
                                        bound ? bound->port : std::uint16_t{0});
    }
};

TEST_F(TcpIpcTest, TheKernelChosenPortIsReportedSoTheTestsAndTheLogCanUseIt) {
    const RunningServer server{sys, backend, tcp_config()};
    ASSERT_FALSE(server.failure());
    ASSERT_TRUE(server.endpoint().has_value());
    EXPECT_NE(server.endpoint()->port, 0);
}

TEST_F(TcpIpcTest, HelloWithTheTokenIsAnsweredWithHelloOk) {
    const RunningServer server{sys, backend, tcp_config()};
    ASSERT_FALSE(server.failure());
    const auto client = connect(server);
    ASSERT_TRUE(client->connected());
    client->send_hello(kToken);
    const auto reply = client->receive();
    ASSERT_TRUE(reply);
    EXPECT_EQ(reply->kind, Reply::Kind::HelloOk);
    EXPECT_EQ(reply->max_frame, kMaxFrame);
    client->send_message(ipc::Ping{});
    EXPECT_EQ(client->receive()->kind, Reply::Kind::Pong);
}

TEST_F(TcpIpcTest, AWrongTokenGetsTheSameSingleStatusAndNothingAboutTheTokenIsLogged) {
    const RunningServer server{sys, backend, tcp_config()};
    const auto client = connect(server);
    client->send_hello(std::string(32, 'x'));
    const auto reply = client->receive();
    ASSERT_TRUE(reply);
    EXPECT_EQ(reply->kind, Reply::Kind::Status);
    EXPECT_EQ(reply->request, 0U);
    EXPECT_EQ(reply->status, kInvalidArgument);
    EXPECT_TRUE(client->closed_by_server());
    const std::string everything = logs.everything();
    EXPECT_EQ(everything.find(kToken), std::string::npos);
    EXPECT_EQ(everything.find(std::string(32, 'x')), std::string::npos);
}

TEST_F(TcpIpcTest, NoPeerUidIsAskedForOverTcpTheTokenIsTheAuthentication) {
    const RunningServer server{sys, backend, tcp_config()};
    sys.succeed(Call::PeerUid, 4242);  // would drop a Unix peer; there is no uid to read on TCP
    const auto client = connect(server);
    client->send_hello(kToken);
    EXPECT_EQ(client->receive()->kind, Reply::Kind::HelloOk);
    EXPECT_EQ(sys.pending_script(Call::PeerUid), 1U) << "peer_uid must not even be called";
}

TEST_F(TcpIpcTest, AnAcceptedSocketGetsNoDelayAndKeepaliveSoAVanishedHostFreesTheSlot) {
    const RunningServer server{sys, backend, tcp_config()};
    const auto client = connect(server);
    client->send_hello(kToken);
    ASSERT_EQ(client->receive()->kind, Reply::Kind::HelloOk);
    using Option = FakeSyscalls::SockOptCall;
    const std::vector<Option> calls = sys.sockopt_calls();
    const auto has = [&calls](Option wanted) {
        return std::ranges::find(calls, wanted) != calls.end();
    };
    EXPECT_TRUE(has({IPPROTO_TCP, TCP_NODELAY, 1}));
    EXPECT_TRUE(has({SOL_SOCKET, SO_KEEPALIVE, 1}));
    EXPECT_TRUE(has({IPPROTO_TCP, TCP_KEEPIDLE, 30}));
    EXPECT_TRUE(has({IPPROTO_TCP, TCP_KEEPINTVL, 10}));
    EXPECT_TRUE(has({IPPROTO_TCP, TCP_KEEPCNT, 3}));
}

TEST_F(TcpIpcTest, ASecondAuthenticatedApplicationIsToldTheSlotIsTakenAndTheFirstIsUndisturbed) {
    const RunningServer server{sys, backend, tcp_config()};
    const auto first = connect(server);
    first->send_hello(kToken);
    ASSERT_EQ(first->receive()->kind, Reply::Kind::HelloOk);

    const auto second = connect(server);
    second->send_hello(kToken);
    const auto reply = second->receive();
    ASSERT_TRUE(reply);
    EXPECT_EQ(reply->status, kBackpressure);
    EXPECT_TRUE(second->closed_by_server());

    first->send_message(ipc::Ping{});  // an authenticated application is never displaced
    EXPECT_EQ(first->receive()->kind, Reply::Kind::Pong);
}

// The rule that keeps a stranger who can reach the port from locking the application out: a
// connection that has not said HELLO holds nothing, so a newcomer replaces it.
TEST_F(TcpIpcTest, AConnectionThatHasNotSaidHelloDoesNotHoldTheSlotAgainstANewcomer) {
    const RunningServer server{sys, backend, tcp_config()};
    const auto idle = connect(server);  // says nothing, for ipc_hello_timeout_ms (5 s here)
    ASSERT_TRUE(idle->connected());

    const auto application = connect(server);
    application->send_hello(kToken);
    const auto reply = application->receive();
    ASSERT_TRUE(reply);
    EXPECT_EQ(reply->kind, Reply::Kind::HelloOk) << "the idle connection kept the slot";
    EXPECT_TRUE(idle->closed_by_server());
}

TEST_F(TcpIpcTest, AHelloThatAlreadyArrivedIsProcessedBeforeAnyDisplacement) {
    const RunningServer server{sys, backend, tcp_config()};
    const auto first = connect(server);
    first->send_hello(kToken);  // in the kernel's buffer before the second connects
    const auto second = connect(server);
    second->send_hello(kToken);

    ASSERT_EQ(first->receive()->kind, Reply::Kind::HelloOk) << "a real HELLO was thrown away";
    const auto refused = second->receive();
    ASSERT_TRUE(refused);
    EXPECT_EQ(refused->status, kBackpressure);
}

TEST_F(TcpIpcTest, ManySilentConnectionsCostOneAtATimeAndTheApplicationStillGetsIn) {
    const RunningServer server{sys, backend, tcp_config()};
    std::vector<std::unique_ptr<Client>> strangers;
    for (int i = 0; i < 64; ++i) {
        strangers.push_back(connect(server));
        ASSERT_TRUE(strangers.back()->connected());
    }
    const auto application = connect(server);
    application->send_hello(kToken);
    EXPECT_EQ(application->receive()->kind, Reply::Kind::HelloOk);
}

TEST_F(IpcServerTest,
       OnAUnixSocketAConnectionThatHasNotSaidHelloStillHoldsTheSlot) {  // 1.0 behaviour
    const RunningServer server{sys, backend, config()};
    Client idle{path()};
    ASSERT_TRUE(idle.connected());
    Client second{path()};
    second.send_hello();
    const auto reply = second.receive();
    ASSERT_TRUE(reply);
    EXPECT_EQ(reply->status, kBackpressure);
}

TEST_F(TcpIpcTest, ABindThatIsNotLoopbackIsLoggedAsAPlaintextWarning) {
    const RunningServer wide{sys, backend, tcp_config("0.0.0.0")};
    ASSERT_FALSE(wide.failure());
    const std::string line = logs.wait_for("ipc_plaintext");
    EXPECT_FALSE(line.empty());
    EXPECT_NE(line.find("|3|"), std::string::npos) << "it is a warning: " << line;
}

TEST_F(TcpIpcTest, ALoopbackBindSaysNothingAboutPlaintext) {
    const RunningServer narrow{sys, backend, tcp_config("127.0.0.1")};
    ASSERT_FALSE(narrow.failure());
    EXPECT_EQ(logs.everything().find("ipc_plaintext"), std::string::npos);
}

TEST_F(TcpIpcTest, ExactlyOneTransportMustBeConfigured) {
    IpcServerConfig both = tcp_config();
    both.socket_path = path();
    const RunningServer with_both{sys, backend, both};
    ASSERT_TRUE(with_both.failure());
    EXPECT_EQ(*with_both.failure(), ErrorCode::InvalidConfig);

    IpcServerConfig neither = tcp_config();
    neither.tcp.reset();
    const RunningServer with_neither{sys, backend, neither};
    ASSERT_TRUE(with_neither.failure());
    EXPECT_EQ(*with_neither.failure(), ErrorCode::InvalidConfig);
}

TEST_F(TcpIpcTest,
       ATcpServerRefusesToRunWithoutAToken) {  // defence in depth: finish() already does
    IpcServerConfig c = tcp_config();
    c.token.clear();
    const RunningServer server{sys, backend, c};
    ASSERT_TRUE(server.failure());
    EXPECT_EQ(*server.failure(), ErrorCode::InvalidConfig);
}

TEST_F(TcpIpcTest, ThePortIsReleasedWhenTheServerStops) {
    std::uint16_t port = 0;
    {
        const RunningServer server{sys, backend, tcp_config()};
        port = server.endpoint().value().port;
    }
    IpcServerConfig again = tcp_config();
    again.tcp->port = port;  // the same port, straight away: the listener was closed
    const RunningServer second{sys, backend, again};
    EXPECT_FALSE(second.failure());
}

TEST_F(IpcServerTest, TheSocketFileIsRemovedWhenTheServerStops) {
    {
        const RunningServer server{sys, backend, config()};
        ASSERT_TRUE(fs::exists(path()));
    }
    EXPECT_FALSE(fs::exists(path()));
}

TEST_F(IpcServerTest, ASecondDaemonOnTheSameSocketIsRefusedAndTheFirstKeepsWorking) {
    const RunningServer first{sys, backend, config()};
    ASSERT_FALSE(first.failure());
    FakeSyscalls other_sys;
    const RunningServer second{other_sys, backend, config()};
    ASSERT_TRUE(second.failure());
    EXPECT_EQ(*second.failure(), ErrorCode::AddressInUse);
    Client client{path()};
    client.send_hello();
    EXPECT_EQ(client.receive()->kind, Reply::Kind::HelloOk);
}

TEST_F(IpcServerTest, StoppingWithAConnectedApplicationClosesItsConnection) {
    auto server = std::make_unique<RunningServer>(sys, backend, config());
    Client client{path()};
    client.send_hello();
    ASSERT_EQ(client.receive()->kind, Reply::Kind::HelloOk);
    server.reset();
    EXPECT_TRUE(client.closed_by_server());
}

// ---- the handshake hook (ADR-0018) ------------------------------------------------------------

bool wait_until(const std::function<bool()>& condition) {
    for (int i = 0; i < 300; ++i) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

struct HookIpcTest : public IpcServerTest {
    [[nodiscard]] IpcServerConfig hook_config() const {
        IpcServerConfig c = config();
        c.handshake_hook = true;
        return c;
    }
};

TEST_F(HookIpcTest, TheIntakeStartsClosedOpensWithTheApplicationAndClosesWhenItLeaves) {
    const RunningServer server{sys, backend, hook_config()};
    EXPECT_EQ(backend.intake_calls(), (std::vector<bool>{false})) << "nobody is there to answer";
    {
        const auto client = connect_ready(path());
        EXPECT_TRUE(wait_until([&] { return backend.intake_calls().size() == 2; }));
        EXPECT_EQ(backend.intake_calls(), (std::vector<bool>{false, true}));
    }
    EXPECT_TRUE(wait_until([&] { return backend.intake_calls().size() == 3; }));
    EXPECT_EQ(backend.intake_calls(), (std::vector<bool>{false, true, false}));
}

TEST_F(HookIpcTest, AnApplicationThatCannotReadQuestionsIsRefusedAndTheIntakeStaysClosed) {
    const RunningServer server{sys, backend, hook_config()};
    Client old{path()};
    old.send_message(ipc::Hello{.major = ipc::kVersionMajor, .minor = 1, .token = {}});
    const auto reply = old.receive();
    ASSERT_TRUE(reply);
    EXPECT_EQ(reply->kind, Reply::Kind::Status);
    EXPECT_EQ(reply->request, 0U);
    EXPECT_EQ(reply->status, kInvalidArgument);
    EXPECT_TRUE(old.closed_by_server());
    EXPECT_EQ(backend.intake_calls(), (std::vector<bool>{false}));
}

TEST_F(HookIpcTest, WithoutTheHookTheIntakeIsNeverTouched) {
    const RunningServer server{sys, backend, config()};
    const auto client = connect_ready(path());
    EXPECT_TRUE(backend.intake_calls().empty());
}

TEST_F(HookIpcTest, AQuestionArrivesAsEventKind6AndTheAnswersReachTheBackend) {
    const RunningServer server{sys, backend, hook_config()};
    const auto client = connect_ready(path());
    ASSERT_TRUE(backend.push(HostEventType::Handshake, 11, "blob").has_value());
    const auto question = client->receive();
    ASSERT_TRUE(question);
    EXPECT_EQ(question->kind, Reply::Kind::Event);
    EXPECT_EQ(question->event, ipc::EventKind::Handshake);
    EXPECT_EQ(question->conn, 11U);
    EXPECT_EQ(question->data, "blob");

    ipc::HandshakeAccept accept;
    accept.request = 1;
    accept.conn = 11;
    const std::string chosen = "chat";
    accept.subprotocol =
        ByteView{reinterpret_cast<const std::byte*>(chosen.data()), chosen.size()};  // NOLINT
    client->send_message(accept);
    const auto accepted = client->receive();
    ASSERT_TRUE(accepted);
    EXPECT_EQ(accepted->kind, Reply::Kind::Status);
    EXPECT_EQ(accepted->request, 1U);
    EXPECT_EQ(accepted->status, kOk);

    client->send_message(ipc::HandshakeReject{.request = 2, .conn = 12, .status = 403});
    EXPECT_EQ(client->receive()->status, kOk);
    const auto commands = backend.commands();
    ASSERT_EQ(commands.size(), 2U);
    EXPECT_EQ(commands[0].kind, RecordedCommand::Kind::Accept);
    EXPECT_EQ(commands[0].conn, 11U);
    EXPECT_EQ(commands[0].data, "chat");
    EXPECT_EQ(commands[1].kind, RecordedCommand::Kind::Reject);
    EXPECT_EQ(commands[1].conn, 12U);
    EXPECT_EQ(commands[1].code, 403U);
}

TEST_F(HookIpcTest, AQuestionTooBigForAFrameIsRefusedWith503AndTheLinkSurvives) {
    const RunningServer server{sys, backend, hook_config()};
    const auto client = connect_ready(path());
    ASSERT_TRUE(
        backend.push(HostEventType::Handshake, 21, std::string(kMaxFrame, 'x')).has_value());
    ASSERT_TRUE(backend.push(HostEventType::Open, 22, "peer").has_value());
    const auto next = client->receive();  // the oversized question is skipped, not fatal
    ASSERT_TRUE(next);
    EXPECT_EQ(next->event, ipc::EventKind::Open);
    EXPECT_EQ(next->conn, 22U);
    const auto commands = backend.commands();
    ASSERT_EQ(commands.size(), 1U);
    EXPECT_EQ(commands[0].kind, RecordedCommand::Kind::Reject);
    EXPECT_EQ(commands[0].conn, 21U);
    EXPECT_EQ(commands[0].code, 503U);
}

/// The whole chain with the hook on: a browser, the real WebSocket server, the daemon, an IPC
/// application that decides.
struct HookEndToEnd : public ::testing::Test {
    static std::unique_ptr<wsrv::EmbeddedServer> start_web_server() {
        wsrv::EmbeddedConfig web;
        EXPECT_TRUE(web.set("allow_origin", "https://app.example.com").has_value());
        EXPECT_TRUE(web.set("shutdown_grace_ms", "1000").has_value());
        EXPECT_TRUE(web.set("handshake_hook", "true").has_value());
        EXPECT_TRUE(web.set("handshake_forward_header", "Cookie").has_value());
        // Long on purpose: a 503 that arrives sooner was not the timeout.
        EXPECT_TRUE(web.set("handshake_decision_timeout_ms", "30000").has_value());
        auto created = wsrv::EmbeddedServer::create(web);
        EXPECT_TRUE(created.has_value());
        EXPECT_TRUE((*created)->start().has_value());
        return std::move(*created);
    }

    [[nodiscard]] IpcServerConfig config() const {
        IpcServerConfig c;
        c.socket_path = dir.file("ipc.sock");
        c.max_frame = 1U << 20U;
        c.restart_grace = 400ms;
        c.handshake_hook = true;
        return c;
    }
    [[nodiscard]] std::string path() const {
        return dir.file("ipc.sock");
    }

    FakeSyscalls sys;
    TempDir dir;
    std::unique_ptr<wsrv::EmbeddedServer> embedded = start_web_server();
    wsrv::wsrvd::EmbeddedBackend backend{*embedded};
};

TEST_F(HookEndToEnd, TheApplicationDecidesAndTheBrowserGetsThatAnswer) {
    const RunningServer server{sys, backend, config()};
    const auto app = connect_ready(path());

    wsrv::test::TcpClient allowed{embedded->port()};
    std::string request = wsrv::test::TcpClient::upgrade_request();
    request.insert(request.size() - 2, "Cookie: session=good\r\n");
    ASSERT_TRUE(allowed.send(request));
    const auto question = app->receive();
    ASSERT_TRUE(question);
    ASSERT_EQ(question->event, ipc::EventKind::Handshake);
    const auto blob =
        wsrv::decode_handshake(ByteView{reinterpret_cast<const std::byte*>(question->data.data()),
                                        question->data.size()});  // NOLINT
    ASSERT_TRUE(blob.has_value());
    EXPECT_EQ(blob->path, "/chat");
    ASSERT_EQ(blob->headers.size(), 1U);
    EXPECT_EQ(blob->headers[0].second, "session=good");
    EXPECT_TRUE(allowed.silent_for(100ms)) << "no 101 until the application answers";

    app->send_message(
        ipc::HandshakeAccept{.request = 1, .conn = question->conn, .subprotocol = {}});
    EXPECT_EQ(app->receive()->status, kOk);
    EXPECT_EQ(allowed.read_http_head().rfind("HTTP/1.1 101 ", 0), 0U);
    const auto open = app->receive();
    ASSERT_TRUE(open);
    EXPECT_EQ(open->event, ipc::EventKind::Open);
    EXPECT_EQ(open->conn, question->conn) << "the id of the question is the id of the connection";

    wsrv::test::TcpClient refused{embedded->port()};
    ASSERT_TRUE(refused.send(wsrv::test::TcpClient::upgrade_request()));
    const auto second = app->receive();
    ASSERT_TRUE(second);
    ASSERT_EQ(second->event, ipc::EventKind::Handshake);
    app->send_message(ipc::HandshakeReject{.request = 2, .conn = second->conn, .status = 401});
    EXPECT_EQ(app->receive()->status, kOk);
    EXPECT_EQ(refused.read_http_head().rfind("HTTP/1.1 401 ", 0), 0U);
}

TEST_F(HookEndToEnd, WithNoApplicationConnectedUpgradesAreRefusedAtOnceNotHeld) {
    const RunningServer server{sys, backend, config()};
    wsrv::test::TcpClient browser{embedded->port()};
    ASSERT_TRUE(browser.send(wsrv::test::TcpClient::upgrade_request()));
    EXPECT_EQ(browser.read_http_head().rfind("HTTP/1.1 503 ", 0), 0U) << "fail closed";
}

TEST_F(HookEndToEnd, WhenTheApplicationDiesWhileAskedTheWaitingBrowserGets503AtOnce) {
    const RunningServer server{sys, backend, config()};
    wsrv::test::TcpClient browser{embedded->port()};
    {
        const auto app = connect_ready(path());
        ASSERT_TRUE(browser.send(wsrv::test::TcpClient::upgrade_request()));
        const auto question = app->receive();
        ASSERT_TRUE(question);
        ASSERT_EQ(question->event, ipc::EventKind::Handshake);
    }  // the application dies without answering; the server's own wait is 30 s
    EXPECT_EQ(browser.read_http_head().rfind("HTTP/1.1 503 ", 0), 0U);
}

}  // namespace

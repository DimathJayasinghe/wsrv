// SPDX-License-Identifier: Apache-2.0
#include "wsrvd/ipc_server.hpp"

#include "base/byte_buffer.hpp"
#include "base/clock.hpp"
#include "capi/embedded_server.hpp"
#include "support/fake_backend.hpp"
#include "support/fake_syscalls.hpp"
#include "support/tcp_client.hpp"
#include "support/wire.hpp"
#include "wsrv/wsrv.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

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
using wsrv::test::FakeSyscalls;
using wsrv::wsrvd::IpcServer;
using wsrv::wsrvd::IpcServerConfig;
namespace ipc = wsrv::ipc;

constexpr std::uint32_t kMaxFrame = 8192;

/// A private directory removed with its contents.
class TempDir {
public:
    TempDir() {
        std::string pattern = (fs::temp_directory_path() / "wsrv-ipcd-XXXXXX").string();
        EXPECT_NE(::mkdtemp(pattern.data()), nullptr);
        path_ = pattern;
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    [[nodiscard]] std::string file(const std::string& name) const {
        return path_ + "/" + name;
    }

private:
    std::string path_;
};

/// The server running on its own thread, as EmbeddedServer does: the loop and the server are
/// created, run and destroyed on that thread; the test thread only talks to the socket.
class RunningServer {
public:
    RunningServer(FakeSyscalls& sys, wsrv::wsrvd::IBackend& backend, IpcServerConfig config) {
        std::promise<std::optional<ErrorCode>> started;
        auto started_future = started.get_future();
        thread_ = std::thread([this, &sys, &backend, config = std::move(config), &started]() mutable {
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

private:
    std::thread thread_;
    std::mutex mutex_;
    EventLoop* loop_ = nullptr;  // guarded by mutex_
    std::optional<ErrorCode> failure_;
};

/// What a test cares about in a reply.
struct Reply {
    enum class Kind : std::uint8_t { HelloOk, Status, Pong, Event, Other } kind = Kind::Other;
    std::uint32_t request = 0;
    std::uint8_t status = 0;
    std::uint32_t max_frame = 0;
    // EVENT frames
    ipc::EventKind event = ipc::EventKind::Open;
    std::uint64_t conn = 0;
    std::uint16_t close_code = 0;
    bool clean = false;
    std::string data;
};

const sockaddr* as_generic(const sockaddr_un& address) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<const sockaddr*>(&address);
}

/// A blocking client with a receive timeout, so a server that misbehaves fails a test instead of
/// hanging it.
class Client {
public:
    explicit Client(const std::string& path, std::chrono::milliseconds timeout = 3000ms)
        : fd_{::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)}, decoder_{1U << 20U} {
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        connected_ = ::connect(fd_, as_generic(address), sizeof(address)) == 0;
        timeval tv{};
        tv.tv_sec = static_cast<time_t>(timeout.count() / 1000);
        tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    ~Client() {
        ::close(fd_);
    }
    [[nodiscard]] bool connected() const {
        return connected_;
    }

    void send_raw(const std::vector<std::byte>& bytes) const {
        ASSERT_EQ(::send(fd_, bytes.data(), bytes.size(), MSG_NOSIGNAL),
                  static_cast<ssize_t>(bytes.size()));
    }
    void send_message(const ipc::Message& message) const {
        send_raw(encode(message));
    }
    /// For a peer the server is expected to drop: it may already have closed by the time we send,
    /// and that is a valid outcome, so a failed send is not a test failure.
    void send_hello_if_still_open() const {
        const std::vector<std::byte> bytes = encode(ipc::Hello{});
        (void)::send(fd_, bytes.data(), bytes.size(), MSG_NOSIGNAL);
    }
    void send_hello(std::string_view token = "") const {
        ipc::Hello hello;
        hello.token = ByteView{reinterpret_cast<const std::byte*>(token.data()),  // NOLINT
                               token.size()};
        send_message(hello);
    }

    /// The next frame, or nothing on EOF / timeout.
    std::optional<Reply> receive() {
        for (;;) {
            ByteView view{pending_};
            const auto before = view.size();
            auto next = decoder_.next(view);
            const auto consumed = before - view.size();
            if (!next) {
                return std::nullopt;
            }
            if (next->has_value()) {
                const Reply reply = to_reply(**next);
                pending_.erase(pending_.begin(),
                               pending_.begin() + static_cast<std::ptrdiff_t>(consumed));
                return reply;
            }
            pending_.erase(pending_.begin(),
                           pending_.begin() + static_cast<std::ptrdiff_t>(consumed));
            std::array<std::byte, 512> buffer{};
            const ssize_t n = ::recv(fd_, buffer.data(), buffer.size(), 0);
            if (n <= 0) {
                return std::nullopt;
            }
            pending_.insert(pending_.end(), buffer.begin(), buffer.begin() + n);
        }
    }

    /// True if the server closes the connection (EOF) without sending anything more.
    [[nodiscard]] bool closed_by_server() const {
        std::array<std::byte, 64> buffer{};
        for (;;) {
            const ssize_t n = ::recv(fd_, buffer.data(), buffer.size(), 0);
            if (n == 0) {
                return true;
            }
            if (n < 0) {
                return errno == ECONNRESET;  // a reset is a close too; a timeout is not
            }
            return false;  // data arrived: it was not silent
        }
    }

    static std::vector<std::byte> encode(const ipc::Message& message) {
        wsrv::ByteBuffer out{1U << 20U};
        EXPECT_TRUE(ipc::encode(message, 1U << 20U, out).has_value());
        const ByteView view = out.view();
        return {view.begin(), view.end()};
    }

private:
    static Reply to_reply(const ipc::Message& message) {
        Reply reply;
        if (const auto* ok = std::get_if<ipc::HelloOk>(&message)) {
            reply.kind = Reply::Kind::HelloOk;
            reply.max_frame = ok->max_frame;
        } else if (const auto* status = std::get_if<ipc::Status>(&message)) {
            reply.kind = Reply::Kind::Status;
            reply.request = status->request;
            reply.status = status->status;
        } else if (std::holds_alternative<ipc::Pong>(message)) {
            reply.kind = Reply::Kind::Pong;
        } else if (const auto* event = std::get_if<ipc::Event>(&message)) {
            reply.kind = Reply::Kind::Event;
            reply.event = event->kind;
            reply.conn = event->conn;
            reply.close_code = event->close_code;
            reply.clean = event->clean;
            reply.data = std::string{wsrv::as_chars(event->data)};
        }
        return reply;
    }

    int fd_;
    bool connected_ = false;
    ipc::FrameDecoder decoder_;
    std::vector<std::byte> pending_;
};

/// A frame header with an arbitrary declared length: `u32 length, u8 type, u8 flags`.
std::vector<std::byte> header(std::uint32_t length, std::uint8_t type, std::uint8_t flags = 0) {
    return {std::byte(length >> 24U),
            std::byte((length >> 16U) & 0xFFU),
            std::byte((length >> 8U) & 0xFFU),
            std::byte(length & 0xFFU),
            std::byte(type),
            std::byte(flags)};
}

struct IpcServerTest : public ::testing::Test {
    FakeSyscalls sys;
    wsrv::testing::FakeBackend backend;
    TempDir dir;

    [[nodiscard]] IpcServerConfig config(std::string token = "") const {
        IpcServerConfig c;
        c.socket_path = dir.file("ipc.sock");
        c.token = std::move(token);
        c.max_frame = kMaxFrame;
        c.hello_timeout = 5000ms;
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
    command.data = ByteView{reinterpret_cast<const std::byte*>(text.data()), text.size()};  // NOLINT
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

TEST_F(IpcServerTest, AnApplicationThatReadsPromptlyIsNeverDisconnected) {
    IpcServerConfig c = config();
    c.stall_timeout = 1500ms;
    const RunningServer server{sys, backend, c};
    const auto client = connect_ready(path());
    push_burst(backend);
    std::this_thread::sleep_for(300ms);  // slow, but well inside the stall timeout
    for (std::size_t i = 0; i < kBurst + 1; ++i) {
        ASSERT_TRUE(client->receive()) << "cut off at event " << i;
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
    EXPECT_TRUE(backend.commands().empty()) << "the grace period has not passed yet";
    ASSERT_TRUE(eventually([&] { return !backend.commands().empty(); }));
    const auto commands = backend.commands();
    ASSERT_EQ(commands.size(), 1U);
    EXPECT_EQ(commands[0].kind, RecordedCommand::Kind::Close);
    EXPECT_EQ(commands[0].conn, 5U);
    EXPECT_EQ(commands[0].code, 1012U);
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
    {
        const auto app = connect_ready(path());
    }
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
    ASSERT_TRUE(eventually([&] { return !backend.commands().empty(); }, 10000ms));
    const auto commands = backend.commands();
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

}  // namespace

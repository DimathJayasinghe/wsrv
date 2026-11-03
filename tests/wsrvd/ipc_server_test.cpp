// SPDX-License-Identifier: Apache-2.0
#include "wsrvd/ipc_server.hpp"

#include "base/byte_buffer.hpp"
#include "base/clock.hpp"
#include "support/fake_syscalls.hpp"
#include "wsrv/wsrv.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <memory>
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
    RunningServer(FakeSyscalls& sys, IpcServerConfig config) {
        std::promise<std::optional<ErrorCode>> started;
        auto started_future = started.get_future();
        thread_ = std::thread([this, &sys, config = std::move(config), &started]() mutable {
            const SteadyClock clock;
            auto loop = EventLoop::create(sys, clock);
            if (!loop) {
                started.set_value(loop.error().code);
                return;
            }
            auto server = IpcServer::create(**loop, sys, std::move(config));
            if (!server) {
                started.set_value(server.error().code);
                return;
            }
            loop_.store(loop->get());
            started.set_value(std::nullopt);
            const wsrv::Result<void> ran = (*loop)->run();
            static_cast<void>(ran);
            loop_.store(nullptr);
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
        if (EventLoop* loop = loop_.load()) {
            loop->stop();
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
    std::atomic<EventLoop*> loop_{nullptr};
    std::optional<ErrorCode> failure_;
};

/// What a test cares about in a reply.
struct Reply {
    enum class Kind : std::uint8_t { HelloOk, Status, Pong, Other } kind = Kind::Other;
    std::uint32_t request = 0;
    std::uint8_t status = 0;
    std::uint32_t max_frame = 0;
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
    const RunningServer server{sys, config("s3cret")};
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
    const RunningServer server{sys, config("s3cret")};
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
    const RunningServer server{sys, config()};
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
    const RunningServer server{sys, config()};
    Client client{path()};
    client.send_message(ipc::Ping{});
    const auto reply = client.receive();
    ASSERT_TRUE(reply);
    EXPECT_EQ(reply->kind, Reply::Kind::Status);
    EXPECT_EQ(reply->status, kInvalidArgument);
    EXPECT_TRUE(client.closed_by_server());
}

TEST_F(IpcServerTest, AHelloSentOneByteAtATimeStillWorks) {
    const RunningServer server{sys, config()};
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
    const RunningServer server{sys, config()};
    Client client{path()};
    std::vector<std::byte> both = Client::encode(ipc::Hello{});
    const std::vector<std::byte> ping = Client::encode(ipc::Ping{});
    both.insert(both.end(), ping.begin(), ping.end());
    client.send_raw(both);  // the widened decoder must start with the bytes after HELLO
    EXPECT_EQ(client.receive()->kind, Reply::Kind::HelloOk);
    EXPECT_EQ(client.receive()->kind, Reply::Kind::Pong);
}

TEST_F(IpcServerTest, PingIsAnsweredWithPongOnceConnected) {
    const RunningServer server{sys, config()};
    Client client{path()};
    client.send_hello();
    ASSERT_EQ(client.receive()->kind, Reply::Kind::HelloOk);
    client.send_message(ipc::Ping{});
    EXPECT_EQ(client.receive()->kind, Reply::Kind::Pong);
}

// ---- one consumer -----------------------------------------------------------------------------

TEST_F(IpcServerTest, ASecondApplicationIsToldTheSlotIsTakenAndTheFirstIsUndisturbed) {
    const RunningServer server{sys, config()};
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
    const RunningServer server{sys, config()};
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
    const RunningServer server{sys, c};
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
    const RunningServer server{sys, c};
    Client client{path()};
    std::this_thread::sleep_for(100ms);
    client.send_hello();
    EXPECT_EQ(client.receive()->kind, Reply::Kind::HelloOk);
    std::this_thread::sleep_for(600ms);  // the deadline is over: it must not fire any more
    client.send_message(ipc::Ping{});
    EXPECT_EQ(client.receive()->kind, Reply::Kind::Pong);
}

TEST_F(IpcServerTest, AHeaderLargerThanAnyHelloClosesTheConnectionWithoutReadingTheBody) {
    const RunningServer server{sys, config()};
    const Client client{path()};
    client.send_raw(header(1U << 20U, static_cast<std::uint8_t>(ipc::FrameType::Hello)));
    EXPECT_TRUE(client.closed_by_server());  // the 1 MiB body was never awaited
}

TEST_F(IpcServerTest, AHeaderAboveTheFrameLimitAfterHelloClosesTheConnection) {
    const RunningServer server{sys, config()};
    Client client{path()};
    client.send_hello();
    ASSERT_EQ(client.receive()->kind, Reply::Kind::HelloOk);
    client.send_raw(header(kMaxFrame + 1, static_cast<std::uint8_t>(ipc::FrameType::SendText)));
    EXPECT_TRUE(client.closed_by_server());
}

TEST_F(IpcServerTest, MalformedFramesCloseTheConnectionWithoutAReply) {
    const RunningServer server{sys, config()};
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

TEST_F(IpcServerTest, AFrameTheDaemonDoesNotHandleYetClosesTheConnection) {
    // Slice 3 adds commands; until then anything but PING after HELLO is a protocol error.
    const RunningServer server{sys, config()};
    Client client{path()};
    client.send_hello();
    ASSERT_EQ(client.receive()->kind, Reply::Kind::HelloOk);
    ipc::SendData send;
    send.request = 1;
    send.conn = 7;
    client.send_message(send);
    EXPECT_TRUE(client.closed_by_server());
}

TEST_F(IpcServerTest, AHelloAfterTheHandshakeClosesTheConnection) {
    const RunningServer server{sys, config()};
    Client client{path()};
    client.send_hello();
    ASSERT_EQ(client.receive()->kind, Reply::Kind::HelloOk);
    client.send_hello();
    EXPECT_TRUE(client.closed_by_server());
}

// ---- who may connect --------------------------------------------------------------------------

TEST_F(IpcServerTest, APeerWithAnotherUidIsDroppedWithoutAReply) {
    const RunningServer server{sys, config()};
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
    const RunningServer server{sys, config()};
    sys.fail(Call::PeerUid, EINVAL);
    const Client client{path()};
    client.send_hello_if_still_open();
    EXPECT_TRUE(client.closed_by_server());
}

// ---- lifecycle --------------------------------------------------------------------------------

TEST_F(IpcServerTest, TheSocketFileIsRemovedWhenTheServerStops) {
    {
        const RunningServer server{sys, config()};
        ASSERT_TRUE(fs::exists(path()));
    }
    EXPECT_FALSE(fs::exists(path()));
}

TEST_F(IpcServerTest, ASecondDaemonOnTheSameSocketIsRefusedAndTheFirstKeepsWorking) {
    const RunningServer first{sys, config()};
    ASSERT_FALSE(first.failure());
    FakeSyscalls other_sys;
    const RunningServer second{other_sys, config()};
    ASSERT_TRUE(second.failure());
    EXPECT_EQ(*second.failure(), ErrorCode::AddressInUse);
    Client client{path()};
    client.send_hello();
    EXPECT_EQ(client.receive()->kind, Reply::Kind::HelloOk);
}

TEST_F(IpcServerTest, StoppingWithAConnectedApplicationClosesItsConnection) {
    auto server = std::make_unique<RunningServer>(sys, config());
    Client client{path()};
    client.send_hello();
    ASSERT_EQ(client.receive()->kind, Reply::Kind::HelloOk);
    server.reset();
    EXPECT_TRUE(client.closed_by_server());
}

}  // namespace

// SPDX-License-Identifier: Apache-2.0
// The whole server on a real loopback port: accept, handshake, echo, caps, churn, shutdown.
#include "server/message_writer.hpp"
#include "server/server_builder.hpp"
#include "server/session_handle.hpp"
#include "support/fake_syscalls.hpp"
#include "support/fakes.hpp"
#include "support/running_server.hpp"
#include "support/tcp_client.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using wsrv::CloseCode;
using wsrv::Opcode;
using wsrv::Session;
using wsrv::test::RunningServer;
using wsrv::test::TcpClient;

class EchoHandler final : public wsrv::IWebSocketHandler {
public:
    void on_text(Session& session, std::string_view text) override {
        static_cast<void>(session.send_text(text).has_value());
    }
    void on_binary(Session& session, wsrv::ByteView data) override {
        static_cast<void>(session.send_binary(data).has_value());
    }
};

class EchoFactory final : public wsrv::IHandlerFactory {
public:
    std::unique_ptr<wsrv::IWebSocketHandler> create(const wsrv::ConnectionInfo&) override {
        return std::make_unique<EchoHandler>();
    }
};

wsrv::ServerBuilder echo_builder(EchoFactory& factory) {
    wsrv::ServerBuilder builder;
    builder.listen("127.0.0.1", 0).handler_factory(factory);
    builder.config().handshake.allowed_origins = {"https://app.example.com"};
    return builder;
}

std::size_t open_descriptors() {
    std::size_t count = 0;
    for ([[maybe_unused]] const auto& entry :
         std::filesystem::directory_iterator{"/proc/self/fd"}) {
        ++count;
    }
    return count;
}

/// Poll until `done()` or 5 s pass; the server runs on its own thread.
template <class Done>
bool eventually(Done&& done) {
    for (int i = 0; i < 500; ++i) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

TEST(Server, EchoesATextMessageOverARealSocket) {
    EchoFactory factory;
    RunningServer running{echo_builder(factory)};
    TcpClient client{running.port()};
    ASSERT_TRUE(client.valid());
    ASSERT_TRUE(client.upgrade());
    ASSERT_TRUE(client.send_frame(Opcode::Text, "hello over tcp"));
    const auto frame = client.read_frame();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->opcode, Opcode::Text);
    EXPECT_EQ(frame->payload, "hello over tcp");
}

TEST(Server, ARefusedOriginGets403AndIsClosed) {
    EchoFactory factory;
    RunningServer running{echo_builder(factory)};
    TcpClient client{running.port()};
    ASSERT_TRUE(client.valid());
    EXPECT_FALSE(client.upgrade("https://evil.example"));
    EXPECT_TRUE(client.wait_for_eof());
}

TEST(Server, TheConnectionCapRefusesTheExtraClientCheaply) {  // FR-S1, T4
    EchoFactory factory;
    wsrv::ServerBuilder builder = echo_builder(factory);
    builder.config().max_connections = 2;
    RunningServer running{builder};
    TcpClient first{running.port()};
    TcpClient second{running.port()};
    ASSERT_TRUE(first.upgrade());
    ASSERT_TRUE(second.upgrade());
    TcpClient third{
        running.port()};  // the TCP connect succeeds (the kernel queue), then it is closed
    EXPECT_TRUE(third.wait_for_eof());
    EXPECT_EQ(running.server().rejected_connections(), 1U);
    ASSERT_TRUE(first.send_frame(Opcode::Text, "still fine"));  // the others are unaffected
    ASSERT_TRUE(first.read_frame().has_value());
}

TEST(Server, ChurnLeavesNoDescriptorsAndTheCapFreeAgain) {  // T15, NFR-R3
    EchoFactory factory;
    wsrv::ServerBuilder builder = echo_builder(factory);
    builder.config().max_connections = 8;
    // This test is about the global cap and descriptor hygiene, not the per-IP limits (ADR-0021).
    builder.config().max_connections_per_ip = 0;
    builder.config().max_new_connections_per_ip_per_second = 0;
    RunningServer running{builder};
    const std::size_t before = open_descriptors();
    for (int round = 0; round < 100; ++round) {
        TcpClient client{running.port()};
        ASSERT_TRUE(client.valid());
        if (round % 2 == 0) {
            ASSERT_TRUE(client.upgrade());
            ASSERT_TRUE(client.send_frame(Opcode::Text, "x"));
            ASSERT_TRUE(client.read_frame().has_value());
        }  // odd rounds: connect and vanish without a byte
    }
    EXPECT_TRUE(eventually([&] { return open_descriptors() <= before; }));
    TcpClient after{running.port()};  // the cap of 8 was never exhausted by dead connections
    EXPECT_TRUE(after.upgrade());
}

TEST(Server, AGracefulShutdownSends1001AndEndsOnceClientsAnswer) {  // EC-C18, FR-S5
    EchoFactory factory;
    RunningServer running{echo_builder(factory)};
    TcpClient client{running.port()};
    ASSERT_TRUE(client.upgrade());
    ASSERT_TRUE(running.server().request_shutdown().has_value());
    const auto close = client.read_frame();
    ASSERT_TRUE(close.has_value());
    EXPECT_EQ(close->opcode, Opcode::Close);
    EXPECT_EQ(close->payload.substr(0, 2), std::string("\x03\xe9", 2));
    ASSERT_TRUE(client.send_frame(Opcode::Close, wsrv::test::close_payload(1001)));
    EXPECT_TRUE(client.wait_for_eof());
    EXPECT_TRUE(eventually([&] { return running.finished(); }));
}

TEST(Server, AClientThatIgnoresTheShutdownIsDroppedAfterTheGracePeriod) {
    EchoFactory factory;
    wsrv::ServerBuilder builder = echo_builder(factory);
    builder.config().shutdown_grace = 300ms;
    RunningServer running{builder};
    TcpClient client{running.port()};
    ASSERT_TRUE(client.upgrade());
    ASSERT_TRUE(running.server().request_shutdown().has_value());
    ASSERT_TRUE(client.read_frame().has_value());  // the 1001, which it ignores
    EXPECT_TRUE(client.wait_for_eof());
    EXPECT_TRUE(eventually([&] { return running.finished(); }));
}

TEST(Server, ASignalStartsAGracefulShutdownAndASecondOneForcesIt) {  // EC-N17
    EchoFactory factory;
    wsrv::ServerBuilder builder = echo_builder(factory);
    builder.config().shutdown_grace = 60s;  // only a second signal can end this quickly
    RunningServer running{builder, /*with_signals=*/true};
    TcpClient client{running.port()};
    ASSERT_TRUE(client.upgrade());
    running.signal(SIGTERM);
    ASSERT_TRUE(client.read_frame().has_value());  // 1001; the client never answers
    EXPECT_FALSE(running.finished());
    running.signal(SIGTERM);
    EXPECT_TRUE(eventually([&] { return running.finished(); }));
    EXPECT_TRUE(client.wait_for_eof());
}

TEST(ServerBuilder, RejectsABadConfigurationNamingTheField) {
    EchoFactory factory;
    wsrv::ServerBuilder builder = echo_builder(factory);
    builder.config().handshake_timeout = 0ms;
    try {
        static_cast<void>(builder.build());
        FAIL() << "expected a ConfigError";
    } catch (const wsrv::ConfigError& error) {
        EXPECT_NE(std::string{error.what()}.find("handshake_timeout"), std::string::npos);
    }
}

TEST(ServerBuilder, RejectsAHostnameAndAMissingFactory) {
    EchoFactory factory;
    wsrv::ServerBuilder named;
    named.listen("localhost", 0).handler_factory(factory);
    EXPECT_THROW(static_cast<void>(named.build()), wsrv::ConfigError);  // no DNS on the loop
    wsrv::ServerBuilder no_factory;
    EXPECT_THROW(static_cast<void>(no_factory.build()), wsrv::ConfigError);
}

TEST(ServerBuilder, APortAlreadyInUseIsAClearStartupError) {  // EC-N20
    EchoFactory factory;
    RunningServer running{echo_builder(factory)};
    wsrv::ServerBuilder second;
    second.listen("127.0.0.1", running.port()).handler_factory(factory);
    try {
        static_cast<void>(second.build());
        FAIL() << "expected a ConfigError";
    } catch (const wsrv::ConfigError& error) {
        EXPECT_NE(std::string{error.what()}.find("AddressInUse"), std::string::npos);
    }
}

// ADR-0022: the descriptor budget is checked before anything is bound.
TEST(ServerBuilder, ADescriptorLimitBelowTheCapRefusesToStartBeforeBinding) {  // T4
    EchoFactory factory;
    wsrv::test::FakeSyscalls sys;
    sys.pretend_fd_limits(256, 256);
    wsrv::test::RecordingLogger logger;
    wsrv::ServerBuilder builder = echo_builder(factory);
    builder.config().max_connections = 1000;
    builder.syscalls(sys).logger(logger);
    try {
        static_cast<void>(builder.build());
        FAIL() << "expected a ConfigError";
    } catch (const wsrv::ConfigError& error) {
        EXPECT_NE(std::string{error.what()}.find("FdLimit"), std::string::npos);
    }
    EXPECT_EQ(sys.calls(wsrv::test::Call::Bind), 0U) << "nothing was bound";
    ASSERT_FALSE(logger.entries.empty());
    const std::string& line = logger.entries.back().message;
    EXPECT_NE(line.find("1000"), std::string::npos) << "the numbers are in the log: " << line;
    EXPECT_NE(line.find("256"), std::string::npos) << line;
}

TEST(ServerBuilder, ALowSoftLimitIsRaisedAtStartAndTheServerRuns) {
    EchoFactory factory;
    wsrv::test::FakeSyscalls sys;
    sys.pretend_fd_limits(256, 1'048'576);
    wsrv::ServerBuilder builder = echo_builder(factory);
    builder.config().max_connections = 1000;
    builder.syscalls(sys);
    RunningServer running{builder};
    TcpClient client{running.port()};
    EXPECT_TRUE(client.upgrade());
    EXPECT_GE(sys.pretended_fd_limits().soft, 1000U);
}

// ---- Streaming writer (ADR-0015) ----------------------------------------------------------------

/// Streams a generated message on request, resuming from on_writable whenever the queue is full:
/// the whole point of FR-F12. "binary N" / "text N" stream N pieces of kPiece bytes; "whole N"
/// tries to send the same amount in one send_binary and reports what that returned.
class StreamingHandler final : public wsrv::IWebSocketHandler {
public:
    static constexpr std::size_t kPiece = 16 * 1024;
    static constexpr std::string_view kUnit =
        "a\xE2\x82\xAC\xC3\xA9\xF0\x9F\x98\x80";  // 1+3+2+4 bytes

    void on_text(Session& session, std::string_view command) override {
        const std::size_t space = command.find(' ');
        const std::string_view verb = command.substr(0, space);
        total_ = std::stoul(std::string{command.substr(space + 1)});
        sent_ = 0;
        if (verb == "whole") {
            const auto refused = session.send_binary(std::vector<std::byte>(total_));
            static_cast<void>(
                session.send_text(refused ? "accepted" : wsrv::to_string(refused.error().code)));
            return;
        }
        text_ = verb == "text";
        auto begun = text_ ? session.begin_text() : session.begin_binary();
        if (!begun) {
            return;
        }
        writer_.emplace(std::move(*begun));
        pump();
    }
    void on_writable(Session& /*session*/) override {
        if (writer_) {
            pump();
        }
    }

private:
    /// Byte `i` of the message being streamed.
    std::byte byte_at(std::size_t i) const {
        return text_ ? static_cast<std::byte>(kUnit[i % kUnit.size()])
                     : static_cast<std::byte>(i % 251);
    }
    void pump() {
        while (sent_ < total_) {
            // Text pieces are 7 bytes so characters are split between writes on purpose.
            const std::size_t piece = std::min(text_ ? std::size_t{7} : kPiece, total_ - sent_);
            std::vector<std::byte> bytes(piece);
            for (std::size_t i = 0; i < piece; ++i) {
                bytes[i] = byte_at(sent_ + i);
            }
            if (!writer_->write(bytes).has_value()) {
                return;  // Backpressure: wait for on_writable
            }
            sent_ += piece;
        }
        static_cast<void>(writer_->finish().has_value());
        writer_.reset();
    }

    std::optional<wsrv::MessageWriter> writer_;
    std::size_t total_ = 0;
    std::size_t sent_ = 0;
    bool text_ = false;
};

class StreamingFactory final : public wsrv::IHandlerFactory {
public:
    std::unique_ptr<wsrv::IWebSocketHandler> create(const wsrv::ConnectionInfo&) override {
        return std::make_unique<StreamingHandler>();
    }
};

/// Read frames until the message ends; returns its opcode and payload, and checks the framing.
struct StreamedMessage {
    Opcode opcode = Opcode::Continuation;
    std::string payload;
    std::size_t frames = 0;
    bool well_formed = true;
};

StreamedMessage read_message(TcpClient& client) {
    StreamedMessage message;
    for (;;) {
        const auto frame = client.read_frame();
        if (!frame) {
            message.well_formed = false;
            return message;
        }
        if (message.frames == 0) {
            message.opcode = frame->opcode;
            message.well_formed = frame->opcode == Opcode::Text || frame->opcode == Opcode::Binary;
        } else {
            message.well_formed = message.well_formed && frame->opcode == Opcode::Continuation;
        }
        ++message.frames;
        message.payload += frame->payload;
        if (frame->fin) {
            return message;
        }
    }
}

wsrv::ServerBuilder streaming_builder(StreamingFactory& factory) {
    wsrv::ServerBuilder builder;
    builder.listen("127.0.0.1", 0).handler_factory(factory);
    builder.config().handshake.allowed_origins = {"https://app.example.com"};
    builder.config().session.outbound.max_queued_bytes = 64 * 1024;  // far below the message
    builder.config().session.outbound.max_frame_payload = 16 * 1024;
    return builder;
}

TEST(ServerStream, AMessageFarLargerThanTheQueueIsStreamedInBoundedMemory) {  // FR-F12, EC-F35
    constexpr std::size_t kTotal = 8 * 1024 * 1024;
    StreamingFactory factory;
    RunningServer running{streaming_builder(factory)};
    TcpClient client{running.port()};
    ASSERT_TRUE(client.upgrade());
    ASSERT_TRUE(client.send_frame(Opcode::Text, "whole " + std::to_string(kTotal)));
    const StreamedMessage whole = read_message(client);  // a single send cannot hold 8 MiB...
    EXPECT_EQ(whole.payload, "BufferFull");
    ASSERT_TRUE(
        client.send_frame(Opcode::Text, "binary " + std::to_string(kTotal)));  // ...a stream can
    const StreamedMessage message = read_message(client);
    ASSERT_TRUE(message.well_formed);
    EXPECT_EQ(message.opcode, Opcode::Binary);
    ASSERT_EQ(message.payload.size(), kTotal);
    EXPECT_GE(message.frames, kTotal / (16 * 1024));  // fragments of at most 16 KiB
    bool intact = true;
    for (std::size_t i = 0; i < kTotal && intact; i += 4099) {
        intact = static_cast<unsigned char>(message.payload[i]) == i % 251;
    }
    EXPECT_TRUE(intact);
}

TEST(ServerStream,
     ATextStreamMayCutCharactersBetweenWritesAndArrivesIntact) {  // EC-F36, RFC 6455 8.1
    constexpr std::size_t kTotal = 10 * StreamingHandler::kUnit.size() * 1000;
    StreamingFactory factory;
    RunningServer running{streaming_builder(factory)};
    TcpClient client{running.port()};
    ASSERT_TRUE(client.upgrade());
    ASSERT_TRUE(client.send_frame(Opcode::Text, "text " + std::to_string(kTotal)));
    const StreamedMessage message = read_message(client);
    ASSERT_TRUE(message.well_formed);
    EXPECT_EQ(message.opcode, Opcode::Text);
    std::string expected;
    for (std::size_t i = 0; i < kTotal; ++i) {
        expected += StreamingHandler::kUnit[i % StreamingHandler::kUnit.size()];
    }
    EXPECT_EQ(message.payload, expected);
}

TEST(ServerStream, APongIsAnsweredInTheMiddleOfAStream) {  // EC-C01, EC-F32
    constexpr std::size_t kTotal = 4 * 1024 * 1024;
    StreamingFactory factory;
    RunningServer running{streaming_builder(factory)};
    TcpClient client{running.port()};
    ASSERT_TRUE(client.upgrade());
    ASSERT_TRUE(client.send_frame(Opcode::Binary, ""));  // warm-up, ignored by the handler
    ASSERT_TRUE(client.send_frame(Opcode::Text, "binary " + std::to_string(kTotal)));
    ASSERT_TRUE(client.send_frame(Opcode::Ping, "mid"));
    bool pong = false;
    std::size_t received = 0;
    for (bool done = false; !done;) {
        const auto frame = client.read_frame();
        ASSERT_TRUE(frame.has_value());
        pong |= frame->opcode == Opcode::Pong && frame->payload == "mid";
        if (frame->opcode != Opcode::Pong) {
            received += frame->payload.size();
            done = frame->fin;
        }
    }
    EXPECT_TRUE(pong);
    EXPECT_EQ(received, kTotal);
}

TEST(ServerStream,
     ADisconnectInTheMiddleOfAStreamEndsCleanlyAndTheServerCarriesOn) {  // EC-N04, EC-N13
    StreamingFactory factory;
    RunningServer running{streaming_builder(factory)};
    {
        TcpClient quitter{running.port()};
        ASSERT_TRUE(quitter.upgrade());
        ASSERT_TRUE(quitter.send_frame(Opcode::Text, "binary " + std::to_string(64 * 1024 * 1024)));
        ASSERT_TRUE(quitter.read_frame().has_value());  // the stream has started...
    }  // ...and the client vanishes
    TcpClient other{running.port()};
    ASSERT_TRUE(other.upgrade());
    ASSERT_TRUE(other.send_frame(Opcode::Text, "binary 1000"));
    const StreamedMessage message = read_message(other);
    EXPECT_EQ(message.payload.size(), 1000U);
}

// ---- SessionHandle (ADR-0014) -------------------------------------------------------------------

/// Keeps the handle of the most recent connection, the way an application would.
class HandleKeeper final : public wsrv::IHandlerFactory {
public:
    std::unique_ptr<wsrv::IWebSocketHandler> create(const wsrv::ConnectionInfo&) override {
        return std::make_unique<Handler>(*this);
    }
    /// Wait until on_open stored a handle.
    wsrv::SessionHandle wait_for_handle() {
        for (int i = 0; i < 500; ++i) {
            {
                const std::lock_guard lock{mutex_};
                if (handle_.id() != 0) {
                    return handle_;
                }
            }
            std::this_thread::sleep_for(10ms);
        }
        return {};
    }

private:
    class Handler final : public wsrv::IWebSocketHandler {
    public:
        explicit Handler(HandleKeeper& keeper) : keeper_{keeper} {}
        void on_open(Session& session) override {
            const std::lock_guard lock{keeper_.mutex_};
            keeper_.handle_ = session.handle();
        }

    private:
        HandleKeeper& keeper_;
    };

    std::mutex mutex_;
    wsrv::SessionHandle handle_;
};

TEST(ServerHandle, AHandleKeptFromOnOpenSendsFromAnotherThread) {  // FR-S3
    HandleKeeper keeper;
    wsrv::ServerBuilder builder;
    builder.listen("127.0.0.1", 0).handler_factory(keeper);
    builder.config().handshake.allowed_origins = {"https://app.example.com"};
    RunningServer running{builder};
    TcpClient client{running.port()};
    ASSERT_TRUE(client.upgrade());
    const wsrv::SessionHandle handle = keeper.wait_for_handle();
    ASSERT_NE(handle.id(), 0U);
    std::thread pusher([&] {  // a thread that is not the loop thread
        EXPECT_TRUE(handle.send_text("pushed").has_value());
        EXPECT_TRUE(handle.close(CloseCode::Normal, "done").has_value());
    });
    pusher.join();
    const auto text = client.read_frame();
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(text->payload, "pushed");
    const auto close = client.read_frame();
    ASSERT_TRUE(close.has_value());
    EXPECT_EQ(close->opcode, Opcode::Close);
}

TEST(ServerHandle, AHandleIsHarmlessAfterTheConnectionAndThenTheServerAreGone) {  // EC-N13
    HandleKeeper keeper;
    wsrv::SessionHandle handle;
    {
        wsrv::ServerBuilder builder;
        builder.listen("127.0.0.1", 0).handler_factory(keeper);
        builder.config().handshake.allowed_origins = {"https://app.example.com"};
        RunningServer running{builder};
        {
            TcpClient client{running.port()};
            ASSERT_TRUE(client.upgrade());
            handle = keeper.wait_for_handle();
            ASSERT_NE(handle.id(), 0U);
            client.close();
            std::this_thread::sleep_for(200ms);  // the connection ends and is erased
        }
        EXPECT_TRUE(handle.send_text("to nobody").has_value());  // accepted, then dropped
        std::this_thread::sleep_for(50ms);
    }  // the server, its loop and its table are destroyed here
    const auto after = handle.send_text("to nothing");
    ASSERT_FALSE(after.has_value());
    EXPECT_EQ(after.error().code, wsrv::ErrorCode::Closing);
}

// ---- read pause (ADR-0016 section 4)
// --------------------------------------------------------------

/// Pauses reads after the first message and counts what it receives, so a test can see that a
/// paused connection delivers nothing and that a resume brings back what was waiting in the kernel.
class PausingHandler final : public wsrv::IWebSocketHandler {
public:
    struct Shared {
        std::atomic<int> received{0};
        std::mutex mutex;
        wsrv::SessionHandle handle;
    };
    explicit PausingHandler(Shared& shared) : shared_{shared} {}
    void on_open(Session& session) override {
        const std::lock_guard lock{shared_.mutex};
        shared_.handle = session.handle();
    }
    void on_text(Session& session, std::string_view) override {
        if (shared_.received.fetch_add(1) == 0) {
            session.pause_reads();
        }
    }

private:
    Shared& shared_;
};

class PausingFactory final : public wsrv::IHandlerFactory {
public:
    std::unique_ptr<wsrv::IWebSocketHandler> create(const wsrv::ConnectionInfo&) override {
        return std::make_unique<PausingHandler>(shared);
    }
    PausingHandler::Shared shared;
};

bool wait_until(const std::function<bool()>& condition) {
    for (int i = 0; i < 300; ++i) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return condition();
}

TEST(ServerReadPause, ABurstSentWhilePausedWaitsInTheKernelAndAResumeBringsItBack) {
    PausingFactory factory;
    wsrv::ServerBuilder builder;
    builder.listen("127.0.0.1", 0).handler_factory(factory);
    builder.config().handshake.allowed_origins = {"https://app.example.com"};
    RunningServer running{builder};
    TcpClient client{running.port()};
    ASSERT_TRUE(client.upgrade());
    ASSERT_TRUE(client.send_frame(Opcode::Text, "first"));
    ASSERT_TRUE(wait_until([&] { return factory.shared.received.load() == 1; }));

    for (int i = 0; i < 20; ++i) {  // sent while paused: the kernel buffers them
        ASSERT_TRUE(client.send_frame(Opcode::Text, "queued"));
    }
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(factory.shared.received.load(), 1);  // nothing was read

    wsrv::SessionHandle handle;
    {
        const std::lock_guard lock{factory.shared.mutex};
        handle = factory.shared.handle;
    }
    ASSERT_TRUE(handle.resume_reads().has_value());  // from this thread, not the loop's
    // The data was already waiting: only a fresh registration makes epoll report it (lesson 12).
    EXPECT_TRUE(wait_until([&] { return factory.shared.received.load() == 21; }));
}

}  // namespace

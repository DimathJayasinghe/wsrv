// SPDX-License-Identifier: Apache-2.0
// Connection behaviour with a fake transport and a fake host: no kernel, no real time.
#include "server/connection.hpp"

#include "support/fake_connection_env.hpp"
#include "support/wire.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using wsrv::CloseCode;
using wsrv::CloseInfo;
using wsrv::Connection;
using wsrv::ErrorCode;
using wsrv::Opcode;
using wsrv::PollEvents;
using wsrv::Session;
using wsrv::test::client_frame;
using wsrv::test::close_payload;
using wsrv::test::FakeHost;
using wsrv::test::FakeTransport;

constexpr std::string_view kRequest = "GET /chat HTTP/1.1\r\n"
                                      "Host: example.com\r\n"
                                      "Connection: Upgrade\r\n"
                                      "Upgrade: websocket\r\n"
                                      "Origin: https://app.example.com\r\n"
                                      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                      "Sec-WebSocket-Version: 13\r\n"
                                      "\r\n";
constexpr std::string_view kResponseStart = "HTTP/1.1 101 Switching Protocols\r\n";

/// What a test wants the handler to do, and what it saw.
struct Script {
    std::function<void(Session&)> on_open;
    std::function<void(Session&, std::string_view)> on_text;
    std::function<void(Session&, const CloseInfo&)> on_close;
    bool echo = false;
    int opened = 0;
    int closed = 0;
    int created = 0;
    std::vector<std::string> texts;
    std::vector<CloseInfo> closes;
    wsrv::Result<void> last_send = {};
};

class ScriptedHandler final : public wsrv::IWebSocketHandler {
public:
    explicit ScriptedHandler(Script& script) : script_{script} {}
    void on_open(Session& session) override {
        ++script_.opened;
        if (script_.on_open) {
            script_.on_open(session);
        }
    }
    void on_text(Session& session, std::string_view text) override {
        script_.texts.emplace_back(text);
        if (script_.echo) {
            script_.last_send = session.send_text(text);  // the view is passed straight back
        }
        if (script_.on_text) {
            script_.on_text(session, text);
        }
    }
    void on_close(Session& session, const CloseInfo& info) override {
        ++script_.closed;
        script_.closes.push_back(info);
        if (script_.on_close) {
            script_.on_close(session, info);
        }
    }

private:
    Script& script_;
};

class ScriptedFactory final : public wsrv::IHandlerFactory {
public:
    explicit ScriptedFactory(Script& script) : script_{script} {}
    std::unique_ptr<wsrv::IWebSocketHandler> create(const wsrv::ConnectionInfo& /*info*/) override {
        ++script_.created;
        return std::make_unique<ScriptedHandler>(script_);
    }

private:
    Script& script_;
};

wsrv::HandshakeConfig handshake_config() {
    wsrv::HandshakeConfig config;
    config.allowed_origins = {"https://app.example.com"};
    return config;
}

struct ConnectionTest : public ::testing::Test {
    FakeHost host;
    wsrv::ServerConfig config;
    Script script;
    ScriptedFactory factory{script};
    wsrv::HandshakeNegotiator negotiator{handshake_config()};
    std::vector<std::byte> scratch = std::vector<std::byte>(16 * 1024);
    FakeTransport* transport = nullptr;
    std::unique_ptr<Connection> conn;

    void open(std::size_t scratch_size = 16 * 1024) {
        scratch.resize(scratch_size);
        auto owned = std::make_unique<FakeTransport>();
        transport = owned.get();
        conn = std::make_unique<Connection>(
            7,
            wsrv::Endpoint{},
            std::move(owned),
            wsrv::ConnectionContext{
                host, config, negotiator, factory, wsrv::MutableByteView{scratch}, nullptr});
        ASSERT_TRUE(conn->start().has_value());
    }
    void readable() {
        conn->on_event(PollEvents::Read);
    }
    void send_bytes(std::string_view text) {
        transport->feed(text);
        readable();
    }
    void send_frame(Opcode opcode, std::string_view payload, bool fin = true) {
        const auto frame = client_frame(opcode, fin, payload);
        transport->feed(wsrv::ByteView{frame});
        readable();
    }
    /// Complete the handshake and forget the 101 so tests look at frames only.
    void establish() {
        open();
        send_bytes(kRequest);
        ASSERT_EQ(transport->written_text().rfind(kResponseStart, 0), 0U);
        transport->written.clear();
    }
    /// The frames written after the handshake.
    wsrv::test::ParsedOutput output() const {
        return wsrv::test::parse_server_output(wsrv::ByteView{transport->written});
    }
    static std::string code_of(const wsrv::test::WireFrame& frame) {
        return frame.payload.substr(0, 2);
    }
};

// ---- handshake phase ----------------------------------------------------------------------------

TEST_F(ConnectionTest, TheRequestMayArriveOneByteAtATimeAndIsAnsweredWith101) {  // EC-H17
    open();
    for (const char byte : kRequest) {
        send_bytes(std::string_view{&byte, 1});
    }
    EXPECT_EQ(transport->written_text().rfind(kResponseStart, 0), 0U);
    EXPECT_EQ(script.opened, 1);
    EXPECT_FALSE(conn->finished());
}

TEST_F(ConnectionTest, AMessageInTheSameReadAsTheHeadIsDelivered) {  // EC-H19, FR-H8
    open();
    const auto frame = client_frame(Opcode::Text, true, "early");
    send_bytes(std::string{kRequest} + std::string{wsrv::as_chars(wsrv::ByteView{frame})});
    ASSERT_EQ(script.texts.size(), 1U);
    EXPECT_EQ(script.texts[0], "early");
}

TEST_F(ConnectionTest, APartiallyWritten101IsFinishedWhenTheKernelDrains) {  // EC-N02
    open();
    transport->set_write_capacity(10);
    send_bytes(kRequest);
    EXPECT_EQ(transport->written.size(), 10U);
    EXPECT_TRUE(wsrv::has(host.interest_now, PollEvents::Write));  // armed while output waits
    transport->set_write_capacity(static_cast<std::size_t>(-1));
    conn->on_event(PollEvents::Write);
    EXPECT_EQ(transport->written_text().rfind(kResponseStart, 0), 0U);
    EXPECT_NE(transport->written_text().find("\r\n\r\n"), std::string::npos);
    EXPECT_FALSE(wsrv::has(host.interest_now, PollEvents::Write));  // disarmed once drained
}

TEST_F(ConnectionTest, AnEchoIsWrittenBehindTheHandshakeResponseNeverBeforeIt) {
    open();
    script.echo = true;
    transport->set_write_capacity(10);  // the 101 cannot be written in one go
    const auto frame = client_frame(Opcode::Text, true, "hi");
    send_bytes(std::string{kRequest} + std::string{wsrv::as_chars(wsrv::ByteView{frame})});
    transport->set_write_capacity(static_cast<std::size_t>(-1));
    conn->on_event(PollEvents::Write);
    const std::string all = transport->written_text();
    const std::size_t head_end = all.find("\r\n\r\n");
    ASSERT_NE(head_end, std::string::npos);
    EXPECT_EQ(all.substr(head_end + 4), std::string("\x81\x02hi", 4));
}

TEST_F(ConnectionTest, ARefusedUpgradeIsAnsweredAndTheConnectionClosed) {
    open();
    std::string request{kRequest};
    request.replace(request.find("https://app.example.com"), 23, "https://evil.example.x");
    send_bytes(request);
    EXPECT_EQ(transport->written_text().rfind("HTTP/1.1 403 ", 0), 0U);
    EXPECT_TRUE(conn->finished());
    EXPECT_TRUE(transport->closed);
    EXPECT_EQ(script.created, 0);  // no handler, no on_open
    EXPECT_EQ(host.retired.size(), 1U);
}

TEST_F(ConnectionTest, ThePeerLeavingMidHandshakeCreatesNoHandlerAndFreesEverything) {  // EC-H21
    open();
    send_bytes(kRequest.substr(0, 40));
    transport->feed_eof();
    readable();
    EXPECT_TRUE(conn->finished());
    EXPECT_EQ(script.created, 0);
    EXPECT_EQ(host.retired, std::vector<wsrv::ConnectionId>{7});
    EXPECT_EQ(host.pending_timers(), 0U);
    EXPECT_FALSE(host.watching);
}

TEST_F(ConnectionTest, AHandshakeThatNeverCompletesIsClosedAtTheTimeout) {  // EC-H20, T2
    open();
    send_bytes(kRequest.substr(0, 20));  // a slow loris: a few bytes, then silence
    host.advance(4900ms, 100ms);
    EXPECT_FALSE(conn->finished());
    host.advance(200ms, 100ms);
    EXPECT_TRUE(conn->finished());
    EXPECT_TRUE(transport->written.empty());  // no response is owed
}

TEST_F(ConnectionTest, GarbageOnThePlainPortIsRefusedWith400) {  // EC-H22
    open();
    send_bytes("\x16\x03\x01\x02\x00\x01\x00\x01\xfc\x03\x03 TLS\r\n\r\n");
    EXPECT_EQ(transport->written_text().rfind("HTTP/1.1 400 ", 0), 0U);
    EXPECT_TRUE(conn->finished());
}

// ---- messages and the read budget ---------------------------------------------------------------

TEST_F(ConnectionTest, ATextMessageIsDeliveredAndEchoedByteForByte) {
    establish();
    script.echo = true;
    send_frame(Opcode::Text, "hello");
    ASSERT_EQ(script.texts, std::vector<std::string>{"hello"});
    const auto out = output();
    ASSERT_EQ(out.frames.size(), 1U);
    EXPECT_EQ(out.frames[0].payload, "hello");
}

TEST_F(ConnectionTest, ALargeMessageSpanningManyReadsIsEchoedIntact) {  // view lifetime, FR-F1
    open(64);  // tiny scratch buffer: the message needs hundreds of reads
    send_bytes(kRequest);
    transport->written.clear();
    script.echo = true;
    config.read.max_reads_per_event = 100000;
    const std::string big(40000, 'x');
    send_frame(Opcode::Text, big);
    ASSERT_EQ(script.texts.size(), 1U);
    EXPECT_EQ(script.texts[0], big);
    std::size_t echoed = 0;
    for (const auto& frame : output().frames) {
        echoed += frame.payload.size();
    }
    EXPECT_EQ(echoed, big.size());
}

TEST_F(ConnectionTest, ABudgetedReadYieldsAndTheRestIsReportedAgain) {  // EC-N07, EC-N08
    open(8);  // an 8-byte scratch buffer: every frame needs several reads
    transport->feed(kRequest);
    for (int guard = 0; guard < 100 && script.opened == 0; ++guard) {
        readable();  // each event reads at most 16 x 8 bytes: the head needs a few events
    }
    ASSERT_EQ(script.opened, 1);
    script.echo = true;
    config.read.max_reads_per_event = 2;
    for (int i = 0; i < 4; ++i) {
        const auto frame = client_frame(Opcode::Text, true, "ping-pong");
        transport->feed(wsrv::ByteView{frame});
    }
    const std::size_t rewatches = host.rewatch_calls;
    readable();
    EXPECT_GT(transport->unread(), 0U);        // it stopped with data left...
    EXPECT_GT(host.rewatch_calls, rewatches);  // ...and asked the loop to report it again
    for (int guard = 0; guard < 200 && transport->unread() > 0; ++guard) {
        readable();
    }
    EXPECT_EQ(script.texts.size(), 4U);
}

TEST_F(ConnectionTest, AnEofInTheMiddleOfAMessageIsAnUncleanClose) {  // EC-N04
    establish();
    const auto frame = client_frame(Opcode::Text, false, "partial");
    transport->feed(wsrv::ByteView{frame});
    transport->feed_eof();
    readable();
    EXPECT_TRUE(conn->finished());
    ASSERT_EQ(script.closes.size(), 1U);
    EXPECT_FALSE(script.closes[0].clean);
    EXPECT_EQ(script.closes[0].code, CloseCode::AbnormalClosure);
    EXPECT_TRUE(script.texts.empty());  // the partial message is never delivered
}

// ---- close handshake and timers -----------------------------------------------------------------

TEST_F(ConnectionTest, ThePeersCloseIsEchoedAndOnCloseRunsOnce) {  // EC-C11
    establish();
    send_frame(Opcode::Close, close_payload(1000, "bye"));
    const auto out = output();
    ASSERT_EQ(out.frames.size(), 1U);
    EXPECT_EQ(out.frames[0].opcode, Opcode::Close);
    EXPECT_EQ(script.closed, 1);
    EXPECT_TRUE(script.closes[0].clean);
    EXPECT_EQ(script.closes[0].code, CloseCode::Normal);
    EXPECT_TRUE(conn->finished());
    EXPECT_TRUE(transport->shut_down);
    EXPECT_TRUE(transport->closed);
    EXPECT_EQ(host.retired.size(), 1U);
}

TEST_F(ConnectionTest, APeerThatNeverAnswersOurCloseIsDroppedAtTheCloseTimeout) {  // EC-C12
    establish();
    script.on_text = [](Session& s, std::string_view) { s.close(CloseCode::Normal, "done"); };
    send_frame(Opcode::Text, "x");
    EXPECT_FALSE(conn->finished());
    host.advance(config.session.close_timeout + 2s, 1s);
    EXPECT_TRUE(conn->finished());
    EXPECT_EQ(script.closed, 1);
    EXPECT_FALSE(script.closes[0].clean);
}

TEST_F(ConnectionTest, ASilentPeerIsPingedThenClosedWith1001) {  // EC-C16, EC-C17, EC-N11
    establish();
    host.advance(31s, 1s);
    const auto pinged = output();
    ASSERT_FALSE(pinged.frames.empty());
    EXPECT_EQ(pinged.frames[0].opcode, Opcode::Ping);
    host.advance(40s, 1s);
    bool closed_with_going_away = false;
    for (const auto& frame : output().frames) {
        closed_with_going_away |=
            frame.opcode == Opcode::Close && code_of(frame) == std::string("\x03\xe9", 2);
    }
    EXPECT_TRUE(closed_with_going_away);
}

TEST_F(ConnectionTest, APeerThatAcceptsNoBytesIsDroppedByTheStallTimer) {  // EC-N09, T3
    establish();
    script.echo = true;
    transport->set_write_capacity(0);  // the peer's receive window is full
    send_frame(Opcode::Text, "queued");
    EXPECT_FALSE(conn->finished());
    host.advance(config.write_stall_timeout + 2s, 1s);
    EXPECT_TRUE(conn->finished());
    EXPECT_TRUE(transport->closed);
    EXPECT_EQ(script.closed, 1);
}

TEST_F(ConnectionTest, AWriteToAGonePeerEndsTheConnectionUncleanlyOnce) {  // EC-N03
    establish();
    script.echo = true;
    transport->fail_writes(ErrorCode::ConnectionClosed);
    send_frame(Opcode::Text, "x");
    EXPECT_TRUE(conn->finished());
    ASSERT_EQ(script.closes.size(), 1U);
    EXPECT_FALSE(script.closes[0].clean);
    EXPECT_EQ(host.retired.size(), 1U);
}

// ---- backpressure -------------------------------------------------------------------------------

TEST_F(ConnectionTest, ASlowConsumerIsClosedWith1008ByDefault) {  // EC-N09
    config.session.outbound.max_queued_bytes = 200;
    establish();
    script.on_text = [&](Session& s, std::string_view) {
        script.last_send = s.send_text(std::string(150, 'a'));
        script.last_send = s.send_text(std::string(150, 'b'));  // does not fit behind the first
    };
    transport->set_write_capacity(0);
    send_frame(Opcode::Text, "go");
    ASSERT_FALSE(script.last_send.has_value());
    EXPECT_EQ(script.last_send.error().code, ErrorCode::Backpressure);
    transport->set_write_capacity(static_cast<std::size_t>(-1));
    conn->on_event(PollEvents::Write);
    bool policy_close = false;
    for (const auto& frame : output().frames) {
        policy_close |=
            frame.opcode == Opcode::Close && code_of(frame) == std::string("\x03\xf0", 2);
    }
    EXPECT_TRUE(policy_close);
}

TEST_F(ConnectionTest, TheDropPolicyKeepsTheConnectionOpen) {
    config.session.outbound.max_queued_bytes = 200;
    config.slow_consumer = wsrv::SlowConsumerPolicy::DropMessage;
    establish();
    script.on_text = [&](Session& s, std::string_view) {
        script.last_send = s.send_text(std::string(150, 'a'));
        script.last_send = s.send_text(std::string(150, 'b'));
    };
    transport->set_write_capacity(0);
    send_frame(Opcode::Text, "go");
    EXPECT_EQ(script.last_send.error().code, ErrorCode::Backpressure);
    EXPECT_FALSE(conn->finished());
}

// ---- handler contract ---------------------------------------------------------------------------

TEST_F(ConnectionTest, AThrowingHandlerClosesWith1011AndTheServerSurvives) {  // EC-A01, FR-S6
    establish();
    script.on_text = [](Session&, std::string_view) { throw std::runtime_error{"boom"}; };
    send_frame(Opcode::Text, "x");
    bool internal_error = false;
    for (const auto& frame : output().frames) {
        internal_error |=
            frame.opcode == Opcode::Close && code_of(frame) == std::string("\x03\xf3", 2);
    }
    EXPECT_TRUE(internal_error);
    EXPECT_TRUE(host.logged("handler threw"));
}

TEST_F(ConnectionTest, ANonStandardThrowIsContainedToo) {
    establish();
    script.on_text = [](Session&, std::string_view) { throw 42; };  // NOLINT: the point of the test
    send_frame(Opcode::Text, "x");
    EXPECT_FALSE(output().frames.empty());
    EXPECT_TRUE(host.logged("handler threw"));
}

TEST_F(ConnectionTest, CloseIsIdempotentAndSendAfterCloseIsRefused) {  // EC-C14, EC-C15, EC-A03
    establish();
    script.on_text = [&](Session& s, std::string_view) {
        s.close(CloseCode::Normal, "first");
        s.close(CloseCode::GoingAway, "second");  // ignored
        script.last_send = s.send_text("too late");
    };
    send_frame(Opcode::Text, "x");
    ASSERT_FALSE(script.last_send.has_value());
    EXPECT_EQ(script.last_send.error().code, ErrorCode::Closing);
    const auto out = output();
    ASSERT_EQ(out.frames.size(), 1U);  // exactly one Close frame, nothing after it
    EXPECT_EQ(out.frames[0].opcode, Opcode::Close);
}

TEST_F(ConnectionTest, OnCloseMayNotSendAndAClosedConnectionIsRetiredOnce) {  // EC-A03
    establish();
    script.on_close = [&](Session& s, const CloseInfo&) {
        script.last_send = s.send_text("after close");
        s.close(CloseCode::Normal, "again");
    };
    send_frame(Opcode::Close, close_payload(1000));
    ASSERT_FALSE(script.last_send.has_value());
    EXPECT_EQ(script.last_send.error().code, ErrorCode::Closing);
    EXPECT_EQ(host.retired.size(), 1U);
}

TEST_F(ConnectionTest, ClosingFromOnOpenSendsTheCloseRightAfterTheHandshake) {
    open();
    script.on_open = [](Session& s) { s.close(CloseCode::PolicyViolation, "not welcome"); };
    send_bytes(kRequest);
    EXPECT_EQ(script.opened, 1);
    const std::string all = transport->written_text();
    EXPECT_NE(all.find(std::string("\x88\x0d\x03\xf0", 4)), std::string::npos);
}

// ---- graceful shutdown --------------------------------------------------------------------------

TEST_F(ConnectionTest, ShutdownSends1001ToAnEstablishedConnection) {  // EC-C18
    establish();
    conn->begin_shutdown();
    const auto out = output();
    ASSERT_EQ(out.frames.size(), 1U);
    EXPECT_EQ(code_of(out.frames[0]), std::string("\x03\xe9", 2));
    conn->abort();  // the deadline passed
    EXPECT_TRUE(conn->finished());
    EXPECT_EQ(script.closed, 1);
}

TEST_F(ConnectionTest, ShutdownDropsAConnectionStillInTheHandshake) {
    open();
    conn->begin_shutdown();
    EXPECT_TRUE(conn->finished());
    EXPECT_TRUE(transport->written.empty());
}

}  // namespace

// SPDX-License-Identifier: Apache-2.0
// Connection behaviour with a fake transport and a fake host: no kernel, no real time.
#include "server/connection.hpp"

#include "server/handshake_decider.hpp"
#include "server/message_writer.hpp"
#include "server/pending_gate.hpp"
#include "support/fake_connection_env.hpp"
#include "support/identity_codecs.hpp"
#include "support/wire.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using wsrv::CloseCode;
using wsrv::CloseInfo;
using wsrv::Connection;
using wsrv::ErrorCode;
using wsrv::MessageWriter;
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
    std::function<void(Session&)> on_writable;
    std::optional<wsrv::MessageWriter> writer;  // outlives the connection on purpose in some tests
    int writable = 0;
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
    void on_writable(Session& session) override {
        ++script_.writable;
        if (script_.on_writable) {
            script_.on_writable(session);
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
    const wsrv::HandshakeNegotiator* active_negotiator = &negotiator;  // a fixture may swap it
    std::vector<std::byte> scratch = std::vector<std::byte>(16 * 1024);
    FakeTransport* transport = nullptr;
    std::unique_ptr<Connection> conn;

    void open(std::size_t scratch_size = 16 * 1024) {
        scratch.resize(scratch_size);
        auto owned = std::make_unique<FakeTransport>();
        transport = owned.get();
        conn = std::make_unique<Connection>(7,
                                            wsrv::Endpoint{},
                                            std::move(owned),
                                            wsrv::ConnectionContext{host,
                                                                    config,
                                                                    *active_negotiator,
                                                                    factory,
                                                                    wsrv::MutableByteView{scratch},
                                                                    nullptr});
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

// ---- a transport that buffers (TLS, ADR-0019 section 3)
// -------------------------------------------

TEST_F(ConnectionTest, OutputHeldByTheTransportArmsWriteInterestUntilItIsPushedOut) {
    establish();
    transport->hold_output(true);
    transport->set_write_capacity(0);  // the kernel buffer is full
    readable();  // any event: the connection settles and looks at the transport
    EXPECT_TRUE(wsrv::has(host.interest_now, PollEvents::Write));
    EXPECT_GT(transport->empty_writes, 0U);  // it tried to push the held bytes out

    transport->set_write_capacity(static_cast<std::size_t>(-1));
    conn->on_event(PollEvents::Write);
    EXPECT_FALSE(transport->wants_write());
    EXPECT_FALSE(wsrv::has(host.interest_now, PollEvents::Write));
    EXPECT_FALSE(conn->finished());
}

TEST_F(ConnectionTest, ATransportThatCannotWriteDuringTheHandshakeIsWokenWhenWritable) {
    open();
    transport->hold_output(true);  // a TLS handshake flight the socket refused
    transport->set_write_capacity(0);
    send_bytes("GET /chat");  // partial head: still handshaking
    EXPECT_TRUE(wsrv::has(host.interest_now, PollEvents::Write));

    transport->set_write_capacity(static_cast<std::size_t>(-1));
    conn->on_event(PollEvents::Write);
    EXPECT_FALSE(wsrv::has(host.interest_now, PollEvents::Write));
}

TEST_F(ConnectionTest, ATransportThatHoldsOutputForeverIsDroppedByTheStallTimer) {  // T3
    establish();
    transport->hold_output(true);
    transport->set_write_capacity(0);
    readable();
    host.advance(config.write_stall_timeout + 1s, 1s);
    EXPECT_TRUE(conn->finished());
}

/// Open with a scratch buffer so small that one read holds only part of the input.
void open_with_tiny_reads(ConnectionTest& test, std::size_t scratch_size) {
    test.open(scratch_size);
    test.transport->feed(kRequest);
    for (int guard = 0; guard < 400 && test.script.opened == 0; ++guard) {
        test.readable();
    }
    ASSERT_EQ(test.script.opened, 1);
    test.transport->written.clear();
}

TEST_F(ConnectionTest, BufferedInputIsReadAgainAfterTheReadBudgetEvenWithoutANewEvent) {
    open_with_tiny_reads(*this, 16);
    script.echo = true;
    config.read.max_reads_per_event = 1;
    for (int i = 0; i < 3; ++i) {
        const auto frame = client_frame(Opcode::Text, true, "buffered");
        transport->feed(wsrv::ByteView{frame});
    }
    transport->hold_input(
        true);  // the transport has taken the bytes: the kernel will not announce them
    readable();
    EXPECT_GE(host.pending_timers(), 1U);  // it asked the loop to call it back at once
    for (int guard = 0; guard < 10 && script.texts.size() < 3; ++guard) {
        host.advance(1ms, 1ms);
    }
    EXPECT_EQ(script.texts.size(), 3U);
}

TEST_F(ConnectionTest, APlainTransportNeverSchedulesARereadAtTheBudget) {
    open_with_tiny_reads(*this, 16);
    config.read.max_reads_per_event = 1;
    const std::size_t timers = host.pending_timers();
    for (int i = 0; i < 3; ++i) {
        const auto frame = client_frame(Opcode::Text, true, "plain");
        transport->feed(wsrv::ByteView{frame});
    }
    readable();
    EXPECT_EQ(host.pending_timers(), timers);  // the kernel's re-armed edge does the job
    EXPECT_EQ(script.texts.size(), 1U);
}

TEST_F(ConnectionTest, ResumingReadsReadsBufferedInputAtOnce) {
    establish();
    conn->pause_reads();
    const auto frame = client_frame(Opcode::Text, true, "held");
    transport->feed(wsrv::ByteView{frame});
    transport->hold_input(true);
    conn->resume_reads();
    host.advance(1ms, 1ms);
    ASSERT_EQ(script.texts.size(), 1U);
    EXPECT_EQ(script.texts[0], "held");
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

// ---- streaming writer (ADR-0015) ----------------------------------------------------------------

constexpr std::size_t kTinyQueue = 200;  // a queue that two 150-byte writes overflow

TEST_F(ConnectionTest, AWriterSendsOneMessageAsFragments) {  // FR-F12, EC-F37
    establish();
    script.on_text = [&](Session& s, std::string_view) {
        auto begun = s.begin_text();
        ASSERT_TRUE(begun.has_value());
        script.writer.emplace(std::move(*begun));
        ASSERT_TRUE(script.writer->write_text("hel").has_value());
        ASSERT_TRUE(script.writer->write_text("lo").has_value());
        ASSERT_TRUE(script.writer->finish().has_value());
    };
    send_frame(Opcode::Text, "go");
    std::string text;
    const auto frames = output().frames;
    ASSERT_FALSE(frames.empty());
    for (const auto& frame : frames) {
        text += frame.payload;
    }
    EXPECT_EQ(text, "hello");
    EXPECT_TRUE(frames.back().fin);
    EXPECT_EQ(frames.front().opcode, Opcode::Text);
    EXPECT_FALSE(script.writer->open());
}

TEST_F(ConnectionTest, AWriterKeptBetweenEventsContinuesInALaterCallback) {
    script.on_open = [&](Session& s) {
        script.writer.emplace(std::move(*s.begin_binary()));
        ASSERT_TRUE(script.writer->write(wsrv::as_bytes(std::string_view{"ab"})).has_value());
    };
    script.on_text = [&](Session&, std::string_view) {
        ASSERT_TRUE(script.writer->write(wsrv::as_bytes(std::string_view{"cd"})).has_value());
        ASSERT_TRUE(script.writer->finish().has_value());
    };
    open();
    send_bytes(kRequest);  // on_open begins the stream and writes "ab" behind the 101
    transport->written.clear();
    ASSERT_TRUE(script.writer->open());
    send_frame(Opcode::Text, "go");
    std::string rest;
    for (const auto& frame : output().frames) {
        rest += frame.payload;
    }
    EXPECT_EQ(rest, "cd");
    EXPECT_TRUE(output().frames.back().fin);
    EXPECT_FALSE(script.writer->open());
}

TEST_F(ConnectionTest, AWriterDestroyedWithoutFinishClosesWith1011) {  // EC-F34, T16
    establish();
    script.on_text = [&](Session& s, std::string_view) {
        auto begun = s.begin_text();
        ASSERT_TRUE(begun.has_value());
        script.writer.emplace(std::move(*begun));
        ASSERT_TRUE(script.writer->write_text("half").has_value());
        script.writer.reset();  // the handler forgot to finish
    };
    send_frame(Opcode::Text, "go");
    bool internal_error = false;
    for (const auto& frame : output().frames) {
        internal_error |=
            frame.opcode == Opcode::Close && code_of(frame) == std::string("\x03\xf3", 2);
    }
    EXPECT_TRUE(internal_error);
    EXPECT_TRUE(host.logged("unfinished"));
}

TEST_F(ConnectionTest, AHandlerThatThrowsMidStreamGetsExactlyOne1011) {  // EC-A01, EC-F34
    establish();
    script.on_text = [&](Session& s, std::string_view) {
        auto begun = s.begin_binary();
        ASSERT_TRUE(begun.has_value());
        MessageWriter local{std::move(*begun)};
        ASSERT_TRUE(local.write(wsrv::as_bytes(std::string_view{"x"})).has_value());
        throw std::runtime_error{"boom"};  // the writer is destroyed while unwinding
    };
    send_frame(Opcode::Text, "go");
    int closes = 0;
    for (const auto& frame : output().frames) {
        closes += frame.opcode == Opcode::Close ? 1 : 0;
    }
    EXPECT_EQ(closes, 1);
}

TEST_F(ConnectionTest,
       ABackpressuredWriteQueuesNothingAndDoesNotApplyTheSlowConsumerPolicy) {  // ADR-0015 5
    config.session.outbound.max_queued_bytes = kTinyQueue;
    establish();
    wsrv::Result<void> second;
    script.on_text = [&](Session& s, std::string_view) {
        script.writer.emplace(std::move(*s.begin_binary()));
        ASSERT_TRUE(script.writer->write(wsrv::as_bytes(std::string(150, 'a'))).has_value());
        second = script.writer->write(wsrv::as_bytes(std::string(150, 'b')));
    };
    send_frame(Opcode::Text, "go");
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code, ErrorCode::Backpressure);
    for (const auto& frame : output().frames) {
        EXPECT_NE(frame.opcode, Opcode::Close);  // cooperative: Backpressure is only a signal
    }
    EXPECT_FALSE(conn->finished());
    EXPECT_TRUE(script.writer->open());  // and the writer can still be used
}

TEST_F(ConnectionTest,
       OnWritableComesOnceOnTheNextWriteEventNotInTheSameCallback) {  // EC-F35, EC-N08
    config.session.outbound.max_queued_bytes = kTinyQueue;
    establish();
    script.on_text = [&](Session& s, std::string_view) {
        script.writer.emplace(std::move(*s.begin_binary()));
        static_cast<void>(script.writer->write(wsrv::as_bytes(std::string(150, 'a'))));
        static_cast<void>(script.writer->write(wsrv::as_bytes(std::string(150, 'b'))));
    };
    send_frame(Opcode::Text, "go");
    EXPECT_EQ(script.writable, 0);                                 // not inside the same event
    EXPECT_TRUE(wsrv::has(host.interest_now, PollEvents::Write));  // armed to get a next turn
    conn->on_event(PollEvents::Write);
    EXPECT_EQ(script.writable, 1);
    EXPECT_FALSE(wsrv::has(host.interest_now, PollEvents::Write));  // disarmed again
    conn->on_event(PollEvents::Write);
    EXPECT_EQ(script.writable, 1);  // owed once, delivered once
}

TEST_F(ConnectionTest, OnWritableIsNotCalledWhenNothingWasRefused) {
    establish();
    script.on_text = [&](Session& s, std::string_view) {
        script.writer.emplace(std::move(*s.begin_binary()));
        ASSERT_TRUE(script.writer->write(wsrv::as_bytes(std::string_view{"fine"})).has_value());
        ASSERT_TRUE(script.writer->finish().has_value());
    };
    send_frame(Opcode::Text, "go");
    conn->on_event(PollEvents::Write);
    EXPECT_EQ(script.writable, 0);
}

TEST_F(ConnectionTest, TheStreamResumesFromOnWritableUntilItIsFinished) {  // FR-F12
    config.session.outbound.max_queued_bytes = kTinyQueue;
    establish();
    std::size_t sent = 0;
    constexpr std::size_t kTotal = 1000;
    const std::string piece(100, 'z');
    const auto pump = [&](Session&) {
        while (sent < kTotal) {
            if (!script.writer->write(wsrv::as_bytes(piece)).has_value()) {
                return;  // wait for on_writable
            }
            sent += piece.size();
        }
        ASSERT_TRUE(script.writer->finish().has_value());
    };
    script.on_text = [&](Session& s, std::string_view) {
        script.writer.emplace(std::move(*s.begin_binary()));
        pump(s);
    };
    script.on_writable = pump;
    send_frame(Opcode::Text, "go");
    for (int turn = 0; turn < 50 && script.writer->open(); ++turn) {
        conn->on_event(PollEvents::Write);
    }
    EXPECT_EQ(sent, kTotal);
    EXPECT_FALSE(script.writer->open());
    std::size_t received = 0;
    const auto frames = output().frames;
    for (const auto& frame : frames) {
        received += frame.payload.size();
    }
    EXPECT_EQ(received, kTotal);
    EXPECT_TRUE(frames.back().fin);
}

TEST_F(ConnectionTest, OnWritableAlsoFollowsARefusedSendUnderTheDropPolicy) {  // ADR-0015 4
    config.session.outbound.max_queued_bytes = kTinyQueue;
    config.slow_consumer = wsrv::SlowConsumerPolicy::DropMessage;
    establish();
    script.on_text = [&](Session& s, std::string_view) {
        static_cast<void>(s.send_text(std::string(150, 'a')));
        script.last_send = s.send_text(std::string(150, 'b'));
    };
    send_frame(Opcode::Text, "go");
    ASSERT_FALSE(script.last_send.has_value());
    conn->on_event(PollEvents::Write);
    EXPECT_EQ(script.writable, 1);
}

TEST_F(ConnectionTest, AThrowingOnWritableClosesWith1011) {  // EC-A01
    config.session.outbound.max_queued_bytes = kTinyQueue;
    config.slow_consumer = wsrv::SlowConsumerPolicy::DropMessage;
    establish();
    script.on_text = [&](Session& s, std::string_view) {
        static_cast<void>(s.send_text(std::string(150, 'a')));
        static_cast<void>(s.send_text(std::string(150, 'b')));
    };
    script.on_writable = [](Session&) { throw std::runtime_error{"boom"}; };
    send_frame(Opcode::Text, "go");
    conn->on_event(PollEvents::Write);
    bool internal_error = false;
    for (const auto& frame : output().frames) {
        internal_error |=
            frame.opcode == Opcode::Close && code_of(frame) == std::string("\x03\xf3", 2);
    }
    EXPECT_TRUE(internal_error);
}

TEST_F(ConnectionTest, AStreamToAPeerThatReadsNothingIsDroppedByTheStallTimer) {  // T3, EC-N09
    config.session.outbound.max_queued_bytes = kTinyQueue;
    establish();
    script.on_text = [&](Session& s, std::string_view) {
        script.writer.emplace(std::move(*s.begin_binary()));
        static_cast<void>(script.writer->write(wsrv::as_bytes(std::string(150, 'a'))));
        static_cast<void>(script.writer->write(wsrv::as_bytes(std::string(150, 'b'))));
    };
    transport->set_write_capacity(0);
    send_frame(Opcode::Text, "go");
    host.advance(config.write_stall_timeout + 1s, 100ms);
    EXPECT_TRUE(conn->finished());
    EXPECT_EQ(script.closed, 1);
    EXPECT_EQ(script.writable, 0);  // a dead connection never calls back
}

TEST_F(ConnectionTest, AWriterOutlivingItsConnectionReportsClosingAndIsHarmless) {  // EC-N13
    establish();
    script.on_text = [&](Session& s, std::string_view) {
        script.writer.emplace(std::move(*s.begin_text()));
        ASSERT_TRUE(script.writer->write_text("a").has_value());
    };
    send_frame(Opcode::Text, "go");
    transport->feed_eof();
    readable();
    ASSERT_TRUE(conn->finished());
    const auto after = script.writer->write_text("b");
    ASSERT_FALSE(after.has_value());
    EXPECT_EQ(after.error().code, ErrorCode::Closing);
    EXPECT_EQ(script.writer->finish().error().code, ErrorCode::Closing);
    script.writer.reset();  // no 1011 attempt, no crash
    conn.reset();           // and the connection can go while a writer still exists
}

TEST_F(ConnectionTest, TheConnectionMayBeDestroyedBeforeItsWriter) {
    establish();
    script.on_text = [&](Session& s, std::string_view) {
        script.writer.emplace(std::move(*s.begin_text()));
    };
    send_frame(Opcode::Text, "go");
    conn.reset();  // the writer is still open, but its connection is gone
    EXPECT_EQ(script.writer->write_text("x").error().code, ErrorCode::Closing);
    script.writer.reset();
}

TEST_F(ConnectionTest, AMovedFromWriterDoesNotCloseTheConnection) {
    establish();
    script.on_text = [&](Session& s, std::string_view) {
        script.writer.emplace(std::move(*s.begin_binary()));
        {
            MessageWriter moved_from = std::move(*script.writer);
            script.writer.reset();
            script.writer.emplace(std::move(moved_from));
        }  // moved_from dies here, empty: nothing is closed
        ASSERT_TRUE(script.writer->open());
        ASSERT_TRUE(script.writer->finish().has_value());
    };
    send_frame(Opcode::Text, "go");
    for (const auto& frame : output().frames) {
        EXPECT_NE(frame.opcode, Opcode::Close);
    }
}

TEST_F(ConnectionTest, OtherSendsAndWritersWaitForTheOpenStream) {  // EC-F33
    establish();
    script.on_text = [&](Session& s, std::string_view) {
        script.writer.emplace(std::move(*s.begin_text()));
        EXPECT_EQ(s.send_text("x").error().code, ErrorCode::MessageInProgress);
        EXPECT_EQ(s.send_binary({}).error().code, ErrorCode::MessageInProgress);
        EXPECT_EQ(s.begin_binary().error().code, ErrorCode::MessageInProgress);
        ASSERT_TRUE(script.writer->finish().has_value());
        EXPECT_TRUE(s.send_text("now").has_value());
    };
    send_frame(Opcode::Text, "go");
    EXPECT_FALSE(conn->finished());
    for (const auto& frame : output().frames) {
        EXPECT_NE(frame.opcode, Opcode::Close);  // a refused send is not a slow consumer
    }
}

TEST_F(ConnectionTest, ATextStreamMayNotEndInsideACharacter) {  // EC-F36, RFC 6455 section 8.1
    establish();
    script.on_text = [&](Session& s, std::string_view) {
        script.writer.emplace(std::move(*s.begin_text()));
        ASSERT_TRUE(script.writer->write(wsrv::as_bytes(std::string_view{"\xE2\x82"})).has_value());
        EXPECT_EQ(script.writer->finish().error().code, ErrorCode::InvalidUtf8);
        EXPECT_TRUE(script.writer->open());
        EXPECT_EQ(script.writer->write(wsrv::as_bytes(std::string_view{"\xFF"})).error().code,
                  ErrorCode::InvalidUtf8);
        ASSERT_TRUE(script.writer->write(wsrv::as_bytes(std::string_view{"\xAC"})).has_value());
        ASSERT_TRUE(script.writer->finish().has_value());
    };
    send_frame(Opcode::Text, "go");
    std::string text;
    for (const auto& frame : output().frames) {
        text += frame.payload;
    }
    EXPECT_EQ(text, "\xE2\x82\xAC");
}

TEST_F(ConnectionTest, FinishingTwiceIsAnArgumentError) {
    establish();
    script.on_text = [&](Session& s, std::string_view) {
        script.writer.emplace(std::move(*s.begin_binary()));
        ASSERT_TRUE(script.writer->finish().has_value());
        EXPECT_EQ(script.writer->finish().error().code, ErrorCode::InvalidArgument);
        EXPECT_EQ(script.writer->write(wsrv::as_bytes(std::string_view{"x"})).error().code,
                  ErrorCode::InvalidArgument);
    };
    send_frame(Opcode::Text, "go");
}

TEST_F(ConnectionTest, ThePeersCloseEndsAnOpenStreamWithoutBlamingTheHandler) {  // EC-C14, EC-F30
    establish();
    script.on_text = [&](Session& s, std::string_view) {
        script.writer.emplace(std::move(*s.begin_binary()));
        ASSERT_TRUE(script.writer->write(wsrv::as_bytes(std::string_view{"x"})).has_value());
    };
    send_frame(Opcode::Text, "go");
    send_frame(Opcode::Close, wsrv::test::close_payload(1000));
    EXPECT_EQ(script.writer->write(wsrv::as_bytes(std::string_view{"y"})).error().code,
              ErrorCode::Closing);
    script.writer.reset();
    EXPECT_FALSE(host.logged("unfinished"));
}

TEST_F(ConnectionTest, NoWriterCanBeBegunOnceClosingStarted) {  // EC-C14
    establish();
    script.on_text = [&](Session& s, std::string_view) {
        s.close(CloseCode::Normal, "");
        EXPECT_EQ(s.begin_text().error().code, ErrorCode::Closing);
        EXPECT_EQ(s.begin_binary().error().code, ErrorCode::Closing);
    };
    send_frame(Opcode::Text, "go");
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

// ---- read pause (ADR-0016 section 4)
// --------------------------------------------------------------

TEST_F(ConnectionTest, PausingReadsDropsReadInterestAtOnce) {
    establish();
    script.on_text = [&](Session& s, std::string_view) { s.pause_reads(); };
    send_frame(Opcode::Text, "a");
    EXPECT_FALSE(wsrv::has(host.interest_now, PollEvents::Read));
}

TEST_F(ConnectionTest, NothingIsReadWhilePausedEvenIfAStaleReadEventArrives) {
    establish();
    script.on_text = [&](Session& s, std::string_view) { s.pause_reads(); };
    send_frame(Opcode::Text, "a");
    send_frame(Opcode::Text, "b");
    send_frame(Opcode::Text, "c");
    EXPECT_EQ(script.texts, std::vector<std::string>{"a"});
    EXPECT_GT(transport->unread(), 0U);  // the bytes wait in the kernel, not in our memory
}

/// A connection whose reads are 7 bytes long: exactly one masked one-byte text frame per read.
void ConnectionTest_establish_with_tiny_reads(ConnectionTest& t) {
    t.open(7);
    t.transport->feed(kRequest);
    for (int i = 0; i < 40 && t.transport->written_text().find("\r\n\r\n") == std::string::npos;
         ++i) {
        t.readable();
    }
    ASSERT_EQ(t.transport->written_text().rfind(kResponseStart, 0), 0U);
    t.transport->written.clear();
}

TEST_F(ConnectionTest, APauseInTheMiddleOfAReadBatchStopsTheBatch) {
    ConnectionTest_establish_with_tiny_reads(*this);
    script.on_text = [&](Session& s, std::string_view) { s.pause_reads(); };
    for (const char* text : {"a", "b", "c"}) {
        transport->feed(wsrv::ByteView{client_frame(Opcode::Text, true, text)});
    }
    readable();
    EXPECT_EQ(script.texts, std::vector<std::string>{"a"});
}

TEST_F(ConnectionTest, ResumingRestoresReadInterestAndRearmsTheEdge) {
    ConnectionTest_establish_with_tiny_reads(*this);
    script.on_text = [&](Session& s, std::string_view text) {
        if (text == "a") {
            s.pause_reads();
        }
    };
    for (const char* text : {"a", "b", "c"}) {
        transport->feed(wsrv::ByteView{client_frame(Opcode::Text, true, text)});
    }
    readable();
    ASSERT_EQ(script.texts.size(), 1U);
    const std::size_t before = host.rewatch_calls;

    script.on_text = nullptr;
    static_cast<Session&>(*conn).resume_reads();
    EXPECT_GT(host.rewatch_calls, before);  // a fresh registration: the kernel reports waiting data
    EXPECT_TRUE(wsrv::has(host.interest_now, PollEvents::Read));
    readable();  // what the loop delivers after the re-arm
    EXPECT_EQ(script.texts, (std::vector<std::string>{"a", "b", "c"}));
}

TEST_F(ConnectionTest, PausingTwiceAndResumingWithoutAPauseAreHarmless) {
    establish();
    script.on_text = [&](Session& s, std::string_view) {
        s.pause_reads();
        s.pause_reads();
    };
    send_frame(Opcode::Text, "a");
    const std::size_t paused_calls = host.rewatch_calls;
    static_cast<Session&>(*conn).resume_reads();
    static_cast<Session&>(*conn).resume_reads();
    EXPECT_EQ(host.rewatch_calls, paused_calls + 1);  // only the first resume changed anything
}

TEST_F(ConnectionTest, APausedConnectionStillWatchesWritabilityWhileOutputIsBlocked) {
    establish();
    transport->set_write_capacity(0);
    script.on_text = [&](Session& s, std::string_view text) {
        s.pause_reads();
        static_cast<void>(s.send_text(text).has_value());
    };
    send_frame(Opcode::Text, "a");
    EXPECT_FALSE(wsrv::has(host.interest_now, PollEvents::Read));
    EXPECT_TRUE(wsrv::has(host.interest_now, PollEvents::Write));
}

TEST_F(ConnectionTest, APeerThatHangsUpWhilePausedIsStillSeenAndTheConnectionEnds) {
    establish();
    script.on_text = [&](Session& s, std::string_view) { s.pause_reads(); };
    send_frame(Opcode::Text, "a");
    transport->feed_eof();
    conn->on_event(PollEvents::Hangup);  // epoll reports a hang-up whatever the interest mask
    EXPECT_TRUE(conn->finished());
    EXPECT_EQ(script.closed, 1);
}

// ---- the asynchronous handshake decision (ADR-0018)
// -----------------------------------------------

/// Records what the server asked and keeps the handles; can be told to refuse the request.
class FakeDecider final : public wsrv::IHandshakeDecider {
public:
    wsrv::Result<void> request(wsrv::HandshakeRequest request,
                               wsrv::PendingHandle handle) override {
        if (refuse) {
            return wsrv::fail(ErrorCode::BufferFull, "fake decider: queue full");
        }
        requests.push_back(std::move(request));
        handles.push_back(std::move(handle));
        return {};
    }
    void withdrawn(wsrv::ConnectionId id) noexcept override {
        withdrawals.push_back(id);
    }
    std::vector<wsrv::HandshakeRequest> requests;
    std::vector<wsrv::PendingHandle> handles;
    std::vector<wsrv::ConnectionId> withdrawals;
    bool refuse = false;
};

wsrv::HandshakeVerdict accept_verdict(std::optional<std::string> subprotocol = std::nullopt) {
    wsrv::HandshakeVerdict verdict;
    verdict.accept = true;
    verdict.subprotocol = std::move(subprotocol);
    return verdict;
}

wsrv::HandshakeVerdict reject_verdict(int status) {
    wsrv::HandshakeVerdict verdict;
    verdict.accept = false;
    verdict.status = status;
    return verdict;
}

wsrv::HandshakeConfig hook_handshake_config() {
    wsrv::HandshakeConfig config = handshake_config();
    config.subprotocols = {"chat", "v2"};
    return config;
}

struct HookTest : public ConnectionTest {
    FakeDecider decider;
    wsrv::PendingGate gate{4};
    wsrv::HandshakeNegotiator hook_negotiator{hook_handshake_config()};
    const wsrv::HandshakeNegotiator* hook_active = &hook_negotiator;  // a fixture may swap it
    std::string request_text{kRequest};

    /// The decider outlives every connection, as in the server (a connection tells it when it
    /// goes): release ours before the members below are destroyed.
    ~HookTest() override {
        conn.reset();
    }

    /// A connection with the hook on. `which_gate` lets a test share one gate between connections.
    std::unique_ptr<Connection> make(FakeTransport*& out_transport, wsrv::PendingGate* which_gate) {
        auto owned = std::make_unique<FakeTransport>();
        out_transport = owned.get();
        wsrv::ConnectionContext context{
            host, config, *hook_active, factory, wsrv::MutableByteView{scratch}, nullptr};
        context.decider = &decider;
        context.pending = which_gate;
        auto connection =
            std::make_unique<Connection>(static_cast<wsrv::ConnectionId>(host.retired.size() + 20),
                                         wsrv::Endpoint{},
                                         std::move(owned),
                                         context);
        EXPECT_TRUE(connection->start().has_value());
        return connection;
    }
    void open_hook() {
        conn = make(transport, &gate);
    }
    void send_request() {
        send_bytes(request_text);
    }
    void decide(const wsrv::HandshakeVerdict& verdict) {
        conn->decide(verdict);
    }
};

TEST_F(HookTest, AValidUpgradeWaitsForTheApplicationAndWritesNothingYet) {  // EC-H27
    open_hook();
    send_request();
    ASSERT_EQ(decider.requests.size(), 1U);
    EXPECT_EQ(decider.requests[0].path, "/chat");
    EXPECT_EQ(decider.requests[0].origin, "https://app.example.com");
    EXPECT_TRUE(transport->written.empty()) << "no 101 before the application agrees";
    EXPECT_EQ(script.created, 0);
    EXPECT_EQ(script.opened, 0);
    EXPECT_EQ(gate.waiting(), 1U);
    EXPECT_FALSE(conn->finished());
    EXPECT_FALSE(wsrv::has(host.interest_now, PollEvents::Read)) << "reading is off while waiting";
    EXPECT_TRUE(wsrv::has(host.interest_now, PollEvents::Hangup)) << "a leaving client is noticed";
}

TEST_F(HookTest, AcceptWritesThe101OpensTheHandlerAndFreesTheSlot) {
    open_hook();
    send_request();
    decide(accept_verdict());
    EXPECT_EQ(transport->written_text().rfind(kResponseStart, 0), 0U);
    EXPECT_EQ(script.opened, 1);
    EXPECT_EQ(gate.waiting(), 0U);
    EXPECT_TRUE(wsrv::has(host.interest_now, PollEvents::Read)) << "the edge is re-armed";
    EXPECT_EQ(host.pending_timers(), 1U) << "only the tick timer is left, not the decision timer";
}

TEST_F(HookTest, FrameBytesBehindTheHeadWaitAndArriveInOrderAfterTheAccept) {  // EC-H28
    open_hook();
    const auto first = client_frame(Opcode::Text, true, "one");
    send_bytes(request_text + std::string{wsrv::as_chars(wsrv::ByteView{first})});
    EXPECT_EQ(script.texts.size(), 0U) << "nothing is delivered before the decision";
    const auto second = client_frame(Opcode::Text, true, "two");
    transport->feed(wsrv::ByteView{second});
    conn->on_event(PollEvents::Read);  // a stray event: the bytes must stay in the kernel
    EXPECT_EQ(transport->unread(), second.size());
    decide(accept_verdict());
    EXPECT_EQ(script.texts, std::vector<std::string>{"one"});
    readable();
    EXPECT_EQ(script.texts, (std::vector<std::string>{"one", "two"}));
}

TEST_F(HookTest, ReadingStopsMidLoopWhenTheHeadCompletesAndNoLaterByteIsLost) {  // EC-H28
    scratch.resize(128);  // the head and the frames behind it arrive over several reads
    open_hook();
    const auto first = client_frame(Opcode::Text, true, "one");
    const auto second = client_frame(Opcode::Text, true, std::string(300, 'x'));
    send_bytes(request_text + std::string{wsrv::as_chars(wsrv::ByteView{first})} +
               std::string{wsrv::as_chars(wsrv::ByteView{second})});
    EXPECT_GT(transport->unread(), 0U) << "the loop must not read on after the head is complete";
    decide(accept_verdict());
    readable();
    ASSERT_EQ(script.texts.size(), 2U) << "nothing may be read and thrown away while waiting";
    EXPECT_EQ(script.texts[0], "one");
    EXPECT_EQ(script.texts[1], std::string(300, 'x'));
}

TEST_F(HookTest, TheApplicationMayChooseAnotherOfferedSubprotocol) {
    request_text.insert(request_text.find("\r\n\r\n") + 2, "Sec-WebSocket-Protocol: chat, v2\r\n");
    open_hook();
    send_request();
    ASSERT_EQ(decider.requests.size(), 1U);
    EXPECT_EQ(decider.requests[0].subprotocol, "chat") << "the server's own preference";
    EXPECT_EQ(decider.requests[0].offered_subprotocols, (std::vector<std::string>{"chat", "v2"}));
    decide(accept_verdict("v2"));
    const std::string response = transport->written_text();
    EXPECT_NE(response.find("Sec-WebSocket-Protocol: v2\r\n"), std::string::npos);
    EXPECT_EQ(response.find("Sec-WebSocket-Protocol: chat"), std::string::npos);
    EXPECT_NE(response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo="),
              std::string::npos);
}

TEST_F(HookTest, ARefusalIsAnsweredWithItsStatusThenTheConnectionCloses) {
    open_hook();
    send_request();
    decide(reject_verdict(401));
    const std::string response = transport->written_text();
    EXPECT_EQ(response.rfind("HTTP/1.1 401 ", 0), 0U);
    EXPECT_NE(response.find("Connection: close"), std::string::npos);
    EXPECT_EQ(response.find("Retry-After"), std::string::npos) << "only 429 and 503 carry it";
    EXPECT_TRUE(conn->finished());
    EXPECT_EQ(script.created, 0) << "the application's handler never existed";
    EXPECT_EQ(gate.waiting(), 0U);
}

TEST_F(HookTest, A429And503CarryRetryAfter) {
    open_hook();
    send_request();
    decide(reject_verdict(429));
    EXPECT_NE(transport->written_text().find("Retry-After: 5\r\n"), std::string::npos);
}

TEST_F(HookTest, NoDecisionInTimeFailsClosedWith503) {  // EC-H29
    config.handshake_decision_timeout = 500ms;
    open_hook();
    send_request();
    host.advance(400ms, 50ms);
    EXPECT_FALSE(conn->finished());
    host.advance(200ms, 50ms);
    EXPECT_TRUE(conn->finished());
    const std::string response = transport->written_text();
    EXPECT_EQ(response.rfind("HTTP/1.1 503 ", 0), 0U);
    EXPECT_NE(response.find("Retry-After: 5\r\n"), std::string::npos);
    EXPECT_EQ(script.created, 0);
    EXPECT_EQ(script.opened, 0);
    EXPECT_EQ(script.closed, 0) << "on_close is owed only after on_open";
    EXPECT_EQ(gate.waiting(), 0U);
}

TEST_F(HookTest, ALateOrRepeatedDecisionChangesNothing) {  // EC-H30
    open_hook();
    send_request();
    decide(accept_verdict());
    const std::string written = transport->written_text();
    decide(reject_verdict(403));  // after the accept
    decide(accept_verdict());     // twice
    EXPECT_EQ(transport->written_text(), written);
    EXPECT_EQ(script.opened, 1);
    EXPECT_FALSE(conn->finished());

    FakeTransport* second_transport = nullptr;
    auto timed_out = make(second_transport, &gate);
    second_transport->feed(request_text);
    timed_out->on_event(PollEvents::Read);
    host.advance(config.handshake_decision_timeout + 100ms, 100ms);
    ASSERT_TRUE(timed_out->finished());
    const std::string refusal = second_transport->written_text();
    timed_out->decide(accept_verdict());  // after the timeout
    EXPECT_EQ(second_transport->written_text(), refusal);
}

TEST_F(HookTest, APeerThatLeavesWhileWaitingEndsQuietlyAndFreesTheSlot) {  // EC-H33, EC-H21
    open_hook();
    send_request();
    conn->on_event(PollEvents::Hangup);
    EXPECT_TRUE(conn->finished());
    EXPECT_TRUE(transport->written.empty());
    EXPECT_EQ(script.created, 0);
    EXPECT_EQ(script.closed, 0);
    EXPECT_EQ(gate.waiting(), 0U);
    EXPECT_EQ(host.pending_timers(), 0U);
    decide(accept_verdict());  // the application answers a connection that is gone
    EXPECT_TRUE(transport->written.empty());
}

TEST_F(HookTest, ShutdownWhileWaitingAnswers503) {  // EC-H32
    open_hook();
    send_request();
    conn->begin_shutdown();
    EXPECT_EQ(transport->written_text().rfind("HTTP/1.1 503 ", 0), 0U);
    EXPECT_TRUE(conn->finished());
    EXPECT_EQ(gate.waiting(), 0U);
}

TEST_F(HookTest, AbortAndDestructionWhileWaitingFreeTheSlotExactlyOnce) {  // EC-H32
    open_hook();
    send_request();
    EXPECT_EQ(gate.waiting(), 1U);
    conn->abort();
    EXPECT_TRUE(conn->finished());
    EXPECT_EQ(gate.waiting(), 0U);
    conn.reset();
    EXPECT_EQ(gate.waiting(), 0U);

    open_hook();
    send_request();
    EXPECT_EQ(gate.waiting(), 1U);
    conn.reset();  // a server torn down with the client still waiting
    EXPECT_EQ(gate.waiting(), 0U);
}

TEST_F(HookTest, AnUpgradeOverTheBoundIsRefusedAtOnceWithoutAskingTheApplication) {  // EC-H31
    wsrv::PendingGate one{1};
    FakeTransport* first_transport = nullptr;
    FakeTransport* second_transport = nullptr;
    auto first = make(first_transport, &one);
    auto second = make(second_transport, &one);
    first_transport->feed(request_text);
    first->on_event(PollEvents::Read);
    second_transport->feed(request_text);
    second->on_event(PollEvents::Read);
    EXPECT_EQ(decider.requests.size(), 1U) << "only the first was asked";
    EXPECT_FALSE(first->finished());
    EXPECT_TRUE(second->finished());
    EXPECT_EQ(second_transport->written_text().rfind("HTTP/1.1 503 ", 0), 0U);
    first->decide(accept_verdict());
    EXPECT_EQ(one.waiting(), 0U);
}

TEST_F(HookTest, ADeciderThatCannotTakeTheRequestMeansRefusalNeverAcceptance) {  // fail closed
    decider.refuse = true;
    open_hook();
    send_request();
    EXPECT_TRUE(conn->finished());
    EXPECT_EQ(transport->written_text().rfind("HTTP/1.1 503 ", 0), 0U);
    EXPECT_EQ(script.opened, 0);
    EXPECT_EQ(gate.waiting(), 0U);
}

TEST_F(HookTest, AHookWithoutAGateFailsClosed) {
    FakeTransport* t = nullptr;
    auto connection = make(t, nullptr);
    t->feed(request_text);
    connection->on_event(PollEvents::Read);
    EXPECT_TRUE(connection->finished());
    EXPECT_EQ(t->written_text().rfind("HTTP/1.1 503 ", 0), 0U);
    EXPECT_TRUE(decider.requests.empty());
}

TEST_F(HookTest, AForwardedHeaderOverTheCapIsRefusedWith431) {  // EC-H34
    config.handshake_forward_headers = {"Cookie"};
    request_text.insert(request_text.find("\r\n\r\n") + 2,
                        "Cookie: " + std::string(wsrv::kMaxForwardedValueBytes + 1, 'a') + "\r\n");
    config.http.max_total_bytes = 16 * 1024;
    open_hook();
    send_request();
    EXPECT_TRUE(conn->finished());
    EXPECT_EQ(transport->written_text().rfind("HTTP/1.1 431 ", 0), 0U);
    EXPECT_TRUE(decider.requests.empty());
    EXPECT_EQ(gate.waiting(), 0U);
}

TEST_F(HookTest, TheDeciderIsToldWhenTheServerEndsTheWaitWithoutAnAnswer) {  // EC-H29/H32/H33
    // timeout
    config.handshake_decision_timeout = 300ms;
    open_hook();
    send_request();
    host.advance(400ms, 50ms);
    EXPECT_EQ(decider.withdrawals, std::vector<wsrv::ConnectionId>{conn->id()});
    // peer gone
    decider.withdrawals.clear();
    open_hook();
    send_request();
    conn->on_event(PollEvents::Hangup);
    EXPECT_EQ(decider.withdrawals.size(), 1U);
    // shutdown
    decider.withdrawals.clear();
    open_hook();
    send_request();
    conn->begin_shutdown();
    EXPECT_EQ(decider.withdrawals.size(), 1U);
    // abort, and destruction
    decider.withdrawals.clear();
    open_hook();
    send_request();
    conn->abort();
    conn.reset();
    EXPECT_EQ(decider.withdrawals.size(), 1U) << "exactly once, not once per exit path";
    decider.withdrawals.clear();
    open_hook();
    send_request();
    conn.reset();
    EXPECT_EQ(decider.withdrawals.size(), 1U);
}

TEST_F(HookTest, ADeciderThatAnsweredIsNeverToldAboutAWithdrawal) {
    open_hook();
    send_request();
    decide(accept_verdict());
    conn->abort();
    open_hook();
    send_request();
    decide(reject_verdict(403));
    EXPECT_TRUE(decider.withdrawals.empty()) << "it answered; there is nothing to forget";
}

TEST_F(HookTest, ADeciderThatRefusedTheRequestIsNotToldAboutAWithdrawalEither) {
    decider.refuse = true;
    open_hook();
    send_request();
    EXPECT_TRUE(decider.withdrawals.empty()) << "it never took the request";
}

TEST_F(HookTest, ARefusedHandshakeNeverReachesTheApplication) {
    request_text.replace(request_text.find("Version: 13"), 11, "Version: 12");
    open_hook();
    send_request();
    EXPECT_TRUE(decider.requests.empty());
    EXPECT_EQ(transport->written_text().rfind("HTTP/1.1 426 ", 0), 0U);
    EXPECT_EQ(gate.waiting(), 0U);
}

TEST_F(HookTest, WithoutADeciderNothingChanges) {  // the hook is opt-in
    open();                                        // the plain fixture: no decider, no gate
    send_bytes(kRequest);
    EXPECT_EQ(transport->written_text().rfind(kResponseStart, 0), 0U);
    EXPECT_EQ(script.opened, 1);
}

// ---- permessage-deflate (ADR-0020) --------------------------------------------------------------
// The wiring only: what the handshake agreed reaches the session, and RSV1 is legal exactly on
// connections that agreed. The codecs copy their input; real compression is tested elsewhere.

wsrv::HandshakeConfig deflate_handshake_config() {
    wsrv::HandshakeConfig config = hook_handshake_config();
    config.permessage_deflate = wsrv::DeflatePolicy{};
    return config;
}

/// `request` with one more header line before the blank line.
std::string with_header(std::string_view request, std::string_view header) {
    std::string text{request};
    text.insert(text.find("\r\n\r\n") + 2, header);
    return text;
}

constexpr std::string_view kDeflateOffer = "Sec-WebSocket-Extensions: permessage-deflate\r\n";

std::vector<std::byte> compressed_text_frame(std::string_view payload) {
    return client_frame(Opcode::Text, true, payload, true);  // RSV1 set
}

struct DeflateTest : public ConnectionTest {
    std::shared_ptr<wsrv::test::IdentityCodecFactory> codecs =
        std::make_shared<wsrv::test::IdentityCodecFactory>();
    wsrv::HandshakeNegotiator deflate_negotiator{deflate_handshake_config()};

    DeflateTest() {
        active_negotiator = &deflate_negotiator;
        wsrv::SessionDeflate deflate;
        deflate.codecs = codecs;
        config.session.deflate = deflate;
    }
    ~DeflateTest() override {
        conn.reset();  // before the negotiator it points at
    }

    /// Open the connection and send the upgrade request with `extra` header lines.
    void upgrade(std::string_view extra) {
        open();
        send_bytes(with_header(kRequest, extra));
        ASSERT_EQ(transport->written_text().rfind(kResponseStart, 0), 0U);
    }
    void send_compressed(std::string_view payload) {
        const auto frame = compressed_text_frame(payload);
        transport->feed(wsrv::ByteView{frame});
        readable();
    }
};

TEST_F(DeflateTest, AnAgreedConnectionAcceptsRsv1AndBuildsItsCodecsFromTheAgreement) {
    upgrade("Sec-WebSocket-Extensions: permessage-deflate; server_max_window_bits=10; "
            "client_max_window_bits=9\r\n");
    EXPECT_NE(transport->written_text().find("Sec-WebSocket-Extensions: permessage-deflate"),
              std::string::npos);
    send_compressed("hello");
    EXPECT_EQ(script.texts, std::vector<std::string>{"hello"});
    ASSERT_TRUE(codecs->last_params.has_value());
    EXPECT_EQ(codecs->last_params->server_window_bits, 10);
    EXPECT_EQ(codecs->last_params->client_window_bits, 9);
    EXPECT_TRUE(codecs->last_params->server_no_context_takeover);  // the policy default
}

TEST_F(DeflateTest, AClientThatDidNotNegotiateMayNotSendRsv1) {  // RFC 6455 section 5.2, EC-F24
    upgrade("");  // the server is ready for compression, this client offered none
    EXPECT_EQ(transport->written_text().find("Sec-WebSocket-Extensions"), std::string::npos);
    transport->written.clear();
    send_compressed("hello");
    EXPECT_TRUE(script.texts.empty());
    EXPECT_EQ(codecs->created, 0) << "no codecs are built for a connection that did not agree";
    const auto out = output();
    ASSERT_FALSE(out.frames.empty());
    EXPECT_EQ(out.frames[0].opcode, Opcode::Close);
    EXPECT_EQ(code_of(out.frames[0]), std::string("\x03\xea", 2));  // 1002
}

TEST_F(DeflateTest, EachConnectionGetsItsOwnAgreementNotTheServerDefault) {
    upgrade("Sec-WebSocket-Extensions: permessage-deflate; server_max_window_bits=9\r\n");
    send_compressed("one");
    ASSERT_TRUE(codecs->last_params.has_value());
    EXPECT_EQ(codecs->last_params->server_window_bits, 9);
    // The server-wide SessionConfig stays as configured: the agreement is per connection.
    ASSERT_TRUE(config.session.deflate.has_value());
    EXPECT_EQ(config.session.deflate->params, wsrv::DeflateParams{});
}

struct DeflateHookTest : public HookTest {
    std::shared_ptr<wsrv::test::IdentityCodecFactory> codecs =
        std::make_shared<wsrv::test::IdentityCodecFactory>();
    wsrv::HandshakeNegotiator deflate_negotiator{deflate_handshake_config()};

    DeflateHookTest() {
        hook_active = &deflate_negotiator;
        wsrv::SessionDeflate deflate;
        deflate.codecs = codecs;
        config.session.deflate = deflate;
    }
    ~DeflateHookTest() override {
        conn.reset();  // before the negotiator it points at
    }
};

// The 101 is rendered again when the application picks another subprotocol (ADR-0018). Losing the
// extension there would leave the client without the header while the session expects RSV1.
TEST_F(DeflateHookTest, ADifferentSubprotocolKeepsTheExtensionInThe101AndTheSessionCompresses) {
    request_text = with_header(request_text, "Sec-WebSocket-Protocol: chat, v2\r\n");
    request_text = with_header(request_text, kDeflateOffer);
    open_hook();
    send_request();
    decide(accept_verdict("v2"));
    const std::string response = transport->written_text();
    EXPECT_NE(response.find("Sec-WebSocket-Protocol: v2\r\n"), std::string::npos);
    EXPECT_NE(response.find("Sec-WebSocket-Extensions: permessage-deflate"), std::string::npos);
    const auto frame = compressed_text_frame("hi");
    transport->feed(wsrv::ByteView{frame});
    readable();
    EXPECT_EQ(script.texts, std::vector<std::string>{"hi"});
}

}  // namespace

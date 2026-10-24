// SPDX-License-Identifier: Apache-2.0
#include "protocol/session.hpp"

#include "support/wire.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using wsrv::CloseCode;
using wsrv::ErrorCode;
using wsrv::MessageType;
using wsrv::Opcode;
using wsrv::ProtocolSession;
using wsrv::SessionConfig;
using wsrv::SessionEvent;
using wsrv::SessionState;
using wsrv::test::client_frame;
using wsrv::test::close_payload;
using wsrv::test::oversized_control_frame;
using wsrv::test::parse_server_output;
using wsrv::test::ParsedOutput;
using wsrv::test::WireFrame;
using Kind = SessionEvent::Kind;

/// A SessionEvent with its views copied out, so tests can keep and compare it.
struct Seen {
    Kind kind;
    MessageType type = MessageType::Text;
    std::string data;
    bool final = false;
    unsigned code = 0;
    std::string reason;
    bool clean = false;

    friend bool operator==(const Seen&, const Seen&) = default;
};

Seen message(std::string data, MessageType type = MessageType::Text) {
    return Seen{Kind::Message, type, std::move(data), true, 0, {}, false};
}
Seen ping(std::string data) {
    return Seen{Kind::Ping, MessageType::Text, std::move(data), false, 0, {}, false};
}
Seen pong(std::string data) {
    return Seen{Kind::Pong, MessageType::Text, std::move(data), false, 0, {}, false};
}
Seen closed(unsigned code, std::string reason, bool clean) {
    return Seen{Kind::Closed, MessageType::Text, {}, false, code, std::move(reason), clean};
}

std::vector<std::byte> cat(std::initializer_list<std::vector<std::byte>> parts) {
    std::vector<std::byte> out;
    for (const auto& p : parts) {
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

/// A scripted peer around one session: feed client bytes in, read server bytes and events out.
class Rig {
public:
    explicit Rig(SessionConfig config = {}) : session_{config, start_} {}

    ProtocolSession& session() {
        return session_;
    }

    /// Feed bytes (in chunks of `chunk`, 0 = all at once) and collect every event.
    std::vector<Seen> feed(std::vector<std::byte> bytes, std::size_t chunk = 0) {
        std::vector<Seen> seen;
        const std::size_t step = chunk == 0 ? std::max<std::size_t>(bytes.size(), 1) : chunk;
        for (std::size_t pos = 0; pos < bytes.size(); pos += step) {
            const std::size_t take = std::min(step, bytes.size() - pos);
            std::vector<std::byte> buffer{bytes.begin() + static_cast<std::ptrdiff_t>(pos),
                                          bytes.begin() + static_cast<std::ptrdiff_t>(pos + take)};
            wsrv::MutableByteView input{buffer};
            drain_events(input, seen);
            EXPECT_TRUE(input.empty()) << "the session left input unread";
        }
        return seen;
    }

    /// Ask for events without new input (timers, flushed Close, EOF).
    std::vector<Seen> poll() {
        std::vector<Seen> seen;
        wsrv::MutableByteView none;
        drain_events(none, seen);
        return seen;
    }

    /// Write out everything the session has pending (as the transport would) and parse it.
    ParsedOutput take_output() {
        std::vector<std::byte> bytes;
        for (;;) {
            const wsrv::ByteView view = session_.pending_output();
            if (view.empty()) {
                break;
            }
            bytes.insert(bytes.end(), view.begin(), view.end());
            session_.consume_output(view.size());
        }
        return parse_server_output(bytes);
    }

    void tick(std::chrono::milliseconds elapsed) {
        now_ += elapsed;
        session_.tick(now_);
    }

private:
    void drain_events(wsrv::MutableByteView& input, std::vector<Seen>& seen) {
        while (const auto event = session_.next_event(input)) {
            seen.push_back(Seen{event->kind,
                                event->type,
                                std::string{wsrv::as_chars(event->data)} +
                                    std::string{wsrv::as_chars(event->tail)},
                                event->final,
                                static_cast<unsigned>(event->code),
                                std::string{event->reason},
                                event->clean});
        }
    }

    wsrv::TimePoint start_;
    wsrv::TimePoint now_ = start_;
    ProtocolSession session_;
};

SessionConfig streaming_config() {
    SessionConfig c;
    c.assembler.streaming = true;
    return c;
}

// ---- Ping / Pong ------------------------------------------------------------------------------

TEST(Session, AMessageArrivesAsAnEventAndNothingIsSent) {
    Rig rig;
    EXPECT_EQ(rig.feed(client_frame(Opcode::Text, true, "hello")),
              (std::vector<Seen>{message("hello")}));
    EXPECT_TRUE(rig.take_output().frames.empty());
    EXPECT_EQ(rig.session().state(), SessionState::Open);
}

TEST(Session, APingIsAnsweredWithAnIdenticalPong) {  // EC-C01, FR-C1
    Rig rig;
    EXPECT_EQ(rig.feed(client_frame(Opcode::Ping, true, "hi")), (std::vector<Seen>{ping("hi")}));
    const ParsedOutput out = rig.take_output();
    ASSERT_TRUE(out.ok);
    EXPECT_EQ(out.frames, (std::vector<WireFrame>{{Opcode::Pong, true, "hi"}}));
}

TEST(Session, APingBetweenFragmentsWithASplitCharacterIsAnsweredAndTheMessageCompletes) {
    Rig rig;  // EC-C01, EC-F25
    const auto events = rig.feed(cat({client_frame(Opcode::Text, false, "a\xE2\x82"),
                                      client_frame(Opcode::Ping, true, "x"),
                                      client_frame(Opcode::Continuation, true, "\xAC")}));
    EXPECT_EQ(events, (std::vector<Seen>{ping("x"), message("a\xE2\x82\xAC")}));
    EXPECT_EQ(rig.take_output().frames, (std::vector<WireFrame>{{Opcode::Pong, true, "x"}}));
}

TEST(Session, AnUnsolicitedPongIsSurfacedAndAnswersNothing) {  // EC-C03
    Rig rig;
    EXPECT_EQ(rig.feed(client_frame(Opcode::Pong, true, "?")), (std::vector<Seen>{pong("?")}));
    EXPECT_TRUE(rig.take_output().frames.empty());
}

TEST(Session, ManyPingsInOneReadAreAnsweredWithTheLatestPongOnly) {  // EC-C02
    Rig rig;
    const auto events = rig.feed(cat({client_frame(Opcode::Ping, true, "1"),
                                      client_frame(Opcode::Ping, true, "2"),
                                      client_frame(Opcode::Ping, true, "3")}));
    EXPECT_EQ(events.size(), 3U);
    EXPECT_EQ(rig.take_output().frames, (std::vector<WireFrame>{{Opcode::Pong, true, "3"}}));
}

TEST(Session, ChunkingNeverChangesWhatTheSessionDoes) {
    const auto wire = cat({client_frame(Opcode::Text, false, "ab"),
                           client_frame(Opcode::Ping, true, "p"),
                           client_frame(Opcode::Continuation, true, "cd")});
    Rig whole;
    Rig bytewise;
    const std::vector<Seen> expected{ping("p"), message("abcd")};
    EXPECT_EQ(whole.feed(wire), expected);
    EXPECT_EQ(bytewise.feed(wire, 1), expected);
    EXPECT_EQ(whole.take_output().frames, bytewise.take_output().frames);
}

// ---- Close handshake: peer initiated ----------------------------------------------------------

TEST(Session, ThePeersCloseIsEchoedAndTheSessionClosesOnceItIsWritten) {  // FR-C2, EC-C08
    for (const unsigned code : {1000U, 1001U, 1011U, 3000U, 4999U}) {
        Rig rig;
        EXPECT_TRUE(
            rig.feed(client_frame(Opcode::Close, true, close_payload(code, "bye"))).empty());
        EXPECT_EQ(rig.session().state(), SessionState::Closing) << code;
        EXPECT_EQ(rig.take_output().frames,
                  (std::vector<WireFrame>{{Opcode::Close, true, close_payload(code)}}))
            << code;
        EXPECT_EQ(rig.session().state(), SessionState::Closed) << code;
        EXPECT_EQ(rig.poll(), (std::vector<Seen>{closed(code, "bye", true)})) << code;
        EXPECT_TRUE(rig.poll().empty()) << code;  // exactly once
    }
}

TEST(Session, ACloseWithoutStatusIsEchoedEmptyAndReportedAs1005) {  // EC-C05
    Rig rig;
    EXPECT_TRUE(rig.feed(client_frame(Opcode::Close, true, "")).empty());
    EXPECT_EQ(rig.take_output().frames, (std::vector<WireFrame>{{Opcode::Close, true, ""}}));
    EXPECT_EQ(rig.poll(), (std::vector<Seen>{closed(1005, "", true)}));
}

TEST(Session, InvalidClosePayloadsAreProtocolErrors) {  // EC-C06..C09, EC-U08
    struct Case {
        std::string payload;
        unsigned expected;
    };
    const std::vector<Case> cases{{std::string("\x03", 1), 1002},
                                  {close_payload(1005), 1002},
                                  {close_payload(1006), 1002},
                                  {close_payload(1015), 1002},
                                  {close_payload(999), 1002},
                                  {close_payload(1004), 1002},
                                  {close_payload(5000), 1002},
                                  {close_payload(1000, "\xFF"), 1007}};
    for (const Case& c : cases) {
        Rig rig;
        const auto events = rig.feed(client_frame(Opcode::Close, true, c.payload));
        ASSERT_EQ(events.size(), 1U) << c.expected;
        EXPECT_EQ(events[0].kind, Kind::ProtocolError);
        EXPECT_EQ(events[0].code, c.expected);
        EXPECT_EQ(rig.take_output().frames,
                  (std::vector<WireFrame>{
                      {Opcode::Close, true, close_payload(c.expected, events[0].reason)}}));
        EXPECT_EQ(rig.poll(), (std::vector<Seen>{closed(c.expected, events[0].reason, false)}));
    }
}

TEST(Session, ACloseFrameLongerThanAControlFrameMayBeIsAProtocolError) {  // EC-C09 on the wire
    // A reason over 123 bytes means a payload over 125, which no control frame can carry; the
    // payload-level rule itself is tested in ClosePayload.*.
    Rig rig;
    const auto events = rig.feed(oversized_control_frame(Opcode::Close, 126));
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].kind, Kind::ProtocolError);
    EXPECT_EQ(events[0].code, 1002U);
}

TEST(Session, EverythingAfterThePeersCloseIsIgnored) {  // FR-C5, EC-C10
    Rig rig;
    const auto events = rig.feed(cat({client_frame(Opcode::Close, true, close_payload(1000)),
                                      client_frame(Opcode::Text, true, "late"),
                                      client_frame(Opcode::Ping, true, "late")}));
    EXPECT_TRUE(events.empty());
    EXPECT_EQ(rig.take_output().frames,
              (std::vector<WireFrame>{{Opcode::Close, true, close_payload(1000)}}));  // no Pong
}

TEST(Session, ANeverCompletedMessageIsNotDeliveredWhenTheCloseArrives) {  // EC-F26
    Rig rig;
    EXPECT_TRUE(rig.feed(cat({client_frame(Opcode::Text, false, "partial"),
                              client_frame(Opcode::Close, true, close_payload(1000))}))
                    .empty());
    rig.take_output();
    EXPECT_EQ(rig.poll(), (std::vector<Seen>{closed(1000, "", true)}));
}

// ---- Close handshake: we initiate -------------------------------------------------------------

TEST(Session, ALocalCloseIsSentAndTheSessionWaitsForThePeersClose) {  // FR-C2
    Rig rig;
    ASSERT_TRUE(rig.session().close(CloseCode::Normal, "bye").has_value());
    EXPECT_EQ(rig.session().state(), SessionState::Closing);
    EXPECT_EQ(rig.take_output().frames,
              (std::vector<WireFrame>{{Opcode::Close, true, close_payload(1000, "bye")}}));
    EXPECT_TRUE(rig.poll().empty());  // not closed until the peer answers
    EXPECT_EQ(rig.session().state(), SessionState::Closing);
    // Our Close is already written, so the peer's Close completes the handshake right away.
    EXPECT_EQ(rig.feed(client_frame(Opcode::Close, true, close_payload(1000))),
              (std::vector<Seen>{closed(1000, "", true)}));
    EXPECT_EQ(rig.session().state(), SessionState::Closed);
}

TEST(Session, DataAndPingsFromThePeerAfterOurCloseAreDroppedButItsCloseIsHonoured) {  // EC-C10
    Rig rig;
    ASSERT_TRUE(rig.session().close(CloseCode::GoingAway, "").has_value());
    rig.take_output();
    const auto events = rig.feed(cat({client_frame(Opcode::Text, true, "ignored"),
                                      client_frame(Opcode::Ping, true, "ignored"),
                                      client_frame(Opcode::Close, true, close_payload(1001))}));
    EXPECT_EQ(events, (std::vector<Seen>{closed(1001, "", true)}));  // and nothing before it
    EXPECT_TRUE(rig.take_output().frames.empty());                   // no Pong, no second Close
}

TEST(Session, SimultaneousClosesProduceOneCloseFrameAndOneClosedEvent) {  // EC-C11
    Rig rig;
    ASSERT_TRUE(rig.session().close(CloseCode::Normal, "").has_value());
    EXPECT_TRUE(rig.feed(client_frame(Opcode::Close, true, close_payload(1000))).empty());
    const ParsedOutput out = rig.take_output();
    ASSERT_EQ(out.frames.size(), 1U);
    EXPECT_EQ(out.frames[0].opcode, Opcode::Close);
    EXPECT_EQ(rig.poll(), (std::vector<Seen>{closed(1000, "", true)}));
    EXPECT_TRUE(rig.poll().empty());
}

TEST(Session, ClosingTwiceIsHarmless) {  // EC-C15
    Rig rig;
    ASSERT_TRUE(rig.session().close(CloseCode::Normal, "a").has_value());
    ASSERT_TRUE(rig.session().close(CloseCode::GoingAway, "b").has_value());
    EXPECT_EQ(rig.take_output().frames,
              (std::vector<WireFrame>{{Opcode::Close, true, close_payload(1000, "a")}}));
    rig.feed(client_frame(Opcode::Close, true, close_payload(1000)));
    ASSERT_TRUE(rig.session().close(CloseCode::Normal, "c").has_value());  // after Closed too
    EXPECT_TRUE(rig.take_output().frames.empty());
}

TEST(Session, CloseRejectsCodesThatMustNotBeSentAndReasonsThatAreNotUtf8) {  // FR-C4
    Rig rig;
    for (const CloseCode code :
         {CloseCode::NoStatusReceived, CloseCode::AbnormalClosure, CloseCode::TlsHandshake}) {
        const auto r = rig.session().close(code, "");
        ASSERT_FALSE(r.has_value());
        EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    }
    const auto bad_reason = rig.session().close(CloseCode::Normal, "\xFF");
    ASSERT_FALSE(bad_reason.has_value());
    EXPECT_EQ(bad_reason.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(rig.session().state(), SessionState::Open);  // nothing happened
    EXPECT_TRUE(rig.take_output().frames.empty());
}

// ---- Protocol failures ------------------------------------------------------------------------

TEST(Session, AProtocolViolationSendsExactlyOneCloseAndThenIgnoresTheConnection) {
    Rig rig;
    const auto events = rig.feed(
        cat({client_frame(Opcode::Text, true, "\xFF"), client_frame(Opcode::Ping, true, "late")}));
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].kind, Kind::ProtocolError);
    EXPECT_EQ(events[0].code, 1007U);
    const ParsedOutput out = rig.take_output();
    ASSERT_EQ(out.frames.size(), 1U);  // the Ping after the violation got no Pong
    EXPECT_EQ(out.frames[0].opcode, Opcode::Close);
    EXPECT_EQ(rig.poll(), (std::vector<Seen>{closed(1007, events[0].reason, false)}));
}

TEST(Session, AFailureAfterOurOwnCloseDoesNotPairOurCodeWithTheFailureReason) {
    SessionConfig config;
    config.max_control_frames_per_second = 3;
    Rig rig{config};
    ASSERT_TRUE(rig.session().close(CloseCode::Normal, "bye").has_value());
    rig.take_output();  // our Close is written; the peer keeps talking
    std::vector<std::byte> pings;
    for (int i = 0; i < 4; ++i) {
        const auto frame = client_frame(Opcode::Ping, true, "");
        pings.insert(pings.end(), frame.begin(), frame.end());
    }
    const auto events = rig.feed(pings);
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].kind, Kind::ProtocolError);  // reported for the log...
    EXPECT_EQ(events[0].code, 1008U);
    // ...but no 1008 Close was sent, so Closed reports the Close we did send, with no unrelated
    // reason.
    EXPECT_EQ(events[1], closed(1000, "", false));
    EXPECT_TRUE(rig.take_output().frames.empty());
}

TEST(Session, AnUnmaskedClientFrameIsAProtocolError) {
    Rig rig;
    const std::vector<std::byte> unmasked{std::byte{0x81}, std::byte{0x01}, std::byte{'x'}};
    const auto events = rig.feed(unmasked);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].code, 1002U);
}

TEST(Session, AnOversizedFrameIsAProtocolErrorBeforeItsPayloadArrives) {
    SessionConfig config;
    config.parser.max_frame_payload = 10;
    Rig rig{config};
    const auto frame = client_frame(Opcode::Binary, true, std::string(11, 'a'));
    const std::vector<std::byte> header_only(frame.begin(), frame.begin() + 6);
    const auto events = rig.feed(header_only);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].code, 1009U);
}

TEST(Session, StreamingModeDeliversParts) {
    Rig rig{streaming_config()};
    const auto events = rig.feed(client_frame(Opcode::Binary, true, "abc"));
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].kind, Kind::Part);
    EXPECT_EQ(events[0].data, "abc");
    EXPECT_TRUE(events[0].final);
}

// ---- Transport end ----------------------------------------------------------------------------

TEST(Session, TheTransportClosingWithoutACloseIsAnAbnormalClosure) {  // EC-F23, EC-N04
    Rig rig;
    rig.feed(client_frame(Opcode::Text, false, "unfinished"));
    rig.session().on_transport_closed();
    EXPECT_EQ(rig.session().state(), SessionState::Closed);
    EXPECT_EQ(rig.poll(), (std::vector<Seen>{closed(1006, "", false)}));
    rig.session().on_transport_closed();  // idempotent
    EXPECT_TRUE(rig.poll().empty());
}

TEST(Session, ThePeerClosingTheTransportRightAfterItsCloseIsStillACleanClose) {  // EC-C13
    Rig rig;
    rig.feed(client_frame(Opcode::Close, true, close_payload(1000, "bye")));
    rig.session().on_transport_closed();  // before our echo was written
    EXPECT_EQ(rig.poll(), (std::vector<Seen>{closed(1000, "bye", true)}));
}

TEST(Session, ANoisyCloseOfTheTransportAfterOurCloseIsAbnormal) {
    Rig rig;
    ASSERT_TRUE(rig.session().close(CloseCode::Normal, "").has_value());
    rig.take_output();
    rig.session().on_transport_closed();  // the peer never answered
    EXPECT_EQ(rig.poll(), (std::vector<Seen>{closed(1006, "", false)}));
}

TEST(Session, ANewSessionHasNothingToSayAndAClosedOneProducesNothing) {
    Rig rig;
    EXPECT_TRUE(rig.poll().empty());
    EXPECT_TRUE(rig.session().pending_output().empty());
    rig.session().on_transport_closed();
    EXPECT_EQ(rig.poll(), (std::vector<Seen>{closed(1006, "", false)}));
    EXPECT_TRUE(rig.feed(client_frame(Opcode::Text, true, "x")).empty());
    EXPECT_TRUE(rig.session().pending_output().empty());
}

// ---- Sending ----------------------------------------------------------------------------------

TEST(SessionSend, TextAndBinaryGoOutAsSingleUnmaskedFrames) {
    Rig rig;
    ASSERT_TRUE(rig.session().send_text("hello").has_value());
    ASSERT_TRUE(
        rig.session().send_binary(wsrv::as_bytes(std::string_view{"\x01\x02"})).has_value());
    EXPECT_EQ(rig.take_output().frames,
              (std::vector<WireFrame>{{Opcode::Text, true, "hello"},
                                      {Opcode::Binary, true, "\x01\x02"}}));
}

TEST(SessionSend, InvalidUtf8TextIsRefusedAndNothingIsQueued) {  // EC-F36 (single-shot)
    Rig rig;
    const auto r = rig.session().send_text("bad \xFF");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::InvalidUtf8);
    EXPECT_TRUE(rig.take_output().frames.empty());
}

TEST(SessionSend, NothingCanBeSentOnceClosingHasStarted) {  // EC-C14
    Rig rig;
    ASSERT_TRUE(rig.session().close(CloseCode::Normal, "").has_value());
    rig.take_output();
    for (const auto& r : {rig.session().send_text("x"),
                          rig.session().send_binary({}),
                          rig.session().send_ping({})}) {
        ASSERT_FALSE(r.has_value());
        EXPECT_EQ(r.error().code, ErrorCode::Closing);
    }
    EXPECT_TRUE(rig.take_output().frames.empty());

    Rig after_peer_close;  // also when the peer started the close
    after_peer_close.feed(client_frame(Opcode::Close, true, close_payload(1000)));
    const auto r = after_peer_close.session().send_text("x");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::Closing);
}

TEST(SessionSend, PingsAreLimitedToOneInFlightAndOneHundredTwentyFiveBytes) {
    Rig rig;
    ASSERT_TRUE(rig.session().send_ping(wsrv::as_bytes(std::string_view{"a"})).has_value());
    const auto second = rig.session().send_ping({});
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code, ErrorCode::Backpressure);
    EXPECT_EQ(rig.take_output().frames, (std::vector<WireFrame>{{Opcode::Ping, true, "a"}}));
    const std::string too_long(126, 'x');
    const auto r = rig.session().send_ping(wsrv::as_bytes(std::string_view{too_long}));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
}

TEST(SessionSend, AFullOutboundQueueIsBackpressureNotGrowth) {  // EC-N09
    SessionConfig config;
    config.outbound.max_queued_bytes = 100;
    Rig rig{config};
    ASSERT_TRUE(rig.session()
                    .send_binary(wsrv::as_bytes(std::string_view{std::string(80, 'a')}))
                    .has_value());
    const auto full =
        rig.session().send_binary(wsrv::as_bytes(std::string_view{std::string(80, 'b')}));
    ASSERT_FALSE(full.has_value());
    EXPECT_EQ(full.error().code, ErrorCode::Backpressure);
    const auto never =
        rig.session().send_binary(wsrv::as_bytes(std::string_view{std::string(200, 'c')}));
    ASSERT_FALSE(never.has_value());
    EXPECT_EQ(never.error().code, ErrorCode::BufferFull);
    EXPECT_EQ(rig.take_output().frames.size(), 1U);
    EXPECT_TRUE(rig.session()
                    .send_binary(wsrv::as_bytes(std::string_view{std::string(80, 'b')}))
                    .has_value());
}

TEST(SessionSend, ABigMessageIsFragmentedAndAPingJumpsInBetweenFragments) {  // EC-F31, EC-F32
    Rig rig;
    const std::string payload(std::size_t{200} * 1024, 'm');
    ASSERT_TRUE(rig.session().send_binary(wsrv::as_bytes(std::string_view{payload})).has_value());
    std::vector<std::byte> wire;
    const wsrv::ByteView first = rig.session().pending_output();
    wire.insert(wire.end(), first.begin(), first.begin() + 1000);  // a partial write
    rig.session().consume_output(1000);
    ASSERT_TRUE(rig.session().send_ping(wsrv::as_bytes(std::string_view{"now"})).has_value());
    for (;;) {
        const wsrv::ByteView view = rig.session().pending_output();
        if (view.empty()) {
            break;
        }
        wire.insert(wire.end(), view.begin(), view.end());
        rig.session().consume_output(view.size());
    }
    const auto parsed = parse_server_output(wire);
    ASSERT_TRUE(parsed.ok);
    ASSERT_GE(parsed.frames.size(), 3U);
    EXPECT_EQ(parsed.frames[0].opcode, Opcode::Binary);
    EXPECT_FALSE(parsed.frames[0].fin);
    EXPECT_EQ(parsed.frames[1], (WireFrame{Opcode::Ping, true, "now"}));  // not inside a frame
    std::string joined;
    for (const WireFrame& f : parsed.frames) {
        if (f.opcode != Opcode::Ping) {
            joined += f.payload;
        }
    }
    EXPECT_EQ(joined, payload);
    EXPECT_TRUE(parsed.frames.back().fin);
}

TEST(SessionSend, CloseDropsMessagesThatHaveNotStartedAndNothingFollowsTheCloseFrame) {
    Rig rig;
    ASSERT_TRUE(rig.session().send_text("never sent").has_value());
    ASSERT_TRUE(rig.session().close(CloseCode::GoingAway, "bye").has_value());
    EXPECT_EQ(rig.take_output().frames,
              (std::vector<WireFrame>{{Opcode::Close, true, close_payload(1001, "bye")}}));
    EXPECT_TRUE(rig.take_output().frames.empty());
}

// ---- Configuration ----------------------------------------------------------------------------

TEST(SessionConfigValidation, TheDefaultsAreValid) {
    EXPECT_TRUE(wsrv::validate(SessionConfig{}).has_value());
}

TEST(SessionConfigValidation, MandatoryTimeoutsCannotBeDisabled) {  // FR-C2, FR-F11
    SessionConfig no_close;
    no_close.close_timeout = 0ms;
    const auto r = wsrv::validate(no_close);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::InvalidConfig);
    SessionConfig no_incomplete;
    no_incomplete.incomplete_message_timeout = 0ms;
    EXPECT_FALSE(wsrv::validate(no_incomplete).has_value());
}

TEST(SessionConfigValidation, DurationsAreCappedAtADayAndNeverNegative) {
    for (auto SessionConfig::*const field : {&SessionConfig::ping_interval,
                                             &SessionConfig::pong_timeout,
                                             &SessionConfig::idle_timeout,
                                             &SessionConfig::close_timeout,
                                             &SessionConfig::incomplete_message_timeout}) {
        SessionConfig ok;
        ok.*field = 24h;
        EXPECT_TRUE(wsrv::validate(ok).has_value());
        SessionConfig too_long;
        too_long.*field = 24h + 1ms;
        EXPECT_FALSE(wsrv::validate(too_long).has_value());
        SessionConfig huge;
        huge.*field = std::chrono::milliseconds::max();  // would overflow nanosecond arithmetic
        EXPECT_FALSE(wsrv::validate(huge).has_value());
        SessionConfig negative;
        negative.*field = -1ms;
        EXPECT_FALSE(wsrv::validate(negative).has_value());
    }
}

TEST(SessionConfigValidation, SizeLimitsHaveHardCeilingsAndNoZero) {  // SECURITY_DESIGN section 3
    constexpr std::uint64_t kFrameCeiling = 64ULL << 20;
    SessionConfig c;
    c.parser.max_frame_payload = kFrameCeiling;
    EXPECT_TRUE(wsrv::validate(c).has_value());
    c.parser.max_frame_payload = kFrameCeiling + 1;
    EXPECT_FALSE(wsrv::validate(c).has_value());
    c.parser.max_frame_payload = 0;
    EXPECT_FALSE(wsrv::validate(c).has_value());

    SessionConfig message;
    message.assembler.max_message_size = std::size_t{1} << 30;
    EXPECT_TRUE(wsrv::validate(message).has_value());
    message.assembler.max_message_size += 1;
    EXPECT_FALSE(wsrv::validate(message).has_value());
    message.assembler.max_message_size = 0;
    EXPECT_FALSE(wsrv::validate(message).has_value());

    SessionConfig fragments;
    fragments.assembler.max_fragments = 0;
    EXPECT_FALSE(wsrv::validate(fragments).has_value());

    SessionConfig frame;
    frame.outbound.max_frame_payload = 0;
    EXPECT_FALSE(wsrv::validate(frame).has_value());
    frame.outbound.max_frame_payload = std::size_t{64} << 20;
    EXPECT_TRUE(wsrv::validate(frame).has_value());
    frame.outbound.max_frame_payload += 1;
    EXPECT_FALSE(wsrv::validate(frame).has_value());
    frame.outbound.max_frame_payload = SIZE_MAX;  // wire_bound() would overflow
    EXPECT_FALSE(wsrv::validate(frame).has_value());

    SessionConfig queue;
    queue.outbound.max_queued_bytes = wsrv::kMessageOverhead - 1;
    EXPECT_FALSE(wsrv::validate(queue).has_value());
    queue.outbound.max_queued_bytes = std::size_t{1} << 30;
    EXPECT_TRUE(wsrv::validate(queue).has_value());
    queue.outbound.max_queued_bytes += 1;
    EXPECT_FALSE(wsrv::validate(queue).has_value());
}

TEST(SessionConfigValidationDeathTest, ConstructingASessionWithABadConfigAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    SessionConfig bad;
    bad.close_timeout = 0ms;
    const auto construct = [&bad] { const ProtocolSession session{bad, wsrv::TimePoint{}}; };
    EXPECT_DEATH(construct(), "WSRV_ASSERT failed");
}

// ---- Timers and rate limits -------------------------------------------------------------------

/// Every optional timer and limit off, the mandatory ones at their maximum; tests switch on only
/// what they exercise.
SessionConfig quiet_config() {
    SessionConfig c;
    c.ping_interval = 0ms;
    c.pong_timeout = 0ms;
    c.idle_timeout = 0ms;
    c.close_timeout =
        24h;  // mandatory timeouts cannot be disabled (FR-C2, FR-F11): "never" = a day
    c.incomplete_message_timeout = 24h;
    c.max_control_frames_per_second = 0;
    c.max_data_frames_per_second = 0;
    return c;
}

TEST(SessionTimers, ZeroDisablesThePingAndIdleTimers) {
    Rig rig{quiet_config()};
    rig.tick(24h - 1ms);
    EXPECT_EQ(rig.session().state(), SessionState::Open);
    EXPECT_TRUE(rig.take_output().frames.empty());
    EXPECT_TRUE(rig.poll().empty());
}

TEST(SessionTimers, TheIdleTimeoutClosesWith1001ExactlyAtTheLimit) {  // EC-C17
    SessionConfig config = quiet_config();
    config.idle_timeout = 60000ms;
    Rig rig{config};
    rig.tick(59999ms);
    EXPECT_EQ(rig.session().state(), SessionState::Open);
    rig.tick(1ms);
    EXPECT_EQ(rig.session().state(), SessionState::Closing);
    EXPECT_EQ(rig.take_output().frames,
              (std::vector<WireFrame>{{Opcode::Close, true, close_payload(1001, "idle timeout")}}));
}

TEST(SessionTimers, AnyCompleteFrameResetsTheIdleTimer) {
    SessionConfig config = quiet_config();
    config.idle_timeout = 60000ms;
    Rig rig{config};
    rig.tick(50000ms);
    rig.feed(client_frame(Opcode::Binary, true, "x"));
    rig.tick(59999ms);
    EXPECT_EQ(rig.session().state(), SessionState::Open);
    rig.tick(1ms);
    EXPECT_EQ(rig.session().state(), SessionState::Closing);
}

TEST(SessionTimers, ASlowTrickleOfOneFrameDoesNotKeepTheConnectionAlive) {  // T2/T7 slowloris
    SessionConfig config = quiet_config();
    config.idle_timeout = 10000ms;
    Rig rig{config};
    const auto frame = client_frame(Opcode::Binary, true, std::string(100, 'a'));
    for (std::size_t i = 0; i + 1 < frame.size(); ++i) {  // all but the last byte, 1 s apart
        rig.feed({frame[i]});
        rig.tick(1000ms);
    }
    EXPECT_EQ(rig.session().state(), SessionState::Closing);
}

TEST(SessionTimers, AQuietConnectionIsPingedAndAPongKeepsItAlive) {  // FR-C6
    SessionConfig config = quiet_config();
    config.ping_interval = 30000ms;
    config.pong_timeout = 10000ms;
    Rig rig{config};
    rig.tick(29999ms);
    EXPECT_TRUE(rig.take_output().frames.empty());
    rig.tick(1ms);
    EXPECT_EQ(rig.take_output().frames, (std::vector<WireFrame>{{Opcode::Ping, true, ""}}));
    rig.tick(9999ms);
    EXPECT_EQ(rig.session().state(), SessionState::Open);
    EXPECT_TRUE(rig.take_output().frames.empty());  // only one Ping in flight
    rig.feed(client_frame(Opcode::Pong, true, ""));
    rig.tick(20000ms);  // 30 s since the Pong's frame: the next Ping is due, no close
    EXPECT_EQ(rig.session().state(), SessionState::Open);
    rig.tick(10000ms);
    EXPECT_EQ(rig.take_output().frames, (std::vector<WireFrame>{{Opcode::Ping, true, ""}}));
}

TEST(SessionTimers, NoPongWithinTheTimeoutClosesWith1001) {  // EC-C16
    SessionConfig config = quiet_config();
    config.ping_interval = 30000ms;
    config.pong_timeout = 10000ms;
    Rig rig{config};
    rig.tick(30000ms);
    rig.take_output();                                                     // the Ping
    rig.feed(client_frame(Opcode::Text, true, "busy but not answering"));  // not a Pong
    rig.tick(9999ms);
    EXPECT_EQ(rig.session().state(), SessionState::Open);
    rig.tick(1ms);
    EXPECT_EQ(rig.session().state(), SessionState::Closing);
    EXPECT_EQ(rig.take_output().frames,
              (std::vector<WireFrame>{{Opcode::Close, true, close_payload(1001, "pong timeout")}}));
}

TEST(SessionTimers, ActivityDelaysTheHeartbeat) {
    SessionConfig config = quiet_config();
    config.ping_interval = 30000ms;
    Rig rig{config};
    rig.tick(20000ms);
    rig.feed(client_frame(Opcode::Text, true, "alive"));
    rig.tick(29999ms);
    EXPECT_TRUE(rig.take_output().frames.empty());
    rig.tick(1ms);
    EXPECT_EQ(rig.take_output().frames.size(), 1U);
}

TEST(SessionTimers, TimeNeverMovesBackwards) {
    SessionConfig config = quiet_config();
    config.idle_timeout = 1000ms;
    Rig rig{config};
    rig.tick(999ms);
    rig.session().tick(wsrv::TimePoint{});  // an earlier time is ignored
    EXPECT_EQ(rig.session().state(), SessionState::Open);
    rig.tick(1ms);
    EXPECT_EQ(rig.session().state(), SessionState::Closing);
}

TEST(SessionTimers, AnUnfinishedMessageIsAbandonedAfterTheIncompleteTimeout) {  // EC-F27
    SessionConfig config = quiet_config();
    config.incomplete_message_timeout = 30000ms;
    Rig rig{config};
    rig.tick(20000ms);  // quiet time before the message does not count
    rig.feed(client_frame(Opcode::Text, false, "first part"));
    rig.tick(29999ms);
    EXPECT_EQ(rig.session().state(), SessionState::Open);
    rig.tick(1ms);
    const auto events = rig.poll();
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].kind, Kind::ProtocolError);
    EXPECT_EQ(events[0].code, 1008U);
    EXPECT_EQ(rig.take_output().frames[0].opcode, Opcode::Close);
}

TEST(SessionTimers, ASlowSingleFrameMessageIsNotAnIncompleteFragmentedMessage) {
    // The incomplete-message timer is for fragmented messages (FRAGMENTATION.md 1.2 item 6); a
    // single frame arriving slowly is the idle timer's business, not this one's.
    SessionConfig config = quiet_config();
    config.incomplete_message_timeout = 30000ms;
    Rig rig{config};
    const auto frame = client_frame(Opcode::Binary, true, std::string(1000, 'a'));
    rig.feed(std::vector<std::byte>(frame.begin(), frame.begin() + 506));  // header + 500 bytes
    rig.tick(29999ms + 1ms);
    rig.tick(10min);
    EXPECT_EQ(rig.session().state(), SessionState::Open);
    EXPECT_TRUE(rig.poll().empty());
}

TEST(SessionTimers, TheIncompleteTimerRunsFromTheEndOfTheFirstFragment) {
    SessionConfig config = quiet_config();
    config.incomplete_message_timeout = 30000ms;
    Rig rig{config};
    rig.feed(client_frame(Opcode::Text, false, "a"));
    rig.tick(29999ms);
    rig.feed(client_frame(Opcode::Continuation, false, "b"));  // progress does not restart it
    rig.tick(1ms);
    const auto events = rig.poll();
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].code, 1008U);
}

TEST(SessionTimers, AFinishedOrControlOnlyMessageStartsNoTimer) {
    SessionConfig config = quiet_config();
    config.incomplete_message_timeout = 1000ms;
    Rig rig{config};
    rig.feed(client_frame(Opcode::Ping, true, ""));
    rig.feed(client_frame(Opcode::Text, false, "a"));
    rig.feed(client_frame(Opcode::Continuation, true, "b"));
    rig.tick(24h);
    EXPECT_EQ(rig.session().state(), SessionState::Open);
}

TEST(SessionTimers, ACloseTheirPeerNeverAnswersEndsAtTheCloseTimeout) {  // EC-C12
    SessionConfig config = quiet_config();
    config.close_timeout = 5000ms;
    Rig rig{config};
    ASSERT_TRUE(rig.session().close(CloseCode::Normal, "").has_value());
    rig.take_output();
    rig.tick(4999ms);
    EXPECT_TRUE(rig.poll().empty());
    EXPECT_EQ(rig.session().state(), SessionState::Closing);
    rig.tick(1ms);
    EXPECT_EQ(rig.poll(), (std::vector<Seen>{closed(1006, "", false)}));
    EXPECT_EQ(rig.session().state(), SessionState::Closed);
}

TEST(SessionTimers, AFailureCloseThatCannotBeWrittenAlsoEndsAtTheCloseTimeout) {
    SessionConfig config = quiet_config();
    config.close_timeout = 5000ms;
    Rig rig{config};
    const auto events = rig.feed(client_frame(Opcode::Text, true, "\xFF"));
    ASSERT_EQ(events.size(), 1U);
    rig.tick(5000ms);  // the peer's TCP window is full: the Close was never written
    EXPECT_EQ(rig.poll(), (std::vector<Seen>{closed(1006, "", false)}));
}

TEST(SessionTimers, ThePeersCloseWhoseEchoCannotBeWrittenEndsAtTheCloseTimeoutWithItsCode) {
    SessionConfig config = quiet_config();
    config.close_timeout = 5000ms;
    Rig rig{config};
    rig.feed(client_frame(Opcode::Close, true, close_payload(3001, "x")));
    rig.tick(5000ms);
    EXPECT_EQ(rig.poll(), (std::vector<Seen>{closed(3001, "x", false)}));
}

TEST(SessionRates, TooManyControlFramesInOneWindowCloseWith1008) {  // EC-C04
    SessionConfig config = quiet_config();
    config.max_control_frames_per_second = 5;
    Rig rig{config};
    std::vector<std::byte> pings;
    for (int i = 0; i < 6; ++i) {
        const auto frame = client_frame(Opcode::Ping, true, "");
        pings.insert(pings.end(), frame.begin(), frame.end());
    }
    const auto events = rig.feed(pings);
    ASSERT_EQ(events.size(), 6U);  // five Pings, then the violation
    EXPECT_EQ(events[4].kind, Kind::Ping);
    EXPECT_EQ(events[5].kind, Kind::ProtocolError);
    EXPECT_EQ(events[5].code, 1008U);
}

TEST(SessionRates, TheWindowResetsOnTheFirstTickAtLeastOneSecondLater) {
    SessionConfig config = quiet_config();
    config.max_control_frames_per_second = 5;
    Rig rig{config};
    auto five_pings = [&] {
        std::vector<std::byte> pings;
        for (int i = 0; i < 5; ++i) {
            const auto frame = client_frame(Opcode::Ping, true, "");
            pings.insert(pings.end(), frame.begin(), frame.end());
        }
        return rig.feed(pings);
    };
    EXPECT_EQ(five_pings().size(), 5U);
    rig.tick(999ms);
    EXPECT_EQ(rig.feed(client_frame(Opcode::Ping, true, "")).back().kind, Kind::ProtocolError);

    Rig again{config};
    EXPECT_EQ(again
                  .feed(cat({client_frame(Opcode::Ping, true, ""),
                             client_frame(Opcode::Ping, true, ""),
                             client_frame(Opcode::Ping, true, ""),
                             client_frame(Opcode::Ping, true, ""),
                             client_frame(Opcode::Ping, true, "")}))
                  .size(),
              5U);
    again.tick(1000ms);
    EXPECT_EQ(again.feed(client_frame(Opcode::Ping, true, "")).back().kind, Kind::Ping);
}

TEST(SessionRates, TooManyDataFramesInOneWindowCloseWith1008) {  // EC-F12, EC-F28
    SessionConfig config = quiet_config();
    config.max_data_frames_per_second = 3;
    Rig rig{config};
    std::vector<std::byte> frames;
    for (int i = 0; i < 4; ++i) {
        const auto frame = client_frame(Opcode::Text, true, "a");
        frames.insert(frames.end(), frame.begin(), frame.end());
    }
    const auto events = rig.feed(frames);
    ASSERT_EQ(events.size(), 4U);  // three messages, then the violation
    EXPECT_EQ(events[2].kind, Kind::Message);
    EXPECT_EQ(events[3].kind, Kind::ProtocolError);
    EXPECT_EQ(events[3].code, 1008U);
}

TEST(SessionRates, ZeroLengthFramesAndOneByteFragmentsCountAsDataFrames) {  // EC-F12, EC-F28
    SessionConfig config = quiet_config();
    config.max_data_frames_per_second = 3;
    Rig empty{config};
    std::vector<std::byte> frames;
    for (int i = 0; i < 4; ++i) {
        const auto frame = client_frame(Opcode::Binary, true, "");
        frames.insert(frames.end(), frame.begin(), frame.end());
    }
    EXPECT_EQ(empty.feed(frames).back().code, 1008U);

    Rig fragments{config};
    std::vector<std::byte> flood = client_frame(Opcode::Text, false, "a");
    for (int i = 0; i < 3; ++i) {
        const auto frame = client_frame(Opcode::Continuation, false, "a");
        flood.insert(flood.end(), frame.begin(), frame.end());
    }
    const auto events = fragments.feed(flood);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].kind, Kind::ProtocolError);
    EXPECT_EQ(events[0].code, 1008U);
}

TEST(SessionRates, ZeroDisablesTheLimits) {
    Rig rig{quiet_config()};
    std::vector<std::byte> frames;
    for (int i = 0; i < 300; ++i) {
        const auto ping = client_frame(Opcode::Ping, true, "");
        const auto text = client_frame(Opcode::Text, true, "a");
        frames.insert(frames.end(), ping.begin(), ping.end());
        frames.insert(frames.end(), text.begin(), text.end());
    }
    EXPECT_EQ(rig.feed(frames).size(), 600U);
}

}  // namespace

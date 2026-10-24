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
Seen protocol_error(unsigned code, std::string reason) {
    return Seen{Kind::ProtocolError, MessageType::Text, {}, false, code, std::move(reason), false};
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

    wsrv::TimePoint start_{};
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
    EXPECT_EQ(whole.feed(wire), bytewise.feed(wire, 1));
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
                                  {close_payload(1000, "\xFF"), 1007},
                                  {close_payload(1000, std::string(124, 'r')), 1002}};
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
    EXPECT_TRUE(rig.feed(client_frame(Opcode::Close, true, close_payload(1000))).empty());
    EXPECT_EQ(rig.poll(), (std::vector<Seen>{closed(1000, "", true)}));
    EXPECT_EQ(rig.session().state(), SessionState::Closed);
}

TEST(Session, DataAndPingsFromThePeerAfterOurCloseAreDroppedButItsCloseIsHonoured) {  // EC-C10
    Rig rig;
    ASSERT_TRUE(rig.session().close(CloseCode::GoingAway, "").has_value());
    rig.take_output();
    const auto events = rig.feed(cat({client_frame(Opcode::Text, true, "ignored"),
                                      client_frame(Opcode::Ping, true, "ignored"),
                                      client_frame(Opcode::Close, true, close_payload(1001))}));
    EXPECT_TRUE(events.empty());
    EXPECT_TRUE(rig.take_output().frames.empty());  // no Pong, no second Close
    EXPECT_EQ(rig.poll(), (std::vector<Seen>{closed(1001, "", true)}));
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
    rig.poll();
    EXPECT_TRUE(rig.feed(client_frame(Opcode::Text, true, "x")).empty());
    EXPECT_TRUE(rig.session().pending_output().empty());
}

}  // namespace

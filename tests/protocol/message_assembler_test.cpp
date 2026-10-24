// SPDX-License-Identifier: Apache-2.0
#include "protocol/message_assembler.hpp"

#include "base/bytes.hpp"
#include "protocol/frame_parser.hpp"
#include "protocol/frame_writer.hpp"
#include "protocol/masker.hpp"
#include "protocol/utf8.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace {

using wsrv::AssemblerConfig;
using wsrv::CloseCode;
using wsrv::MessageEvent;
using wsrv::MessageType;
using wsrv::Opcode;

struct Spec {
    Opcode opcode;
    bool fin;
    std::string payload;
};

constexpr wsrv::MaskKey kKey{std::byte{0x0f}, std::byte{0xa1}, std::byte{0x5c}, std::byte{0x33}};

std::vector<std::byte> wire(const std::vector<Spec>& frames) {
    std::vector<std::byte> out;
    for (const Spec& spec : frames) {
        wsrv::FrameHeader h;
        h.fin = spec.fin;
        h.opcode = spec.opcode;
        h.masked = true;
        h.payload_length = spec.payload.size();
        h.mask_key = kKey;
        std::array<std::byte, wsrv::kMaxHeaderSize> header{};
        const std::size_t n = *wsrv::encode_header(header, h);
        out.insert(out.end(), header.begin(), header.begin() + static_cast<std::ptrdiff_t>(n));
        std::vector<std::byte> body(spec.payload.size());
        std::transform(spec.payload.begin(), spec.payload.end(), body.begin(), [](char c) {
            return static_cast<std::byte>(c);
        });
        wsrv::apply_mask(body, kKey);
        out.insert(out.end(), body.begin(), body.end());
    }
    return out;
}

struct Ev {
    MessageEvent::Kind kind;
    MessageType type;
    std::string data;
    bool final;
    Opcode control;

    friend bool operator==(const Ev&, const Ev&) = default;
};

struct Collected {
    std::vector<Ev> events;
    std::optional<wsrv::ProtocolFailure> failure;
};

AssemblerConfig buffered(std::size_t max_message = 1 << 20, std::size_t max_fragments = 1024) {
    AssemblerConfig c;
    c.max_message_size = max_message;
    c.max_fragments = max_fragments;
    return c;
}

AssemblerConfig streaming(std::size_t max_message = 1 << 20) {
    AssemblerConfig c = buffered(max_message);
    c.streaming = true;
    return c;
}

/// Bytes -> FrameParser -> MessageAssembler, in chunks of `chunk` (0 = all at once).
Collected pipeline(const std::vector<std::byte>& bytes,
                   const AssemblerConfig& config,
                   std::size_t chunk = 0,
                   wsrv::FrameParserConfig parser_config = {}) {
    wsrv::FrameParser parser{parser_config};
    wsrv::MessageAssembler assembler{config};
    Collected out;
    const std::size_t step = chunk == 0 ? std::max<std::size_t>(bytes.size(), 1) : chunk;
    for (std::size_t pos = 0; pos < bytes.size(); pos += step) {
        const std::size_t take = std::min(step, bytes.size() - pos);
        std::vector<std::byte> buffer{bytes.begin() + static_cast<std::ptrdiff_t>(pos),
                                      bytes.begin() + static_cast<std::ptrdiff_t>(pos + take)};
        wsrv::MutableByteView input{buffer};
        for (;;) {
            const auto frame_event = parser.next(input);
            if (!frame_event) {
                out.failure = frame_event.error();
                return out;
            }
            if (!frame_event->has_value()) {
                break;
            }
            const auto message_event = assembler.handle(**frame_event);
            if (!message_event) {
                out.failure = message_event.error();
                return out;
            }
            if (message_event->has_value()) {
                const MessageEvent& e = **message_event;
                out.events.push_back(
                    Ev{e.kind,
                       e.type,
                       std::string{wsrv::as_chars(e.data)} + std::string{wsrv::as_chars(e.tail)},
                       e.final,
                       e.control_opcode});
            }
        }
    }
    return out;
}

enum class DiscardWhen : std::uint8_t { AfterFirstDataPayload, AfterFirstControl };

/// Like pipeline(), but calls discard_message() once at the chosen moment, the way the session does
/// after it sends a Close (docs/FRAGMENTATION.md, EC-F30).
Collected pipeline_with_discard(const std::vector<std::byte>& bytes,
                                const AssemblerConfig& config,
                                std::size_t chunk,
                                DiscardWhen when) {
    wsrv::FrameParser parser;
    wsrv::MessageAssembler assembler{config};
    Collected out;
    bool discarded = false;
    Opcode frame_opcode = Opcode::Text;
    for (std::size_t pos = 0; pos < bytes.size(); pos += chunk) {
        const std::size_t take = std::min(chunk, bytes.size() - pos);
        std::vector<std::byte> buffer{bytes.begin() + static_cast<std::ptrdiff_t>(pos),
                                      bytes.begin() + static_cast<std::ptrdiff_t>(pos + take)};
        wsrv::MutableByteView input{buffer};
        for (;;) {
            const auto frame_event = parser.next(input);
            if (!frame_event) {
                out.failure = frame_event.error();
                return out;
            }
            if (!frame_event->has_value()) {
                break;
            }
            const wsrv::FrameEvent& fe = **frame_event;
            if (fe.kind == wsrv::FrameEvent::Kind::Header) {
                frame_opcode = fe.header.opcode;
            }
            const auto message_event = assembler.handle(fe);
            if (!message_event) {
                out.failure = message_event.error();
                return out;
            }
            if (message_event->has_value()) {
                const MessageEvent& e = **message_event;
                out.events.push_back(
                    Ev{e.kind,
                       e.type,
                       std::string{wsrv::as_chars(e.data)} + std::string{wsrv::as_chars(e.tail)},
                       e.final,
                       e.control_opcode});
            }
            const bool trigger =
                when == DiscardWhen::AfterFirstDataPayload
                    ? fe.kind == wsrv::FrameEvent::Kind::Payload && !wsrv::is_control(frame_opcode)
                    : fe.kind == wsrv::FrameEvent::Kind::End && wsrv::is_control(frame_opcode);
            if (trigger && !discarded) {
                discarded = true;
                out.events.clear();  // what came before the discard was delivered legitimately
                assembler.discard_message();
            }
        }
    }
    return out;
}

Ev message(MessageType type, std::string data) {
    return Ev{MessageEvent::Kind::Message,
              type,
              std::move(data),
              true,
              Opcode::Ping};  // Ping = the placeholder
}

Ev control(Opcode opcode, std::string data) {
    return Ev{MessageEvent::Kind::Control, MessageType::Text, std::move(data), true, opcode};
}

Ev part(MessageType type, std::string data, bool final) {
    return Ev{MessageEvent::Kind::Part, type, std::move(data), final, Opcode::Ping};
}

// ---- buffered: the basics
// -------------------------------------------------------------------------------

TEST(MessageAssembler, SingleFrameMessages) {
    const Collected text = pipeline(wire({{Opcode::Text, true, "Hello"}}), buffered());
    EXPECT_EQ(text.events, (std::vector<Ev>{message(MessageType::Text, "Hello")}));
    const Collected binary =
        pipeline(wire({{Opcode::Binary, true, std::string("\x00\xff\x01", 3)}}), buffered());
    EXPECT_EQ(binary.events,
              (std::vector<Ev>{message(MessageType::Binary, std::string("\x00\xff\x01", 3))}));
}

TEST(MessageAssembler, ZeroLengthMessagesAreDelivered) {
    const Collected out =
        pipeline(wire({{Opcode::Text, true, ""}, {Opcode::Binary, true, ""}}), buffered());
    EXPECT_EQ(out.events,
              (std::vector<Ev>{message(MessageType::Text, ""), message(MessageType::Binary, "")}));
}

TEST(MessageAssembler, FragmentsAreJoinedIntoOneMessage) {
    const Collected out = pipeline(wire({{Opcode::Text, false, "Hel"},
                                         {Opcode::Continuation, false, "lo, "},
                                         {Opcode::Continuation, true, "world"}}),
                                   buffered());
    EXPECT_EQ(out.events, (std::vector<Ev>{message(MessageType::Text, "Hello, world")}));
}

TEST(MessageAssembler, AZeroLengthFinalContinuationCompletesTheMessage) {  // EC-F37
    const Collected out = pipeline(
        wire({{Opcode::Binary, false, "abc"}, {Opcode::Continuation, true, ""}}), buffered());
    EXPECT_EQ(out.events, (std::vector<Ev>{message(MessageType::Binary, "abc")}));
}

TEST(MessageAssembler, SeveralMessagesInARow) {
    const Collected out = pipeline(wire({{Opcode::Text, true, "one"},
                                         {Opcode::Binary, true, "two"},
                                         {Opcode::Text, false, "th"},
                                         {Opcode::Continuation, true, "ree"}}),
                                   buffered());
    EXPECT_EQ(out.events,
              (std::vector<Ev>{message(MessageType::Text, "one"),
                               message(MessageType::Binary, "two"),
                               message(MessageType::Text, "three")}));
}

// ---- sequencing errors
// ----------------------------------------------------------------------------------

TEST(MessageAssembler, ContinuationWithoutAStartedMessageIsAProtocolError) {  // EC-F09
    const Collected out = pipeline(wire({{Opcode::Continuation, true, "x"}}), buffered());
    ASSERT_TRUE(out.failure.has_value());
    EXPECT_EQ(out.failure->code, CloseCode::ProtocolError);
    const Collected after_complete = pipeline(
        wire({{Opcode::Text, true, "done"}, {Opcode::Continuation, true, "x"}}), buffered());
    ASSERT_TRUE(after_complete.failure.has_value());
    EXPECT_EQ(after_complete.events.size(),
              1U);  // the first message was delivered before the error
}

TEST(MessageAssembler, ANewDataFrameInsideAFragmentedMessageIsAProtocolError) {  // EC-F10
    for (const Opcode intruder : {Opcode::Text, Opcode::Binary}) {
        const Collected out =
            pipeline(wire({{Opcode::Text, false, "part"}, {intruder, true, "oops"}}), buffered());
        ASSERT_TRUE(out.failure.has_value());
        EXPECT_EQ(out.failure->code, CloseCode::ProtocolError);
        EXPECT_TRUE(out.events.empty());
    }
}

TEST(MessageAssembler, ControlFramesBetweenFragmentsDoNotDisturbTheMessage) {  // EC-F11
    const Collected out = pipeline(wire({{Opcode::Text, false, "Hel"},
                                         {Opcode::Ping, true, "p1"},
                                         {Opcode::Continuation, false, "lo"},
                                         {Opcode::Pong, true, ""},
                                         {Opcode::Continuation, true, "!"}}),
                                   buffered());
    EXPECT_EQ(out.events,
              (std::vector<Ev>{control(Opcode::Ping, "p1"),
                               control(Opcode::Pong, ""),
                               message(MessageType::Text, "Hello!")}));
}

TEST(MessageAssembler, AControlFrameOutsideAMessageIsDeliveredToo) {
    const Collected out =
        pipeline(wire({{Opcode::Ping, true, "hi"}, {Opcode::Text, true, "x"}}), buffered());
    EXPECT_EQ(out.events,
              (std::vector<Ev>{control(Opcode::Ping, "hi"), message(MessageType::Text, "x")}));
}

TEST(MessageAssembler, ACharacterSplitAcrossFragmentsAndAPingStillDecodes) {  // EC-F25, EC-U06
    const Collected out = pipeline(wire({{Opcode::Text, false, "a\xE2\x82"},
                                         {Opcode::Ping, true, "x"},
                                         {Opcode::Continuation,
                                          true,
                                          "\xAC"
                                          "b"}}),
                                   buffered());
    EXPECT_EQ(out.events,
              (std::vector<Ev>{control(Opcode::Ping, "x"),
                               message(MessageType::Text,
                                       "a\xE2\x82\xAC"
                                       "b")}));
}

TEST(MessageAssembler, ACloseFrameMidMessageIsDeliveredAndTheMessageCanBeDiscarded) {  // EC-F26
    wsrv::FrameParser parser;
    wsrv::MessageAssembler assembler{buffered()};
    std::vector<std::byte> bytes =
        wire({{Opcode::Text, false, "partial"}, {Opcode::Close, true, std::string("\x03\xe8", 2)}});
    wsrv::MutableByteView input{bytes};
    std::vector<Ev> events;
    for (;;) {
        const auto fe = parser.next(input);
        ASSERT_TRUE(fe.has_value());
        if (!fe->has_value()) {
            break;
        }
        const auto me = assembler.handle(**fe);
        ASSERT_TRUE(me.has_value());
        if (me->has_value()) {
            events.push_back(Ev{(*me)->kind,
                                (*me)->type,
                                std::string{wsrv::as_chars((*me)->data)},
                                (*me)->final,
                                (*me)->control_opcode});
        }
    }
    ASSERT_EQ(events.size(), 1U);  // only the Close; the partial message was never completed
    EXPECT_EQ(events[0].control, Opcode::Close);
    EXPECT_TRUE(assembler.in_message());
    assembler.discard_message();
    EXPECT_FALSE(assembler.in_message());
}

// ---- discard_message(): the session drops a message it will not deliver (EC-F30)
// -----------------------------------------------------------------------------------------------

/// The rest of the discarded message still arrives after the discard; none of it may surface, and
/// the stream after the discarded message must be clean.
TEST(MessageAssembler, DiscardInTheMiddleOfAFrameSwallowsTheRestOfThatFrame) {  // EC-F26, EC-F30
    // header (6 bytes) + 4 payload bytes in the first read; the other 4 arrive after the discard.
    const auto bytes = wire({{Opcode::Text, true, "abcdefgh"}, {Opcode::Text, true, "next"}});
    for (const AssemblerConfig& config : {buffered(), streaming()}) {
        const Collected out =
            pipeline_with_discard(bytes, config, 10, DiscardWhen::AfterFirstDataPayload);
        EXPECT_FALSE(out.failure.has_value()) << config.streaming;
        ASSERT_EQ(out.events.size(), 1U) << config.streaming;
        EXPECT_EQ(out.events[0].data, "next") << config.streaming;
    }
}

TEST(MessageAssembler, DiscardMidFrameStreamingBinaryDeliversNoFurtherParts) {  // EC-F30
    const auto bytes = wire({{Opcode::Binary, false, "abcdefgh"},
                             {Opcode::Continuation, true, "ijkl"},
                             {Opcode::Binary, true, "next"}});
    const Collected out =
        pipeline_with_discard(bytes, streaming(), 12, DiscardWhen::AfterFirstDataPayload);
    EXPECT_FALSE(out.failure.has_value());
    EXPECT_EQ(out.events, (std::vector<Ev>{part(MessageType::Binary, "next", true)}));
}

TEST(MessageAssembler, DiscardInsideAMultiByteCharacterIsNotAnEncodingError) {  // EC-F30, EC-U06
    // The cut lands inside the euro sign; the validator must not be asked about the rest.
    const auto bytes = wire({{Opcode::Text, true, "a\xE2\x82\xAC"}, {Opcode::Text, true, "next"}});
    const Collected out =
        pipeline_with_discard(bytes, streaming(), 8, DiscardWhen::AfterFirstDataPayload);
    EXPECT_FALSE(out.failure.has_value());
    EXPECT_EQ(out.events, (std::vector<Ev>{part(MessageType::Text, "next", true)}));
}

TEST(MessageAssembler, ContinuationsOfADiscardedMessageAreSwallowedUntilFin) {  // EC-F30, F26
    // The server sent Close mid-message, but the peer is still sending the rest of that message
    // (RFC 6455 section 5.5.1: keep reading until the peer's Close). Not a protocol error.
    const auto bytes = wire({{Opcode::Text, false, "ab"},
                             {Opcode::Ping, true, "p"},
                             {Opcode::Continuation, false, "cd"},
                             {Opcode::Continuation, true, "ef"},
                             {Opcode::Text, true, "next"}});
    for (const AssemblerConfig& config : {buffered(), streaming()}) {
        const Collected out =
            pipeline_with_discard(bytes, config, 4096, DiscardWhen::AfterFirstControl);
        EXPECT_FALSE(out.failure.has_value()) << config.streaming;
        ASSERT_EQ(out.events.size(), 1U) << config.streaming;
        EXPECT_EQ(out.events[0].data, "next") << config.streaming;
    }
}

TEST(MessageAssembler, ANewDataFrameWhileDrainingADiscardedMessageIsStillAProtocolError) {  // F09
    const auto bytes = wire(
        {{Opcode::Text, false, "ab"}, {Opcode::Ping, true, "p"}, {Opcode::Text, true, "oops"}});
    const Collected out =
        pipeline_with_discard(bytes, buffered(), 4096, DiscardWhen::AfterFirstControl);
    ASSERT_TRUE(out.failure.has_value());
    EXPECT_EQ(out.failure->code, CloseCode::ProtocolError);
}

TEST(MessageAssembler, LimitsStillApplyWhileDrainingADiscardedMessage) {  // EC-F19, EC-F20
    const auto too_big = wire({{Opcode::Text, false, "ab"},
                               {Opcode::Ping, true, "p"},
                               {Opcode::Continuation, true, std::string(11, 'x')}});
    const Collected big =
        pipeline_with_discard(too_big, buffered(10), 4096, DiscardWhen::AfterFirstControl);
    ASSERT_TRUE(big.failure.has_value());
    EXPECT_EQ(big.failure->code, CloseCode::MessageTooBig);

    std::vector<Spec> flood{{Opcode::Text, false, "a"}, {Opcode::Ping, true, "p"}};
    for (int i = 0; i < 4; ++i) {
        flood.push_back({Opcode::Continuation, false, ""});
    }
    const Collected many = pipeline_with_discard(
        wire(flood), buffered(1 << 20, 3), 4096, DiscardWhen::AfterFirstControl);
    ASSERT_TRUE(many.failure.has_value());
    EXPECT_EQ(many.failure->code, CloseCode::PolicyViolation);
}

// ---- limits
// ---------------------------------------------------------------------------------------------

TEST(MessageAssembler, MessageSizeLimitIsExactAndEnforcedBeforeThePayloadArrives) {  // EC-F19
    const Collected at_limit =
        pipeline(wire({{Opcode::Binary, true, std::string(10, 'a')}}), buffered(10));
    EXPECT_EQ(at_limit.events.size(), 1U);
    const Collected over =
        pipeline(wire({{Opcode::Binary, true, std::string(11, 'a')}}), buffered(10));
    ASSERT_TRUE(over.failure.has_value());
    EXPECT_EQ(over.failure->code, CloseCode::MessageTooBig);

    // Ten one-byte fragments fit; the eleventh fragment's HEADER is what trips the limit.
    std::vector<Spec> specs{{Opcode::Binary, false, "a"}};
    for (int i = 0; i < 9; ++i) {
        specs.push_back({Opcode::Continuation, false, "a"});
    }
    specs.push_back({Opcode::Continuation, true, "a"});
    EXPECT_EQ(pipeline(wire(specs), buffered(11)).events.size(), 1U);
    const Collected tripped = pipeline(wire(specs), buffered(10));
    ASSERT_TRUE(tripped.failure.has_value());
    EXPECT_EQ(tripped.failure->code, CloseCode::MessageTooBig);
}

TEST(MessageAssembler, TooManyFragmentsIsAPolicyViolation) {  // EC-F20, EC-F28
    auto fragments = [](int count) {
        std::vector<Spec> specs{{Opcode::Text, false, "a"}};
        for (int i = 1; i < count - 1; ++i) {
            specs.push_back({Opcode::Continuation, false, "a"});
        }
        specs.push_back({Opcode::Continuation, true, "a"});
        return wire(specs);
    };
    EXPECT_EQ(pipeline(fragments(5), buffered(1 << 20, 5)).events.size(), 1U);
    const Collected out = pipeline(fragments(6), buffered(1 << 20, 5));
    ASSERT_TRUE(out.failure.has_value());
    EXPECT_EQ(out.failure->code, CloseCode::PolicyViolation);
}

TEST(MessageAssembler, ControlFramesDoNotCountAsFragmentsOrSize) {
    const Collected out = pipeline(wire({{Opcode::Text, false, "a"},
                                         {Opcode::Ping, true, std::string(100, 'p')},
                                         {Opcode::Continuation, true, "b"}}),
                                   buffered(2, 2));
    EXPECT_EQ(out.events,
              (std::vector<Ev>{control(Opcode::Ping, std::string(100, 'p')),
                               message(MessageType::Text, "ab")}));
}

// ---- UTF-8
// ----------------------------------------------------------------------------------------------

TEST(MessageAssembler, InvalidUtf8FailsAtTheFirstBadByteNotAtTheEndOfTheMessage) {  // EC-U06
    // The first fragment already contains the bad byte; no final fragment ever arrives.
    const Collected out = pipeline(wire({{Opcode::Text, false, "ok\xED\xA0\x80"}}), buffered());
    ASSERT_TRUE(out.failure.has_value());
    EXPECT_EQ(out.failure->code, CloseCode::InvalidPayload);
}

TEST(MessageAssembler, ATextMessageThatEndsInsideACharacterIsInvalid) {
    const Collected out = pipeline(wire({{Opcode::Text, true, "a\xE2\x82"}}), buffered());
    ASSERT_TRUE(out.failure.has_value());
    EXPECT_EQ(out.failure->code, CloseCode::InvalidPayload);
    const Collected fragmented = pipeline(
        wire({{Opcode::Text, false, "a\xE2"}, {Opcode::Continuation, true, "\x82"}}), buffered());
    ASSERT_TRUE(fragmented.failure.has_value());
}

TEST(MessageAssembler, BinaryMessagesAreNeverValidated) {  // EC-U07
    const Collected out = pipeline(wire({{Opcode::Binary, true, "\xFF\xFE\xC0\x80"}}), buffered());
    ASSERT_FALSE(out.failure.has_value());
    EXPECT_EQ(out.events.size(), 1U);
}

TEST(MessageAssembler, EachMessageStartsWithAFreshUtf8State) {
    const Collected out = pipeline(
        wire({{Opcode::Text, true, "caf\xC3\xA9"}, {Opcode::Text, true, "plain"}}), buffered());
    EXPECT_EQ(out.events.size(), 2U);
}

// ---- state
// ----------------------------------------------------------------------------------------------

TEST(MessageAssembler, StaysFailedUntilReset) {
    wsrv::MessageAssembler assembler{buffered()};
    wsrv::FrameEvent header;
    header.kind = wsrv::FrameEvent::Kind::Header;
    header.header.opcode = Opcode::Continuation;  // illegal first frame
    header.header.masked = true;
    ASSERT_FALSE(assembler.handle(header).has_value());
    header.header.opcode = Opcode::Text;
    EXPECT_FALSE(assembler.handle(header).has_value());
    assembler.reset();
    EXPECT_TRUE(assembler.handle(header).has_value());
}

// ---- property: any fragmentation and any chunking
// -------------------------------------------------------

std::string random_text(std::mt19937& rng) {
    static const std::vector<std::string> kPieces{
        "a", "Z", "\xC3\xA9", "\xE2\x82\xAC", "\xF0\x9F\x98\x80", " ", "\xD7\x90"};
    std::string out;
    const std::size_t n = rng() % 60;
    for (std::size_t i = 0; i < n; ++i) {
        out += kPieces[rng() % kPieces.size()];
    }
    return out;
}

std::vector<Spec> fragment_randomly(Opcode opcode, const std::string& payload, std::mt19937& rng) {
    std::vector<Spec> specs;
    std::size_t pos = 0;
    bool first = true;
    do {
        const std::size_t remaining = payload.size() - pos;
        std::size_t take = 0;
        if (remaining > 0) {
            take = rng() % 3 == 0 ? remaining : 1 + rng() % remaining;
        }
        pos += take;
        specs.push_back({first ? opcode : Opcode::Continuation,
                         pos == payload.size(),
                         payload.substr(pos - take, take)});
        first = false;
        if (rng() % 4 == 0 && pos < payload.size()) {
            specs.push_back({Opcode::Ping, true, "k"});  // an interleaved control frame
        }
    } while (pos < payload.size());
    return specs;
}

TEST(MessageAssembler, AnyFragmentationAndChunkingYieldsTheOriginalMessages) {
    std::mt19937 rng{2718};
    for (int round = 0; round < 300; ++round) {
        std::vector<Spec> all;
        std::vector<Ev> expected;
        for (int m = 0; m < 1 + static_cast<int>(rng() % 3); ++m) {
            const bool text = rng() % 2 == 0;
            const std::string payload =
                text ? random_text(rng) : std::string(rng() % 40, static_cast<char>(rng()));
            for (const Spec& spec :
                 fragment_randomly(text ? Opcode::Text : Opcode::Binary, payload, rng)) {
                all.push_back(spec);
                if (spec.opcode == Opcode::Ping) {
                    expected.push_back(control(Opcode::Ping, "k"));
                }
            }
            expected.push_back(message(text ? MessageType::Text : MessageType::Binary, payload));
        }
        const Collected out = pipeline(wire(all), buffered(), 1 + rng() % 50);
        ASSERT_FALSE(out.failure.has_value()) << round;
        // Compare messages and pings separately (their relative order is covered by the targeted
        // tests).
        std::vector<Ev> got_messages;
        std::vector<Ev> want_messages;
        for (const Ev& e : out.events) {
            if (e.kind == MessageEvent::Kind::Message) {
                got_messages.push_back(e);
            }
        }
        for (const Ev& e : expected) {
            if (e.kind == MessageEvent::Kind::Message) {
                want_messages.push_back(e);
            }
        }
        ASSERT_EQ(got_messages, want_messages) << round;
    }
}

// ---- streaming delivery (docs/FRAGMENTATION.md 1.3)
// -----------------------------------------------------

TEST(MessageAssemblerStreaming, BinaryPartsAreDeliveredAsTheyArriveWithAFinalFlag) {
    const Collected out = pipeline(
        wire({{Opcode::Binary, false, "abcd"}, {Opcode::Continuation, true, "efgh"}}), streaming());
    EXPECT_EQ(out.events,
              (std::vector<Ev>{part(MessageType::Binary, "abcd", false),
                               part(MessageType::Binary, "efgh", true)}));
}

TEST(MessageAssemblerStreaming, ExactlyOneFinalPartEvenForAZeroLengthFinalFrame) {
    const Collected out = pipeline(
        wire({{Opcode::Binary, false, "abc"}, {Opcode::Continuation, true, ""}}), streaming());
    EXPECT_EQ(out.events,
              (std::vector<Ev>{part(MessageType::Binary, "abc", false),
                               part(MessageType::Binary, "", true)}));
    const Collected empty = pipeline(wire({{Opcode::Text, true, ""}}), streaming());
    EXPECT_EQ(empty.events, (std::vector<Ev>{part(MessageType::Text, "", true)}));
}

TEST(MessageAssemblerStreaming, TextPartsEndOnCodePointBoundaries) {  // EC-F29
    // Byte-at-a-time: the euro sign (E2 82 AC) is held back until its last byte arrives.
    const Collected out = pipeline(wire({{Opcode::Text,
                                          true,
                                          "a\xE2\x82\xAC"
                                          "b"}}),
                                   streaming(),
                                   1);
    std::string joined;
    for (const Ev& e : out.events) {
        ASSERT_EQ(e.kind, MessageEvent::Kind::Part);
        EXPECT_TRUE(wsrv::is_valid_utf8(wsrv::as_bytes(e.data)))
            << "a part ends inside a character";
        joined += e.data;
    }
    EXPECT_EQ(joined,
              "a\xE2\x82\xAC"
              "b");
    ASSERT_FALSE(out.events.empty());
    EXPECT_TRUE(out.events.back().final);
    EXPECT_EQ(
        std::count_if(out.events.begin(), out.events.end(), [](const Ev& e) { return e.final; }),
        1);
}

TEST(MessageAssemblerStreaming,
     ByteAtATimeDeliveryHoldsBackOnlyTheUnfinishedCharacter) {  // used by lesson 08
    // "a", euro sign (E2 82 AC), "b", one wire byte per read: the euro is withheld until its last
    // byte.
    const Collected out = pipeline(wire({{Opcode::Text,
                                          true,
                                          "a\xE2\x82\xAC"
                                          "b"}}),
                                   streaming(),
                                   1);
    EXPECT_EQ(out.events,
              (std::vector<Ev>{part(MessageType::Text, "a", false),
                               part(MessageType::Text, "\xE2\x82\xAC", false),
                               part(MessageType::Text, "b", true)}));
}

TEST(MessageAssemblerStreaming,
     ACharacterSplitAcrossFragmentsAndAPingIsHeldAndThenDelivered) {  // EC-F25, F29
    const Collected out = pipeline(wire({{Opcode::Text, false, "x\xE2\x82"},
                                         {Opcode::Ping, true, "p"},
                                         {Opcode::Continuation, true, "\xAC"}}),
                                   streaming());
    EXPECT_EQ(out.events,
              (std::vector<Ev>{part(MessageType::Text, "x", false),
                               control(Opcode::Ping, "p"),
                               part(MessageType::Text, "\xE2\x82\xAC", true)}));
}

TEST(MessageAssemblerStreaming, InvalidUtf8StillFailsAtTheFirstBadByte) {
    const Collected out = pipeline(wire({{Opcode::Text, false, "ok\xFF"}}), streaming());
    ASSERT_TRUE(out.failure.has_value());
    EXPECT_EQ(out.failure->code, CloseCode::InvalidPayload);
    const Collected truncated = pipeline(wire({{Opcode::Text, true, "ok\xE2\x82"}}), streaming());
    ASSERT_TRUE(truncated.failure.has_value());
    EXPECT_EQ(truncated.failure->code, CloseCode::InvalidPayload);
}

TEST(MessageAssemblerStreaming, LimitsStillApply) {
    const Collected big =
        pipeline(wire({{Opcode::Binary, true, std::string(11, 'a')}}), streaming(10));
    ASSERT_TRUE(big.failure.has_value());
    EXPECT_EQ(big.failure->code, CloseCode::MessageTooBig);
    EXPECT_EQ(
        pipeline(wire({{Opcode::Binary, true, std::string(10, 'a')}}), streaming(10)).events.size(),
        1U);
}

TEST(MessageAssemblerStreaming,
     PartsConcatenateToTheBufferedMessageForAnyFragmentationAndChunking) {
    std::mt19937 rng{161803};
    for (int round = 0; round < 300; ++round) {
        const bool text = rng() % 2 == 0;
        const std::string payload =
            text ? random_text(rng) : std::string(rng() % 50, static_cast<char>(rng()));
        std::vector<Spec> specs =
            fragment_randomly(text ? Opcode::Text : Opcode::Binary, payload, rng);
        std::erase_if(specs, [](const Spec& s) { return s.opcode == Opcode::Ping; });
        specs.back().fin = true;
        const std::size_t chunk = 1 + rng() % 40;
        const Collected parts = pipeline(wire(specs), streaming(), chunk);
        ASSERT_FALSE(parts.failure.has_value()) << round;
        std::string joined;
        int finals = 0;
        for (const Ev& e : parts.events) {
            ASSERT_EQ(e.kind, MessageEvent::Kind::Part) << round;
            if (text) {
                ASSERT_TRUE(wsrv::is_valid_utf8(wsrv::as_bytes(e.data)))
                    << round;  // every part decodes on its own
            }
            joined += e.data;
            finals += e.final ? 1 : 0;
        }
        EXPECT_EQ(joined, payload) << round;
        EXPECT_EQ(finals, 1) << round;
        const Collected whole = pipeline(wire(specs), buffered(), chunk);
        ASSERT_EQ(whole.events.size(), 1U);
        EXPECT_EQ(whole.events[0].data, joined) << round;
    }
}

TEST(MessageAssemblerStreaming, AHeldCharacterIsCompletedWithoutCopyingTheChunk) {  // I2, EC-F29
    // The euro sign is split across two fragments; the second fragment arrives as one 1,001-byte
    // read. The completed character must come from a 4-byte internal array and the remainder of
    // the read must be handed on as a view into the caller's own buffer, not copied.
    const auto bytes = wire({{Opcode::Text, false, "x\xE2\x82"},
                             {Opcode::Continuation, true, "\xAC" + std::string(1000, 'y')}});
    wsrv::FrameParser parser;
    wsrv::MessageAssembler assembler{streaming()};
    std::vector<std::byte> buffer = bytes;
    wsrv::MutableByteView input{buffer};
    std::vector<MessageEvent> seen;
    std::vector<std::string> heads;
    for (;;) {
        const auto fe = parser.next(input);
        ASSERT_TRUE(fe.has_value());
        if (!fe->has_value()) {
            break;
        }
        const auto me = assembler.handle(**fe);
        ASSERT_TRUE(me.has_value());
        if (me->has_value()) {
            seen.push_back(**me);
            heads.emplace_back(wsrv::as_chars((*me)->data));
        }
    }
    ASSERT_EQ(seen.size(), 2U);
    EXPECT_EQ(heads[0], "x");
    EXPECT_EQ(heads[1], "\xE2\x82\xAC");  // data: only the completed character
    EXPECT_LE(seen[1].data.size(), 4U);
    EXPECT_EQ(seen[1].tail.size(), 1000U);          // tail: the rest of the read
    EXPECT_GE(seen[1].tail.data(), buffer.data());  // a view into the caller's buffer
    EXPECT_LT(seen[1].tail.data(), buffer.data() + buffer.size());
    EXPECT_TRUE(seen[1].final);
}

}  // namespace

// SPDX-License-Identifier: Apache-2.0
#include "protocol/frame_parser.hpp"

#include "base/bytes.hpp"
#include "protocol/frame_writer.hpp"
#include "protocol/masker.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace {

using wsrv::CloseCode;
using wsrv::FrameHeader;
using wsrv::FrameParser;
using wsrv::FrameParserConfig;
using wsrv::Opcode;

struct Frame {
    FrameHeader header;
    std::string payload;
};

struct Outcome {
    std::vector<Frame> frames;
    std::optional<wsrv::ProtocolFailure> failure;
    bool mid_frame = false;  // a Header was seen without its End
    std::size_t leftover =
        0;  // bytes the parser did not consume (only meaningful without a failure)
};

std::vector<std::byte> to_bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> out;
    for (const unsigned v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

std::vector<std::byte> concat(std::initializer_list<std::vector<std::byte>> parts) {
    std::vector<std::byte> out;
    for (const auto& part : parts) {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

std::vector<std::byte> text_bytes(std::string_view text) {
    const auto view = wsrv::as_bytes(text);
    return {view.begin(), view.end()};
}

/// Feed `bytes` to a fresh parser in chunks of `chunk` bytes (0 = all at once) and collect the
/// frames.
Outcome
run(const std::vector<std::byte>& bytes, FrameParserConfig config = {}, std::size_t chunk = 0) {
    FrameParser parser{config};
    Outcome out;
    Frame current;
    const std::size_t step = chunk == 0 ? std::max<std::size_t>(bytes.size(), 1) : chunk;
    for (std::size_t pos = 0; pos < bytes.size(); pos += step) {
        const std::size_t take = std::min(step, bytes.size() - pos);
        std::vector<std::byte> buffer{bytes.begin() + static_cast<std::ptrdiff_t>(pos),
                                      bytes.begin() + static_cast<std::ptrdiff_t>(pos + take)};
        wsrv::MutableByteView input{buffer};
        for (;;) {
            const auto event = parser.next(input);
            if (!event) {
                out.failure = event.error();
                return out;
            }
            if (!event->has_value()) {
                break;
            }
            switch ((*event)->kind) {
            case wsrv::FrameEvent::Kind::Header:
                current = Frame{(*event)->header, {}};
                out.mid_frame = true;
                break;
            case wsrv::FrameEvent::Kind::Payload:
                current.payload += std::string{wsrv::as_chars((*event)->payload)};
                break;
            case wsrv::FrameEvent::Kind::End:
                out.frames.push_back(current);
                out.mid_frame = false;
                break;
            }
        }
        out.leftover += input.size();
    }
    return out;
}

FrameParserConfig server_side() {  // parses client frames: must be masked
    return FrameParserConfig{};
}

FrameParserConfig client_side() {  // parses server frames: must NOT be masked
    FrameParserConfig c;
    c.expect_masked = false;
    return c;
}

constexpr unsigned kM0 = 0x37, kM1 = 0xfa, kM2 = 0x21, kM3 = 0x3d;

// ---- RFC 6455 section 5.7 examples
// -------------------------------------------------------------------

TEST(FrameParser, MaskedHelloFromTheRfc) {
    const Outcome out = run(
        to_bytes({0x81, 0x85, kM0, kM1, kM2, kM3, 0x7f, 0x9f, 0x4d, 0x51, 0x58}), server_side());
    ASSERT_FALSE(out.failure.has_value());
    ASSERT_EQ(out.frames.size(), 1U);
    const FrameHeader& h = out.frames[0].header;
    EXPECT_TRUE(h.fin);
    EXPECT_EQ(h.opcode, Opcode::Text);
    EXPECT_TRUE(h.masked);
    EXPECT_EQ(h.payload_length, 5U);
    EXPECT_EQ(std::to_integer<unsigned>(h.mask_key[0]), kM0);
    EXPECT_EQ(out.frames[0].payload, "Hello");
}

TEST(FrameParser, UnmaskedHelloFromTheRfc) {
    const Outcome out = run(to_bytes({0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f}), client_side());
    ASSERT_EQ(out.frames.size(), 1U);
    EXPECT_FALSE(out.frames[0].header.masked);
    EXPECT_EQ(out.frames[0].payload, "Hello");
}

TEST(FrameParser, PingAndPongFromTheRfc) {
    const Outcome ping = run(to_bytes({0x89, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f}), client_side());
    ASSERT_EQ(ping.frames.size(), 1U);
    EXPECT_EQ(ping.frames[0].header.opcode, Opcode::Ping);
    EXPECT_EQ(ping.frames[0].payload, "Hello");
    const Outcome pong = run(
        to_bytes({0x8a, 0x85, kM0, kM1, kM2, kM3, 0x7f, 0x9f, 0x4d, 0x51, 0x58}), server_side());
    ASSERT_EQ(pong.frames.size(), 1U);
    EXPECT_EQ(pong.frames[0].header.opcode, Opcode::Pong);
    EXPECT_EQ(pong.frames[0].payload, "Hello");
}

TEST(FrameParser, ExtendedLengthsFromTheRfc) {
    std::vector<std::byte> f256 = to_bytes({0x82, 0x7E, 0x01, 0x00});
    f256.resize(f256.size() + 256, std::byte{0xAB});
    const Outcome a = run(f256, client_side());
    ASSERT_EQ(a.frames.size(), 1U);
    EXPECT_EQ(a.frames[0].header.payload_length, 256U);
    EXPECT_EQ(a.frames[0].payload.size(), 256U);

    std::vector<std::byte> f64k = to_bytes({0x82, 0x7F, 0, 0, 0, 0, 0, 0x01, 0x00, 0x00});
    f64k.resize(f64k.size() + 65536, std::byte{0xCD});
    const Outcome b = run(f64k, client_side());
    ASSERT_EQ(b.frames.size(), 1U);
    EXPECT_EQ(b.frames[0].header.payload_length, 65536U);
    EXPECT_EQ(b.frames[0].payload.size(), 65536U);
}

// ---- EC-F01 .. EC-F08: header violations
// -------------------------------------------------------------

std::optional<CloseCode> failure_of(std::initializer_list<unsigned> bytes,
                                    FrameParserConfig config = {}) {
    const Outcome out = run(to_bytes(bytes), config);
    return out.failure ? std::optional<CloseCode>{out.failure->code} : std::nullopt;
}

TEST(FrameParser, ReservedBitsWithoutAnExtensionAreAProtocolError) {            // EC-F01
    EXPECT_EQ(failure_of({0xC1, 0x80, 0, 0, 0, 0}), CloseCode::ProtocolError);  // RSV1
    EXPECT_EQ(failure_of({0xA1, 0x80, 0, 0, 0, 0}), CloseCode::ProtocolError);  // RSV2
    EXPECT_EQ(failure_of({0x91, 0x80, 0, 0, 0, 0}), CloseCode::ProtocolError);  // RSV3
}

TEST(FrameParser,
     Rsv1IsAllowedOnlyWhenNegotiatedAndNeverOnControlOrContinuationFrames) {  // EC-F01, EC-F24
    FrameParserConfig deflate;
    deflate.allow_rsv1 = true;
    EXPECT_EQ(failure_of({0xC1, 0x80, 0, 0, 0, 0}, deflate), std::nullopt);  // first text frame: ok
    EXPECT_EQ(failure_of({0xC2, 0x80, 0, 0, 0, 0}, deflate),
              std::nullopt);  // first binary frame: ok
    EXPECT_EQ(failure_of({0xC0, 0x80, 0, 0, 0, 0}, deflate),
              CloseCode::ProtocolError);  // continuation
    EXPECT_EQ(failure_of({0xC9, 0x80, 0, 0, 0, 0}, deflate), CloseCode::ProtocolError);  // ping
    EXPECT_EQ(failure_of({0xC8, 0x80, 0, 0, 0, 0}, deflate), CloseCode::ProtocolError);  // close
    EXPECT_EQ(failure_of({0xA1, 0x80, 0, 0, 0, 0}, deflate),
              CloseCode::ProtocolError);  // RSV2 still never
}

TEST(FrameParser, ReservedOpcodesAreAProtocolError) {  // EC-F02
    for (const unsigned opcode : {3U, 4U, 5U, 6U, 7U, 0xBU, 0xCU, 0xDU, 0xEU, 0xFU}) {
        EXPECT_EQ(failure_of({0x80U | opcode, 0x80, 0, 0, 0, 0}), CloseCode::ProtocolError)
            << opcode;
    }
}

TEST(FrameParser, ClientFramesMustBeMaskedAndServerFramesMustNot) {  // EC-F03
    EXPECT_EQ(failure_of({0x81, 0x05, 'H', 'e', 'l', 'l', 'o'}),
              CloseCode::ProtocolError);  // unmasked, server side
    EXPECT_EQ(failure_of({0x81, 0x85, 0, 0, 0, 0, 'H', 'e', 'l', 'l', 'o'}, client_side()),
              CloseCode::ProtocolError);  // masked, client side
}

TEST(FrameParser, LengthEncodingMustBeMinimal) {  // EC-F04
    EXPECT_EQ(failure_of({0x82, 0xFE, 0x00, 0x7D, 0, 0, 0, 0}),
              CloseCode::ProtocolError);  // 125 in the 16-bit form
    EXPECT_EQ(failure_of({0x82, 0xFE, 0x00, 0x00, 0, 0, 0, 0}),
              CloseCode::ProtocolError);  // 0 in the 16-bit form
    EXPECT_EQ(failure_of({0x82, 0xFF, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0, 0, 0, 0}),
              CloseCode::ProtocolError);  // 65535 in the 64-bit form
    EXPECT_EQ(failure_of({0x82, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0x7D, 0, 0, 0, 0}),
              CloseCode::ProtocolError);  // 125 in the 64-bit form
    // The smallest value each form may carry is accepted (header level; payload not needed to
    // succeed).
    FrameParserConfig big;
    big.max_frame_payload = 1 << 20;
    EXPECT_EQ(failure_of({0x82, 0xFE, 0x00, 0x7E, 0, 0, 0, 0}, big), std::nullopt);
    EXPECT_EQ(failure_of({0x82, 0xFF, 0, 0, 0, 0, 0, 0x01, 0x00, 0x00, 0, 0, 0, 0}, big),
              std::nullopt);
}

TEST(FrameParser, TheTopBitOfA64BitLengthMustBeZero) {  // EC-F05
    EXPECT_EQ(failure_of({0x82, 0xFF, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}),
              CloseCode::ProtocolError);
    EXPECT_EQ(failure_of({0x82, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0}),
              CloseCode::ProtocolError);
}

TEST(FrameParser, DeclaredLengthAboveTheLimitIsRejectedAtOnceWithoutAnyPayload) {  // EC-F06, EC-F22
    // 2^40 bytes declared: 1009 as soon as the header is complete; not a single payload byte was
    // needed.
    EXPECT_EQ(failure_of({0x82, 0xFF, 0, 0, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0}),
              CloseCode::MessageTooBig);
    EXPECT_EQ(failure_of({0x82, 0xFF, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0}),
              CloseCode::MessageTooBig);  // 2^63 - 1
}

TEST(FrameParser, TheFrameLimitIsExact) {
    FrameParserConfig small;
    small.expect_masked = false;
    small.max_frame_payload = 200;
    EXPECT_EQ(failure_of({0x82, 0x7E, 0x00, 200}, small), std::nullopt);
    EXPECT_EQ(failure_of({0x82, 0x7E, 0x00, 201}, small), CloseCode::MessageTooBig);
}

TEST(FrameParser, ControlFramesLongerThan125BytesAreAProtocolError) {  // EC-F07
    EXPECT_EQ(failure_of({0x89, 0xFE, 0x00, 0x7E, 0, 0, 0, 0}),
              CloseCode::ProtocolError);  // ping, 16-bit length
    EXPECT_EQ(failure_of({0x88, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0x7E, 0, 0, 0, 0}),
              CloseCode::ProtocolError);
    EXPECT_EQ(failure_of({0x8A, 0xFE}),
              CloseCode::ProtocolError);  // pong: failing needs only the first two bytes
    EXPECT_EQ(failure_of({0x89, 0xFD, 0, 0, 0, 0}), std::nullopt);  // exactly 125 is fine
}

TEST(FrameParser, ControlFramesMustNotBeFragmented) {  // EC-F08
    for (const unsigned opcode : {0x8U, 0x9U, 0xAU}) {
        EXPECT_EQ(failure_of({opcode, 0x80, 0, 0, 0, 0}), CloseCode::ProtocolError)
            << opcode;  // FIN = 0
    }
}

TEST(FrameParser, FailuresNeedOnlyTheBytesThatProveThem) {
    // A reserved opcode is detected after the first two bytes, with no mask key or payload on the
    // wire yet.
    FrameParser parser;
    auto bytes = to_bytes({0x83, 0x80});
    wsrv::MutableByteView input{bytes};
    const auto result = parser.next(input);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, CloseCode::ProtocolError);
}

// ---- EC-F12 .. EC-F17: payload handling and chunking
// -----------------------------------------------------

TEST(FrameParser, ZeroLengthFramesOfEveryKindAreValid) {  // EC-F12
    for (const unsigned first : {0x81U, 0x82U, 0x80U, 0x89U, 0x8AU, 0x88U}) {
        const Outcome out = run(to_bytes({first, 0x80, kM0, kM1, kM2, kM3}));
        ASSERT_FALSE(out.failure.has_value()) << first;
        ASSERT_EQ(out.frames.size(), 1U) << first;
        EXPECT_EQ(out.frames[0].header.payload_length, 0U);
        EXPECT_TRUE(out.frames[0].payload.empty());
    }
}

std::vector<std::byte>
masked_frame(Opcode opcode, bool fin, std::string_view payload, wsrv::MaskKey key) {
    FrameHeader h;
    h.fin = fin;
    h.opcode = opcode;
    h.masked = true;
    h.payload_length = payload.size();
    h.mask_key = key;
    std::array<std::byte, wsrv::kMaxHeaderSize> header{};
    const std::size_t n = *wsrv::encode_header(header, h);
    std::vector<std::byte> out(header.begin(), header.begin() + static_cast<std::ptrdiff_t>(n));
    std::vector<std::byte> body = text_bytes(payload);
    wsrv::apply_mask(body, key);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

TEST(FrameParser, EverySplitPointAndByteAtATimeGiveTheSameFrames) {  // EC-F13, F14, F16, F17
    const wsrv::MaskKey key{std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}};
    const std::vector<std::byte> stream = concat(
        {masked_frame(Opcode::Text, false, "fragment one, ", key),
         masked_frame(Opcode::Ping, true, "ping!", key),
         masked_frame(Opcode::Continuation, true, std::string(300, 'z'), key),  // 16-bit length
         masked_frame(Opcode::Binary, true, "", key)});
    const Outcome whole = run(stream);
    ASSERT_FALSE(whole.failure.has_value());
    ASSERT_EQ(whole.frames.size(), 4U);
    EXPECT_EQ(whole.frames[0].payload, "fragment one, ");
    EXPECT_EQ(whole.frames[2].payload, std::string(300, 'z'));

    auto expect_same = [&](const Outcome& other, const std::string& what) {
        ASSERT_FALSE(other.failure.has_value()) << what;
        ASSERT_EQ(other.frames.size(), whole.frames.size()) << what;
        for (std::size_t i = 0; i < whole.frames.size(); ++i) {
            ASSERT_EQ(other.frames[i].header, whole.frames[i].header) << what << " frame " << i;
            ASSERT_EQ(other.frames[i].payload, whole.frames[i].payload) << what << " frame " << i;
        }
    };
    expect_same(run(stream, {}, 1), "byte at a time");
    for (std::size_t chunk = 2; chunk <= 41; ++chunk) {
        expect_same(run(stream, {}, chunk), "chunk " + std::to_string(chunk));
    }
    for (std::size_t split = 1; split < stream.size(); ++split) {
        const std::vector<std::byte> a{stream.begin(),
                                       stream.begin() + static_cast<std::ptrdiff_t>(split)};
        const std::vector<std::byte> b{stream.begin() + static_cast<std::ptrdiff_t>(split),
                                       stream.end()};
        FrameParser parser;
        std::vector<Frame> frames;
        Frame current;
        for (const auto* part : {&a, &b}) {
            std::vector<std::byte> copy = *part;
            wsrv::MutableByteView input{copy};
            for (;;) {
                const auto event = parser.next(input);
                ASSERT_TRUE(event.has_value()) << split;
                if (!event->has_value()) {
                    break;
                }
                if ((*event)->kind == wsrv::FrameEvent::Kind::Header) {
                    current = Frame{(*event)->header, {}};
                } else if ((*event)->kind == wsrv::FrameEvent::Kind::Payload) {
                    current.payload += std::string{wsrv::as_chars((*event)->payload)};
                } else {
                    frames.push_back(current);
                }
            }
        }
        ASSERT_EQ(frames.size(), whole.frames.size()) << "split " << split;
        for (std::size_t i = 0; i < frames.size(); ++i) {
            ASSERT_EQ(frames[i].payload, whole.frames[i].payload)
                << "split " << split << " frame " << i;
        }
    }
}

TEST(FrameParser, ManyFramesInOneReadAreAllDelivered) {  // EC-F15
    const wsrv::MaskKey key{};                           // zero key: EC-F16
    std::vector<std::byte> stream;
    for (int i = 0; i < 50; ++i) {
        const auto f = masked_frame(Opcode::Text, true, "msg" + std::to_string(i), key);
        stream.insert(stream.end(), f.begin(), f.end());
    }
    const Outcome out = run(stream);
    ASSERT_EQ(out.frames.size(), 50U);
    EXPECT_EQ(out.frames[49].payload, "msg49");
    EXPECT_EQ(out.leftover, 0U);
}

TEST(FrameParser, UnmasksInPlaceAndHandsOutViewsIntoTheCallersBuffer) {
    std::vector<std::byte> buffer = concat(
        {to_bytes({0x81, 0x85, kM0, kM1, kM2, kM3}), to_bytes({0x7f, 0x9f, 0x4d, 0x51, 0x58})});
    wsrv::MutableByteView input{buffer};
    FrameParser parser;
    ASSERT_TRUE(parser.next(input).value().has_value());  // Header
    const auto payload = parser.next(input);
    ASSERT_TRUE(payload.has_value() && payload->has_value());
    EXPECT_EQ((*payload)->kind, wsrv::FrameEvent::Kind::Payload);
    EXPECT_GE((*payload)->payload.data(), buffer.data());
    EXPECT_LT((*payload)->payload.data(), buffer.data() + buffer.size());
    EXPECT_EQ(wsrv::as_chars((*payload)->payload), "Hello");
    EXPECT_EQ(wsrv::as_chars(wsrv::ByteView{buffer}.subspan(6)),
              "Hello");  // the caller's buffer itself was unmasked
}

TEST(FrameParser, BytesBeyondTheFrameStayInTheInput) {
    std::vector<std::byte> buffer =
        concat({masked_frame(Opcode::Text, true, "ab", wsrv::MaskKey{}), to_bytes({0x81, 0x80})});
    wsrv::MutableByteView input{buffer};
    FrameParser parser;
    int events = 0;
    while (true) {
        const auto event = parser.next(input);
        ASSERT_TRUE(event.has_value());
        if (!event->has_value()) {
            break;
        }
        ++events;
    }
    EXPECT_EQ(events, 3);  // Header, Payload, End
    EXPECT_EQ(input.size(),
              0U);  // the next frame's first two bytes were consumed into its header buffer
}

TEST(FrameParser, ADeclaredButUndeliveredHugeFrameCostsNothing) {  // EC-F22
    FrameParserConfig config;
    config.expect_masked = false;
    config.max_frame_payload = 1ULL << 40;
    const Outcome out = run(to_bytes({0x82, 0x7F, 0, 0, 0x01, 0, 0, 0, 0, 0}), config);
    EXPECT_FALSE(out.failure.has_value());
    EXPECT_TRUE(out.frames.empty());
    EXPECT_TRUE(
        out.mid_frame);  // header accepted, payload still pending; no buffer was allocated for it
}

TEST(FrameParser, StaysFailedUntilReset) {
    FrameParser parser;
    auto bad = to_bytes({0x83, 0x80, 0, 0, 0, 0});
    wsrv::MutableByteView in1{bad};
    ASSERT_FALSE(parser.next(in1).has_value());
    EXPECT_TRUE(parser.failed());
    auto good = masked_frame(Opcode::Text, true, "ok", wsrv::MaskKey{});
    wsrv::MutableByteView in2{good};
    EXPECT_FALSE(parser.next(in2).has_value());
    parser.reset();
    EXPECT_FALSE(parser.failed());
    wsrv::MutableByteView in3{good};
    const auto event = parser.next(in3);
    ASSERT_TRUE(event.has_value() && event->has_value());
}

// ---- round trip: encoder -> parser
// ------------------------------------------------------------------------

Opcode random_opcode(std::mt19937& rng, bool control) {
    if (control) {
        return rng() % 2 == 0 ? Opcode::Ping : Opcode::Pong;
    }
    constexpr std::array<Opcode, 3> kData{Opcode::Binary, Opcode::Text, Opcode::Continuation};
    return kData[rng() % kData.size()];
}

std::size_t random_length(std::mt19937& rng, bool control) {
    if (control) {
        return rng() % 126;
    }
    return rng() % 5 == 0 ? rng() % 70000 : rng() % 300;
}

TEST(FrameParser, RandomFramesSurviveEncodeThenParseUnderRandomChunking) {
    std::mt19937 rng{314159};
    for (int round = 0; round < 500; ++round) {
        std::vector<std::byte> stream;
        std::vector<std::pair<FrameHeader, std::string>> expected;
        const int frames = 1 + static_cast<int>(rng() % 5);
        for (int i = 0; i < frames; ++i) {
            const bool control = rng() % 4 == 0;
            FrameHeader h;
            h.opcode = random_opcode(rng, control);
            h.fin = control || rng() % 2 == 0;
            h.masked = true;
            std::string payload(random_length(rng, control), '\0');
            for (char& c : payload) {
                c = static_cast<char>(rng() & 0xFFU);
            }
            h.payload_length = payload.size();
            for (auto& b : h.mask_key) {
                b = static_cast<std::byte>(rng() & 0xFFU);
            }
            const auto frame = masked_frame(h.opcode, h.fin, payload, h.mask_key);
            stream.insert(stream.end(), frame.begin(), frame.end());
            expected.emplace_back(h, payload);
        }
        FrameParserConfig config;
        config.max_frame_payload = 1 << 20;
        const Outcome out = run(stream, config, 1 + rng() % 200);
        ASSERT_FALSE(out.failure.has_value()) << round;
        ASSERT_EQ(out.frames.size(), expected.size()) << round;
        for (std::size_t i = 0; i < expected.size(); ++i) {
            ASSERT_EQ(out.frames[i].header, expected[i].first) << round << "/" << i;
            ASSERT_EQ(out.frames[i].payload, expected[i].second) << round << "/" << i;
        }
    }
}

}  // namespace

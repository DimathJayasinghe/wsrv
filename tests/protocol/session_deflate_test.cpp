// SPDX-License-Identifier: Apache-2.0
// permessage-deflate inside ProtocolSession (RFC 7692, ADR-0020 sections 4 and 5): RSV1 handling,
// inflate limits, compression of outbound messages. The peer's codecs are the real zlib ones.
#include "extensions/zlib_codec.hpp"
#include "protocol/session.hpp"
#include "support/peer_deflate.hpp"
#include "support/wire.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {

using wsrv::CloseCode;
using wsrv::DeflateParams;
using wsrv::ErrorCode;
using wsrv::MessageType;
using wsrv::Opcode;
using wsrv::ProtocolSession;
using wsrv::SessionConfig;
using wsrv::SessionDeflate;
using wsrv::SessionEvent;
using wsrv::test::client_frame;
using wsrv::test::parse_server_output;
using wsrv::test::PeerCompressor;
using wsrv::test::PeerDecompressor;
using wsrv::test::WireFrame;
using Kind = SessionEvent::Kind;

std::string text_of(std::size_t size) {
    std::string text;
    while (text.size() < size) {
        text += R"({"id":42,"name":"alice","tags":["a","b","c"],"active":true},)";
    }
    text.resize(size);
    return text;
}

/// Text that compresses only about 2:1 (random letters), so honest tests stay under the ratio
/// limit.
std::string varied_text(std::size_t size) {
    std::string text(size, ' ');
    std::uint32_t state = 2463534242U;
    for (char& c : text) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        c = static_cast<char>('a' + (state % 16U));
    }
    return text;
}

struct Seen {
    Kind kind;
    std::string data;
    bool final = false;
    unsigned code = 0;
    std::string reason;
};

SessionConfig deflate_config(bool server_no_context_takeover = false) {
    SessionConfig config;
    SessionDeflate deflate;
    deflate.params.server_no_context_takeover = server_no_context_takeover;
    deflate.codecs = std::make_shared<wsrv::ZlibCodecFactory>(wsrv::ZlibSettings{});
    deflate.min_compress_size = 64;
    config.deflate = deflate;
    return config;
}

class Rig {
public:
    explicit Rig(SessionConfig config = deflate_config()) : session_{std::move(config), start_} {}

    ProtocolSession& session() {
        return session_;
    }

    std::vector<Seen> feed(std::vector<std::byte> bytes, std::size_t chunk = 0) {
        std::vector<Seen> seen;
        const std::size_t step = chunk == 0 ? std::max<std::size_t>(bytes.size(), 1) : chunk;
        for (std::size_t pos = 0; pos < bytes.size(); pos += step) {
            const std::size_t take = std::min(step, bytes.size() - pos);
            std::vector<std::byte> buffer{bytes.begin() + static_cast<std::ptrdiff_t>(pos),
                                          bytes.begin() + static_cast<std::ptrdiff_t>(pos + take)};
            wsrv::MutableByteView input{buffer};
            while (const auto event = session_.next_event(input)) {
                seen.push_back(Seen{event->kind,
                                    std::string{wsrv::as_chars(event->data)} +
                                        std::string{wsrv::as_chars(event->tail)},
                                    event->final,
                                    static_cast<unsigned>(event->code),
                                    std::string{event->reason}});
            }
            EXPECT_TRUE(input.empty());
        }
        return seen;
    }

    wsrv::test::ParsedOutput take_output() {
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

private:
    wsrv::TimePoint start_;
    ProtocolSession session_;
};

std::vector<std::byte> cat(std::initializer_list<std::vector<std::byte>> parts) {
    std::vector<std::byte> out;
    for (const auto& p : parts) {
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

/// The Seen events other than ProtocolError / Closed bookkeeping, as "text" messages.
std::vector<std::string> messages(const std::vector<Seen>& seen) {
    std::vector<std::string> out;
    for (const Seen& s : seen) {
        if (s.kind == Kind::Message) {
            out.push_back(s.data);
        }
    }
    return out;
}

const Seen* error_event(const std::vector<Seen>& seen) {
    for (const Seen& s : seen) {
        if (s.kind == Kind::ProtocolError) {
            return &s;
        }
    }
    return nullptr;
}

// ---- receive

TEST(SessionDeflate, ACompressedMessageIsDeliveredInflated) {
    PeerCompressor peer;
    Rig rig;
    const std::string text = text_of(3000);
    auto seen = rig.feed(client_frame(Opcode::Text, true, peer.compress(text), true));
    EXPECT_EQ(messages(seen), (std::vector<std::string>{text}));
    EXPECT_EQ(error_event(seen), nullptr);
}

TEST(SessionDeflate, ACompressedMessageInManyFragmentsWithAPingBetweenIsOneMessage) {  // EC-D03
    PeerCompressor peer;
    Rig rig;
    const std::string text = text_of(5000);
    const std::string compressed = peer.compress(text);
    const std::size_t third = compressed.size() / 3;
    auto seen =
        rig.feed(cat({client_frame(Opcode::Text, false, compressed.substr(0, third), true),
                      client_frame(Opcode::Ping, true, "hi"),
                      client_frame(Opcode::Continuation, false, compressed.substr(third, third)),
                      client_frame(Opcode::Continuation, true, compressed.substr(2 * third))}));
    ASSERT_EQ(error_event(seen), nullptr);
    EXPECT_EQ(messages(seen), (std::vector<std::string>{text}));
    ASSERT_GE(seen.size(), 2U);
    EXPECT_EQ(seen.front().kind, Kind::Ping);  // the Ping surfaced between the fragments
    const auto out = rig.take_output();
    ASSERT_EQ(out.frames.size(), 1U);
    EXPECT_EQ(out.frames[0], (WireFrame{Opcode::Pong, true, "hi", false}));
}

TEST(SessionDeflate, ContextTakeoverCarriesAcrossMessages) {
    PeerCompressor peer;
    Rig rig;
    const std::string text = text_of(2000);
    (void)rig.feed(client_frame(Opcode::Text, true, peer.compress(text), true));
    auto seen =
        rig.feed(client_frame(Opcode::Text, true, peer.compress(text), true));  // tiny, refers back
    EXPECT_EQ(messages(seen), (std::vector<std::string>{text}));
}

TEST(SessionDeflate, AnUncompressedMessageIsStillAcceptedWhenTheExtensionIsOn) {  // EC-D07
    Rig rig;
    auto seen = rig.feed(client_frame(Opcode::Text, true, "plain text"));
    EXPECT_EQ(messages(seen), (std::vector<std::string>{"plain text"}));
}

TEST(SessionDeflate, ByteAtATimeInputGivesTheSameMessage) {
    PeerCompressor peer;
    Rig rig;
    const std::string text = text_of(4000);
    const std::string compressed = peer.compress(text);
    auto seen = rig.feed(
        cat({client_frame(Opcode::Text, false, compressed.substr(0, compressed.size() / 2), true),
             client_frame(Opcode::Continuation, true, compressed.substr(compressed.size() / 2))}),
        1);
    EXPECT_EQ(messages(seen), (std::vector<std::string>{text}));
}

TEST(SessionDeflate, RsvOneIsAProtocolErrorWhenTheExtensionWasNotNegotiated) {
    Rig rig{SessionConfig{}};
    auto seen = rig.feed(client_frame(Opcode::Text, true, "x", true));
    const Seen* error = error_event(seen);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(error->code, static_cast<unsigned>(CloseCode::ProtocolError));
}

TEST(SessionDeflate, RsvOneOnAContinuationFrameIsAProtocolError) {  // EC-D04
    PeerCompressor peer;
    Rig rig;
    const std::string compressed = peer.compress(text_of(500));
    auto seen =
        rig.feed(cat({client_frame(Opcode::Text, false, compressed.substr(0, 10), true),
                      client_frame(Opcode::Continuation, true, compressed.substr(10), true)}));
    const Seen* error = error_event(seen);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(error->code, static_cast<unsigned>(CloseCode::ProtocolError));
}

TEST(SessionDeflate, RsvOneOnAControlFrameIsAProtocolError) {  // EC-D04
    Rig rig;
    auto seen = rig.feed(client_frame(Opcode::Ping, true, "", true));
    const Seen* error = error_event(seen);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(error->code, static_cast<unsigned>(CloseCode::ProtocolError));
}

TEST(SessionDeflate, InvalidDeflateDataClosesWith1007) {  // EC-D05
    Rig rig;
    auto seen = rig.feed(client_frame(Opcode::Text, true, std::string(40, '\xff'), true));
    const Seen* error = error_event(seen);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(error->code, static_cast<unsigned>(CloseCode::InvalidPayload));
}

TEST(SessionDeflate, TextThatInflatesToInvalidUtf8ClosesWith1007) {
    PeerCompressor peer;
    Rig rig;
    auto seen = rig.feed(
        client_frame(Opcode::Text, true, peer.compress(std::string("ok \xff\xfe bad")), true));
    const Seen* error = error_event(seen);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(error->code, static_cast<unsigned>(CloseCode::InvalidPayload));
}

TEST(SessionDeflate, TheSizeLimitAppliesToTheInflatedMessage) {  // SECURITY_DESIGN T6
    PeerCompressor peer;
    SessionConfig config = deflate_config();
    config.assembler.max_message_size = 1U << 20;
    config.parser.max_frame_payload = 4U << 20U;  // the compressed frame itself is nearly 1 MiB
    Rig rig{config};
    const std::string big = varied_text(2U << 20U);
    auto seen = rig.feed(client_frame(Opcode::Binary, true, peer.compress(big), true));
    const Seen* error = error_event(seen);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(error->code, static_cast<unsigned>(CloseCode::MessageTooBig));
    EXPECT_EQ(error->reason, "message exceeds the size limit");
}

TEST(SessionDeflate, TheRatioLimitStopsABombBeforeTheSizeLimit) {  // SECURITY_DESIGN T6
    PeerCompressor peer;
    SessionConfig config = deflate_config();
    config.assembler.max_message_size =
        wsrv::kMaxConfiguredMessage;  // 1 GiB: only the ratio can stop it
    Rig rig{config};
    const std::string zeros(32U << 20U, '\0');  // about 32 KiB compressed: a ratio of about 1000
    const std::string compressed = peer.compress(zeros);
    ASSERT_LT(compressed.size() * 200, zeros.size());
    auto seen = rig.feed(client_frame(Opcode::Binary, true, compressed, true));
    const Seen* error = error_event(seen);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(error->code, static_cast<unsigned>(CloseCode::MessageTooBig));
    EXPECT_EQ(error->reason, "inflated size out of proportion to the compressed size");
}

TEST(SessionDeflate, AWellCompressedButHonestMessageStaysUnderTheRatio) {
    PeerCompressor peer;
    Rig rig;
    const std::string text = varied_text(60000);
    auto seen = rig.feed(client_frame(Opcode::Text, true, peer.compress(text), true));
    EXPECT_EQ(messages(seen), (std::vector<std::string>{text}));
}

TEST(SessionDeflate, StreamingModeDeliversInflatedParts) {
    PeerCompressor peer;
    SessionConfig config = deflate_config();
    config.assembler.streaming = true;
    Rig rig{config};
    const std::string text = varied_text(50000);
    auto seen = rig.feed(client_frame(Opcode::Text, true, peer.compress(text), true));
    std::string joined;
    bool saw_final = false;
    for (const Seen& s : seen) {
        ASSERT_EQ(s.kind, Kind::Part);
        joined += s.data;
        saw_final = saw_final || s.final;
    }
    EXPECT_EQ(joined, text);
    EXPECT_TRUE(saw_final);
}

TEST(SessionDeflate, AnEmptyCompressedMessageIsDelivered) {  // EC-D06
    Rig rig;
    auto seen = rig.feed(client_frame(Opcode::Binary, true, std::string(1, '\0'), true));
    EXPECT_EQ(messages(seen), (std::vector<std::string>{""}));
}

TEST(SessionDeflate, ACloseDuringACompressedMessageStillEndsTheSessionCleanly) {
    PeerCompressor peer;
    Rig rig;
    const std::string compressed = peer.compress(text_of(2000));
    auto seen = rig.feed(cat({client_frame(Opcode::Text, false, compressed.substr(0, 20), true),
                              client_frame(Opcode::Close, true, wsrv::test::close_payload(1000))}));
    EXPECT_EQ(error_event(seen), nullptr);
    EXPECT_TRUE(messages(seen).empty());
    const auto out = rig.take_output();
    ASSERT_EQ(out.frames.size(), 1U);
    EXPECT_EQ(out.frames[0].opcode, Opcode::Close);
}

// ---- send

TEST(SessionDeflate, ALargeMessageIsSentCompressedWithRsvOneOnTheFirstFrameOnly) {  // EC-D02
    SessionConfig config = deflate_config();
    config.outbound.max_frame_payload = 100;  // force fragmentation of the compressed bytes
    Rig rig{config};
    const std::string text = text_of(20000);
    ASSERT_TRUE(rig.session().send_text(text).has_value());
    const auto out = rig.take_output();
    ASSERT_TRUE(out.ok);
    ASSERT_GT(out.frames.size(), 1U);
    std::string joined;
    for (std::size_t i = 0; i < out.frames.size(); ++i) {
        EXPECT_EQ(out.frames[i].rsv1, i == 0) << i;
        EXPECT_EQ(out.frames[i].opcode, i == 0 ? Opcode::Text : Opcode::Continuation);
        EXPECT_EQ(out.frames[i].fin, i + 1 == out.frames.size());
        joined += out.frames[i].payload;
    }
    EXPECT_LT(joined.size(), text.size() / 5);  // really compressed
    EXPECT_EQ(PeerDecompressor{}.decompress(joined), text);
}

TEST(SessionDeflate, ASmallMessageIsSentPlain) {  // EC-D07
    Rig rig;
    ASSERT_TRUE(rig.session().send_text("short").has_value());
    const auto out = rig.take_output();
    ASSERT_EQ(out.frames.size(), 1U);
    EXPECT_EQ(out.frames[0], (WireFrame{Opcode::Text, true, "short", false}));
}

TEST(SessionDeflate, SeveralMessagesDecodeInOrderWithContextTakeover) {
    Rig rig;
    PeerDecompressor peer;
    const std::vector<std::string> texts{text_of(3000), text_of(3000), text_of(900), text_of(7000)};
    for (const std::string& text : texts) {
        ASSERT_TRUE(rig.session().send_text(text).has_value());
    }
    const auto out = rig.take_output();
    ASSERT_EQ(out.frames.size(), texts.size());
    for (std::size_t i = 0; i < texts.size(); ++i) {
        EXPECT_TRUE(out.frames[i].rsv1);
        EXPECT_EQ(peer.decompress(out.frames[i].payload), texts[i]) << i;
    }
}

TEST(SessionDeflate, ABinaryMessageIsCompressedToo) {
    Rig rig;
    const std::string data(5000, 'z');
    ASSERT_TRUE(rig.session().send_binary(wsrv::as_bytes(data)).has_value());
    const auto out = rig.take_output();
    ASSERT_EQ(out.frames.size(), 1U);
    EXPECT_EQ(out.frames[0].opcode, Opcode::Binary);
    EXPECT_TRUE(out.frames[0].rsv1);
    EXPECT_EQ(PeerDecompressor{}.decompress(out.frames[0].payload), data);
}

TEST(SessionDeflate, AMessageRefusedByBackpressureNeverTouchesTheCompressor) {
    SessionConfig config = deflate_config();
    config.outbound.max_queued_bytes = 8000;
    Rig rig{config};
    PeerDecompressor peer;
    const std::string first = varied_text(5000);  // about 3 KiB once compressed
    ASSERT_TRUE(rig.session().send_text(first).has_value());
    // Its worst case no longer fits next to the first message: refused BEFORE the compressor
    // sees it. Had it been compressed and dropped, the peer's history would be one message short.
    const auto refused = rig.session().send_text(varied_text(4800));
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code, ErrorCode::Backpressure);
    auto out = rig.take_output();
    ASSERT_EQ(out.frames.size(), 1U);
    ASSERT_EQ(peer.decompress(out.frames[0].payload), first);

    const std::string third = varied_text(3000);
    ASSERT_TRUE(rig.session().send_text(third).has_value());
    out = rig.take_output();
    ASSERT_EQ(out.frames.size(), 1U);
    EXPECT_EQ(peer.decompress(out.frames[0].payload), third);  // history still in step
}

TEST(SessionDeflate, AMessageTooBigForTheWorstCaseButNotForThePlainQueueGoesOutPlain) {
    SessionConfig config = deflate_config();
    config.outbound.max_queued_bytes = 8000;
    Rig rig{config};
    const std::string text =
        varied_text(7500);  // 7500 fits; its compression worst case (8500+) does not
    ASSERT_TRUE(rig.session().send_text(text).has_value());
    const auto out = rig.take_output();
    ASSERT_EQ(out.frames.size(), 1U);
    EXPECT_FALSE(out.frames[0].rsv1);
    EXPECT_EQ(out.frames[0].payload, text);
}

TEST(SessionDeflate, WithoutNegotiationNothingIsCompressed) {
    Rig rig{SessionConfig{}};
    const std::string text = text_of(5000);
    ASSERT_TRUE(rig.session().send_text(text).has_value());
    const auto out = rig.take_output();
    ASSERT_EQ(out.frames.size(), 1U);
    EXPECT_FALSE(out.frames[0].rsv1);
    EXPECT_EQ(out.frames[0].payload, text);
}

TEST(SessionDeflate, StreamedMessagesAreNotCompressed) {
    Rig rig;
    ASSERT_TRUE(rig.session().begin_message(Opcode::Text).has_value());
    ASSERT_TRUE(rig.session().write_part(wsrv::as_bytes(text_of(1000))).has_value());
    ASSERT_TRUE(rig.session().end_message().has_value());
    const auto out = rig.take_output();
    ASSERT_FALSE(out.frames.empty());
    EXPECT_FALSE(out.frames[0].rsv1);
}

TEST(SessionDeflate, NoContextTakeoverMessagesStandAlone) {
    Rig rig{deflate_config(true)};
    const std::string text = text_of(3000);
    ASSERT_TRUE(rig.session().send_text(text).has_value());
    ASSERT_TRUE(rig.session().send_text(text).has_value());
    const auto out = rig.take_output();
    ASSERT_EQ(out.frames.size(), 2U);
    EXPECT_EQ(out.frames[0].payload, out.frames[1].payload);
    EXPECT_EQ(PeerDecompressor{}.decompress(out.frames[1].payload), text);
}

// ---- configuration

TEST(SessionDeflate, TheConfigurationIsChecked) {
    SessionConfig config = deflate_config();
    EXPECT_TRUE(wsrv::validate(config).has_value());
    SessionConfig no_factory = config;
    no_factory.deflate->codecs = nullptr;
    EXPECT_FALSE(wsrv::validate(no_factory).has_value());
    SessionConfig zero_ratio = config;
    zero_ratio.deflate->max_inflate_ratio = 0;
    EXPECT_FALSE(wsrv::validate(zero_ratio).has_value());
    SessionConfig big_ratio = config;
    big_ratio.deflate->max_inflate_ratio = wsrv::kMaxConfiguredInflateRatio + 1;
    EXPECT_FALSE(wsrv::validate(big_ratio).has_value());
    SessionConfig bad_window = config;
    bad_window.deflate->params.server_window_bits = 8;
    EXPECT_FALSE(wsrv::validate(bad_window).has_value());
}

// RFC 7692 section 7.1.2.2 allows a client window of 8..15 and the negotiation agrees to it (our
// inflater always uses 15 bits, which decodes any window), so the session must accept what the
// negotiation can hand it: a rejected agreement would abort the server at the first handshake.
TEST(SessionDeflate, EveryClientWindowTheNegotiationCanAgreeToIsAValidConfiguration) {
    for (int bits = wsrv::kMinClientWindowBits; bits <= wsrv::kMaxDeflateWindowBits; ++bits) {
        SessionConfig config = deflate_config();
        config.deflate->params.client_window_bits = bits;
        EXPECT_TRUE(wsrv::validate(config).has_value()) << "client window " << bits;
    }
    for (const int bits : {wsrv::kMinClientWindowBits - 1, wsrv::kMaxDeflateWindowBits + 1}) {
        SessionConfig config = deflate_config();
        config.deflate->params.client_window_bits = bits;
        EXPECT_FALSE(wsrv::validate(config).has_value()) << "client window " << bits;
    }
}

TEST(SessionDeflate, ASessionAgreedToAClientWindowOfEightInflatesNormally) {
    SessionConfig config = deflate_config();
    config.deflate->params.client_window_bits = 8;
    PeerCompressor peer;
    Rig rig{config};  // constructing the session asserts validate(config)
    const std::string text = text_of(3000);
    const auto seen = rig.feed(client_frame(wsrv::Opcode::Text, true, peer.compress(text), true));
    EXPECT_EQ(messages(seen), std::vector<std::string>{text});
}

}  // namespace

// SPDX-License-Identifier: Apache-2.0
// zlib codecs for permessage-deflate (RFC 7692 section 7.2, ADR-0020 sections 4 and 5).
#include "extensions/zlib_codec.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

using wsrv::ByteView;
using wsrv::DeflateCodecs;
using wsrv::DeflateParams;
using wsrv::ErrorCode;
using wsrv::IMessageInflater;
using wsrv::Result;

std::vector<std::byte> bytes_of(std::initializer_list<int> values) {
    std::vector<std::byte> out;
    for (const int v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

std::vector<std::byte> bytes_of(const std::string& text) {
    const ByteView view = wsrv::as_bytes(text);
    return {view.begin(), view.end()};
}

/// Deterministic, hard-to-compress data (xorshift).
std::vector<std::byte> noise(std::size_t size, std::uint32_t seed = 2463534242U) {
    std::vector<std::byte> out(size);
    std::uint32_t state = seed;
    for (std::byte& b : out) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        b = static_cast<std::byte>(state & 0xffU);
    }
    return out;
}

/// Compressible, text-like data.
std::vector<std::byte> text_of(std::size_t size) {
    std::string text;
    while (text.size() < size) {
        text += R"({"id":42,"name":"alice","tags":["a","b","c"],"active":true},)";
    }
    text.resize(size);
    return bytes_of(text);
}

DeflateCodecs make_codecs(const DeflateParams& params, wsrv::ZlibSettings settings = {}) {
    const wsrv::ZlibCodecFactory factory{settings};
    auto codecs = factory.create(params);
    EXPECT_TRUE(codecs.has_value());
    return std::move(*codecs);
}

DeflateParams params(int window = 15, bool server_ncto = false, bool client_ncto = false) {
    DeflateParams p;
    p.server_window_bits = window;
    p.server_no_context_takeover = server_ncto;
    p.client_no_context_takeover = client_ncto;
    return p;
}

std::vector<std::byte> compress(wsrv::IMessageDeflater& deflater, ByteView message) {
    std::vector<std::byte> out;
    const auto result = deflater.deflate(message, out);
    EXPECT_TRUE(result.has_value())
        << (result ? "" : std::string{result.error().context}) << " size " << message.size();
    return out;
}

/// Feed `compressed` in slices of `in_step` bytes and drain through an output buffer of
/// `out_size` bytes, then finish. Returns the inflated bytes or the first error.
Result<std::vector<std::byte>> inflate_all(IMessageInflater& inflater,
                                           ByteView compressed,
                                           std::size_t in_step = static_cast<std::size_t>(-1),
                                           std::size_t out_size = 16 * 1024) {
    std::vector<std::byte> result;
    std::vector<std::byte> buffer(out_size);
    while (!compressed.empty()) {
        ByteView rest = compressed.first(std::min(in_step, compressed.size()));
        compressed = compressed.subspan(rest.size());
        for (;;) {
            auto step = inflater.inflate(rest, buffer);
            if (!step) {
                return std::unexpected{step.error()};
            }
            EXPECT_LE(step->produced, buffer.size());
            result.insert(result.end(),
                          buffer.begin(),
                          buffer.begin() + static_cast<std::ptrdiff_t>(step->produced));
            rest = rest.subspan(step->consumed);
            if (rest.empty() && step->produced < buffer.size()) {
                break;
            }
        }
    }
    for (;;) {
        auto step = inflater.finish(buffer);
        if (!step) {
            return std::unexpected{step.error()};
        }
        result.insert(result.end(),
                      buffer.begin(),
                      buffer.begin() + static_cast<std::ptrdiff_t>(step->produced));
        if (step->done) {
            return result;
        }
    }
}

// ---- the RFC's own examples (section 7.2.3)

TEST(ZlibCodec, HelloCompressesToTheBytesInRfc7692Section7231) {
    auto codecs = make_codecs(params());
    EXPECT_EQ(compress(*codecs.deflater, wsrv::as_bytes("Hello")),
              bytes_of({0xf2, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00}));
}

TEST(ZlibCodec, TheSecondIdenticalMessageIsTinyWithContextTakeover) {  // section 7.2.3.2
    auto codecs = make_codecs(params());
    (void)compress(*codecs.deflater, wsrv::as_bytes("Hello"));
    EXPECT_EQ(compress(*codecs.deflater, wsrv::as_bytes("Hello")),
              bytes_of({0xf2, 0x00, 0x11, 0x00, 0x00}));
}

TEST(ZlibCodec, NoContextTakeoverMakesEveryMessageStandAlone) {
    auto codecs = make_codecs(params(15, true));
    const auto first = compress(*codecs.deflater, wsrv::as_bytes("Hello"));
    const auto second = compress(*codecs.deflater, wsrv::as_bytes("Hello"));
    EXPECT_EQ(first, second);
}

TEST(ZlibCodec, TheRfcExampleInflatesBackToHello) {
    auto codecs = make_codecs(params());
    const auto input = bytes_of({0xf2, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00});
    auto out = inflate_all(*codecs.inflater, input);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, bytes_of("Hello"));
}

TEST(ZlibCodec, AnEmptyMessageIsTheSingleByteZero) {  // EC-D06, section 7.2.3.6
    auto codecs = make_codecs(params());
    auto out = inflate_all(*codecs.inflater, bytes_of({0x00}));
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(out->empty());
    std::vector<std::byte> compressed;
    ASSERT_TRUE(codecs.deflater->deflate({}, compressed).has_value());
    EXPECT_EQ(compressed, bytes_of({0x00}));
}

// ---- round trips

TEST(ZlibCodec, EveryWindowAndTakeoverModeRoundTripsSeveralMessages) {
    const std::vector<std::vector<std::byte>> messages{
        bytes_of("x"), text_of(5000), noise(40000), text_of(300000), bytes_of(""), text_of(777)};
    for (int window = 9; window <= 15; ++window) {
        for (const bool takeover_off : {false, true}) {
            auto sender = make_codecs(params(window, takeover_off));
            auto receiver = make_codecs(params(15, false, takeover_off));  // matching direction
            for (const auto& message : messages) {
                const auto compressed = compress(*sender.deflater, message);
                auto back = inflate_all(*receiver.inflater, compressed);
                ASSERT_TRUE(back.has_value()) << window << ' ' << takeover_off;
                EXPECT_EQ(*back, message) << window << ' ' << takeover_off << ' ' << message.size();
            }
        }
    }
}

TEST(ZlibCodec, IncompressibleDataStaysWithinDeflatesWorstCase) {
    auto codecs = make_codecs(params());
    const auto message = noise(100000);
    const auto compressed = compress(*codecs.deflater, message);
    EXPECT_GT(compressed.size(), 16U * 1024U);  // more than one output step was needed
    EXPECT_LT(compressed.size(), message.size() + message.size() / 100 + 64);
}

TEST(ZlibCodec, ByteAtATimeInputAndTinyOutputBuffersGiveTheSameResult) {
    auto sender = make_codecs(params());
    const auto message = text_of(20000);
    const auto compressed = compress(*sender.deflater, message);
    auto receiver = make_codecs(params());
    auto back = inflate_all(*receiver.inflater, compressed, 1, 7);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, message);
}

/// finish() must give the same verdict whatever the output size. A stored block (LEN 4) whose four
/// data bytes are the very tail the receiver appends (00 00 ff ff) is a valid message ending at a
/// block boundary. With a 4-byte output buffer the first finish() call is exactly full, and the
/// second call used to re-read zlib's block-boundary flag, which zlib clears on re-entry: a false
/// "message ends inside a block".
TEST(ZlibCodec, FinishGivesTheSameVerdictWhateverTheOutputBufferSize) {
    const auto stored_header = bytes_of({0x00, 0x04, 0x00, 0xfb, 0xff});  // non-final, LEN 4
    for (const std::size_t out_size : {std::size_t{1}, std::size_t{4}, std::size_t{5}}) {
        auto receiver = make_codecs(params());
        std::vector<std::byte> big(1024);
        const auto first = receiver.inflater->inflate(stored_header, big);
        ASSERT_TRUE(first.has_value());
        ASSERT_EQ(first->produced, 0U);
        std::vector<std::byte> out(out_size);
        std::vector<std::byte> all;
        bool done = false;
        for (int calls = 0; !done && calls < 16; ++calls) {
            const auto step = receiver.inflater->finish(out);
            ASSERT_TRUE(step.has_value()) << "out_size " << out_size << ": "
                                          << std::string{step.error().context};
            all.insert(all.end(),
                       out.begin(),
                       out.begin() + static_cast<std::ptrdiff_t>(step->produced));
            done = step->done;
        }
        EXPECT_TRUE(done) << "out_size " << out_size;
        EXPECT_EQ(all, bytes_of({0x00, 0x00, 0xff, 0xff})) << "out_size " << out_size;
    }
}

TEST(ZlibCodec, TheInflaterIsReusableForTheNextMessageAfterFinish) {
    auto sender = make_codecs(params());
    auto receiver = make_codecs(params());
    for (int i = 0; i < 5; ++i) {
        const auto message = text_of(1000 + static_cast<std::size_t>(i) * 313);
        auto back = inflate_all(*receiver.inflater, compress(*sender.deflater, message));
        ASSERT_TRUE(back.has_value());
        EXPECT_EQ(*back, message);
    }
}

// ---- hostile input (SECURITY_DESIGN T6)

TEST(ZlibCodec, ABombIsProducedOnlyAsFastAsTheCallerAsks) {
    // 64 MiB of zeros compresses to about 64 KiB: a ratio the caller's limit must catch.
    auto sender = make_codecs(params());
    const std::vector<std::byte> zeros(std::size_t{64} << 20U);
    const auto bomb = compress(*sender.deflater, zeros);
    ASSERT_LT(bomb.size() * 100, zeros.size());

    auto receiver = make_codecs(params());
    std::vector<std::byte> piece(16 * 1024);
    auto step = receiver.inflater->inflate(bomb, piece);
    ASSERT_TRUE(step.has_value());
    EXPECT_EQ(step->produced, piece.size());  // exactly one piece, not the whole message
    EXPECT_LT(step->consumed, bomb.size());   // and the rest of the input is still waiting
}

TEST(ZlibCodec, GarbageIsInvalidCompressedData) {
    for (const auto& garbage :
         {bytes_of({0xff, 0xff, 0xff, 0xff}), noise(64, 7), bytes_of({0x07})}) {
        auto codecs = make_codecs(params());
        auto out = inflate_all(*codecs.inflater, garbage);
        ASSERT_FALSE(out.has_value());
        EXPECT_EQ(out.error().code, ErrorCode::InvalidCompressedData);
    }
}

TEST(ZlibCodec, ATruncatedMessageIsInvalidCompressedData) {
    auto sender = make_codecs(params());
    auto compressed = compress(*sender.deflater, text_of(4000));
    compressed.resize(compressed.size() / 2);
    auto receiver = make_codecs(params());
    auto out = inflate_all(*receiver.inflater, compressed);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, ErrorCode::InvalidCompressedData);
}

TEST(ZlibCodec, ANonEmptyMessageWithoutPayloadIsInvalid) {
    auto codecs = make_codecs(params());
    auto out = inflate_all(*codecs.inflater, {});
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, ErrorCode::InvalidCompressedData);
}

TEST(ZlibCodec, AFinalBlockEndsTheMessageAndTheNextOneStartsFresh) {  // section 7.2.3.4
    auto codecs = make_codecs(params());
    // BFINAL stored empty block, then a normal message in the same inflater.
    auto first = inflate_all(*codecs.inflater, bytes_of({0x01, 0x00, 0x00, 0xff, 0xff}));
    ASSERT_TRUE(first.has_value());
    EXPECT_TRUE(first->empty());
    auto second =
        inflate_all(*codecs.inflater, bytes_of({0xf2, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00}));
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*second, bytes_of("Hello"));
}

TEST(ZlibCodec, DataAfterTheFinalBlockIsInvalid) {
    auto codecs = make_codecs(params());
    auto out = inflate_all(*codecs.inflater, bytes_of({0x01, 0x00, 0x00, 0xff, 0xff, 0x42}));
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, ErrorCode::InvalidCompressedData);
}

// ---- factory

TEST(ZlibCodec, TheFactoryRefusesWindowsAndLevelsZlibCannotDo) {
    const wsrv::ZlibCodecFactory factory{wsrv::ZlibSettings{}};
    for (const int bad : {0, 8, 16}) {
        auto codecs = factory.create(params(bad));
        ASSERT_FALSE(codecs.has_value()) << bad;
        EXPECT_EQ(codecs.error().code, ErrorCode::InvalidConfig);
    }
    EXPECT_TRUE(wsrv::valid(wsrv::ZlibSettings{}));
    EXPECT_FALSE(wsrv::valid(wsrv::ZlibSettings{0, 6}));
    EXPECT_FALSE(wsrv::valid(wsrv::ZlibSettings{6, 10}));
    const wsrv::ZlibCodecFactory broken{wsrv::ZlibSettings{10, 6}};
    EXPECT_FALSE(broken.create(params()).has_value());
}

}  // namespace

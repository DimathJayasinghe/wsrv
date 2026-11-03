// SPDX-License-Identifier: Apache-2.0
#include "ipc/frame_decoder.hpp"

#include "base/byte_buffer.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace wsrv;
using namespace wsrv::ipc;

constexpr std::size_t kLimit = 4096;

std::vector<std::byte> bytes_of(std::initializer_list<unsigned> values) {
    std::vector<std::byte> out;
    for (const unsigned v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

void append_frame(std::vector<std::byte>& stream, const Message& message) {
    ByteBuffer out{1U << 20};
    ASSERT_TRUE(encode(message, 1U << 20, out).has_value());
    const ByteView view = out.view();
    stream.insert(stream.end(), view.begin(), view.end());
}

/// What a run produced: each decoded message re-encoded (canonical bytes), and the failure if any.
struct Outcome {
    std::vector<std::vector<std::byte>> frames;
    std::optional<ErrorCode> failure;
    std::size_t leftover = 0;
};

/// Feed `stream` in chunks of `chunk` bytes (0 = all at once).
Outcome run(const std::vector<std::byte>& stream, std::size_t chunk, std::size_t limit = kLimit) {
    FrameDecoder decoder{limit};
    Outcome out;
    std::size_t pos = 0;
    while (pos < stream.size() && !out.failure) {
        const std::size_t take =
            chunk == 0 ? stream.size() - pos : std::min(chunk, stream.size() - pos);
        const std::vector<std::byte> piece(stream.begin() + static_cast<std::ptrdiff_t>(pos),
                                           stream.begin() +
                                               static_cast<std::ptrdiff_t>(pos + take));
        pos += take;
        ByteView input{piece};
        while (!input.empty()) {
            const auto r = decoder.next(input);
            if (!r) {
                out.failure = r.error().code;
                break;
            }
            if (!r->has_value()) {
                break;
            }
            ByteBuffer again{1U << 20};
            EXPECT_TRUE(encode(**r, 1U << 20, again).has_value());
            const ByteView view = again.view();
            out.frames.emplace_back(view.begin(), view.end());
        }
        out.leftover = input.size();
    }
    return out;
}

std::vector<std::byte> sample_stream() {
    std::vector<std::byte> stream;
    const std::vector<std::byte> blob(300, std::byte{0x5A});
    append_frame(stream, Ping{});
    append_frame(stream, SendData{1, 9, true, as_bytes("hello")});
    append_frame(stream, SendData{2, 9, false, ByteView{blob}});
    append_frame(stream, Event{EventKind::Close, 9, 1000, true, as_bytes("bye")});
    append_frame(stream, Status{2, 0});
    append_frame(stream, Pong{});
    return stream;
}

TEST(IpcFrameDecoder, DecodesSeveralFramesFromOneBuffer) {
    const Outcome out = run(sample_stream(), 0);
    EXPECT_FALSE(out.failure);
    EXPECT_EQ(out.frames.size(), 6U);
}

TEST(IpcFrameDecoder, ChunkingNeverChangesTheResult) {
    const auto stream = sample_stream();
    const Outcome whole = run(stream, 0);
    ASSERT_FALSE(whole.failure);
    for (const std::size_t chunk : {1U, 2U, 3U, 5U, 7U, 64U, 301U}) {
        const Outcome pieces = run(stream, chunk);
        EXPECT_EQ(pieces.failure, whole.failure) << "chunk " << chunk;
        EXPECT_EQ(pieces.frames, whole.frames) << "chunk " << chunk;
    }
}

TEST(IpcFrameDecoder, AFrameCutOffMidwayWaitsForMoreBytes) {
    auto stream = sample_stream();
    stream.resize(stream.size() - 3);  // the last Pong is incomplete
    const Outcome out = run(stream, 0);
    EXPECT_FALSE(out.failure);
    EXPECT_EQ(out.frames.size(), 5U);
}

// ---- the header is judged before any body byte exists
// -----------------------------------------------

TEST(IpcFrameDecoder, ALengthBelowTwoIsInvalid) {
    for (const unsigned length : {0U, 1U}) {
        const Outcome out = run(bytes_of({0, 0, 0, length, 6, 0}), 0);
        EXPECT_EQ(out.failure, ErrorCode::InvalidFrame) << length;
    }
}

TEST(IpcFrameDecoder, ALengthAboveTheLimitFailsFromTheHeaderAlone) {
    // 0x7FFFFFFF declared, only the 6 header bytes sent: the failure must not wait for a body.
    const Outcome out = run(bytes_of({0x7F, 0xFF, 0xFF, 0xFF, 3, 0}), 0);
    EXPECT_EQ(out.failure, ErrorCode::BufferFull);
}

TEST(IpcFrameDecoder, TheLimitItselfIsAccepted) {
    std::vector<std::byte> stream;
    const std::vector<std::byte> data(kLimit - 2 - 12, std::byte{1});  // length == kLimit exactly
    append_frame(stream, SendData{1, 1, false, ByteView{data}});
    EXPECT_FALSE(run(stream, 0).failure);
    EXPECT_FALSE(run(stream, 100).failure);
    EXPECT_EQ(run(stream, 0, kLimit - 1).failure,
              ErrorCode::BufferFull);  // one byte less is too big
}

TEST(IpcFrameDecoder, NonzeroFlagsAndUnknownTypesAreInvalid) {
    EXPECT_EQ(run(bytes_of({0, 0, 0, 2, 6, 1}), 0).failure, ErrorCode::InvalidFrame);
    EXPECT_EQ(run(bytes_of({0, 0, 0, 2, 6, 0x80}), 0).failure, ErrorCode::InvalidFrame);
    EXPECT_EQ(run(bytes_of({0, 0, 0, 2, 0, 0}), 0).failure, ErrorCode::InvalidFrame);
    EXPECT_EQ(run(bytes_of({0, 0, 0, 2, 10, 0}), 0).failure, ErrorCode::InvalidFrame);
    EXPECT_EQ(run(bytes_of({0, 0, 0, 2, 255, 0}), 0).failure, ErrorCode::InvalidFrame);
}

TEST(IpcFrameDecoder, ABadBodyIsInvalidWhateverTheChunking) {
    // A Status whose body is 6 bytes instead of 5.
    const auto stream = bytes_of({0, 0, 0, 8, 9, 0, 0, 0, 0, 1, 0, 0});
    for (const std::size_t chunk : {0U, 1U, 4U}) {
        EXPECT_EQ(run(stream, chunk).failure, ErrorCode::InvalidFrame) << chunk;
    }
}

TEST(IpcFrameDecoder, AFailureIsStickyAndLaterGoodBytesAreIgnored) {
    FrameDecoder decoder{kLimit};
    auto bad = bytes_of({0, 0, 0, 0, 6, 0});
    ByteView input{bad};
    ASSERT_FALSE(decoder.next(input).has_value());
    EXPECT_TRUE(decoder.failed());
    auto good = bytes_of({0, 0, 0, 2, 6, 0});
    ByteView again{good};
    const auto r = decoder.next(again);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::InvalidFrame);
}

// ---- memory: nothing is reserved from a declared length
// ---------------------------------------------

TEST(IpcFrameDecoder, ABigDeclaredFrameJustWaitsForItsBytes) {
    // Declares the largest legal body, sends 10 bytes of it. The gather buffer must hold about
    // that.
    FrameDecoder decoder{1U << 20};
    std::vector<std::byte> start =
        bytes_of({0, 0x10, 0x00, 0x00, 3, 0});  // 1 MiB frame, SendBinary
    start.insert(start.end(), 10, std::byte{7});
    ByteView input{start};
    const auto r = decoder.next(input);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value());
    EXPECT_TRUE(input.empty());
    EXPECT_FALSE(decoder.failed());
}

TEST(IpcFrameDecoder, ABodyDeliveredInOnePieceIsAViewIntoTheInput) {
    std::vector<std::byte> stream;
    append_frame(stream, SendData{1, 5, true, as_bytes("zero-copy")});
    FrameDecoder decoder{kLimit};
    ByteView input{stream};
    const auto r = decoder.next(input);
    ASSERT_TRUE(r.has_value() && r->has_value());
    const auto& data = std::get<SendData>(**r);
    EXPECT_GE(data.data.data(), stream.data());
    EXPECT_LT(data.data.data(), stream.data() + stream.size());
    EXPECT_TRUE(input.empty());
}

TEST(IpcFrameDecoder, AnEmptyInputNeedsMoreBytes) {
    FrameDecoder decoder{kLimit};
    ByteView none;
    const auto r = decoder.next(none);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value());
}

}  // namespace

// SPDX-License-Identifier: Apache-2.0
#include "protocol/frame_writer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace {

using wsrv::FrameHeader;
using wsrv::Opcode;

std::vector<unsigned> encode(const FrameHeader& header) {
    std::array<std::byte, wsrv::kMaxHeaderSize> buffer{};
    const auto size = wsrv::encode_header(buffer, header);
    EXPECT_TRUE(size.has_value());
    std::vector<unsigned> out;
    for (std::size_t i = 0; i < size.value_or(0); ++i) {
        out.push_back(std::to_integer<unsigned>(buffer[i]));
    }
    return out;
}

FrameHeader frame(Opcode opcode, std::uint64_t length, bool masked = false) {
    FrameHeader h;
    h.opcode = opcode;
    h.payload_length = length;
    h.masked = masked;
    h.mask_key = {std::byte{0x37}, std::byte{0xfa}, std::byte{0x21}, std::byte{0x3d}};
    return h;
}

TEST(FrameWriter, UnmaskedHelloHeaderFromTheRfc) {  // RFC 6455 section 5.7
    EXPECT_EQ(encode(frame(Opcode::Text, 5)), (std::vector<unsigned>{0x81, 0x05}));
}

TEST(FrameWriter, MaskedHelloHeaderFromTheRfc) {
    EXPECT_EQ(encode(frame(Opcode::Text, 5, true)),
              (std::vector<unsigned>{0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d}));
}

TEST(FrameWriter, UsesTheShortestLengthEncodingAtEveryBoundary) {  // EC-F04
    EXPECT_EQ(encode(frame(Opcode::Binary, 0)), (std::vector<unsigned>{0x82, 0x00}));
    EXPECT_EQ(encode(frame(Opcode::Binary, 125)), (std::vector<unsigned>{0x82, 125}));
    EXPECT_EQ(encode(frame(Opcode::Binary, 126)), (std::vector<unsigned>{0x82, 126, 0x00, 126}));
    EXPECT_EQ(encode(frame(Opcode::Binary, 256)), (std::vector<unsigned>{0x82, 126, 0x01, 0x00}));
    EXPECT_EQ(encode(frame(Opcode::Binary, 65535)), (std::vector<unsigned>{0x82, 126, 0xFF, 0xFF}));
    EXPECT_EQ(encode(frame(Opcode::Binary, 65536)),
              (std::vector<unsigned>{0x82, 127, 0, 0, 0, 0, 0, 0x01, 0x00, 0x00}));
    EXPECT_EQ(encode(frame(Opcode::Binary, 0x7FFFFFFFFFFFFFFFULL)),
              (std::vector<unsigned>{0x82, 127, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
}

TEST(FrameWriter, MaskedFramesAppendTheKeyAfterTheLength) {
    const auto bytes = encode(frame(Opcode::Binary, 300, true));
    EXPECT_EQ(bytes, (std::vector<unsigned>{0x82, 0xFE, 0x01, 0x2C, 0x37, 0xfa, 0x21, 0x3d}));
}

TEST(FrameWriter, FlagsAndOpcodesLandInTheFirstByte) {
    FrameHeader h = frame(Opcode::Continuation, 0);
    h.fin = false;
    EXPECT_EQ(encode(h)[0], 0x00U);
    h.fin = true;
    h.rsv1 = true;
    h.opcode = Opcode::Text;
    EXPECT_EQ(encode(h)[0], 0xC1U);
    h.rsv1 = false;
    h.rsv2 = true;
    EXPECT_EQ(encode(h)[0], 0xA1U);
    h.rsv2 = false;
    h.rsv3 = true;
    h.opcode = Opcode::Binary;
    EXPECT_EQ(encode(h)[0], 0x92U);
    EXPECT_EQ(encode(frame(Opcode::Close, 0))[0], 0x88U);
    EXPECT_EQ(encode(frame(Opcode::Ping, 0))[0], 0x89U);
    EXPECT_EQ(encode(frame(Opcode::Pong, 0))[0], 0x8AU);
}

TEST(FrameWriter, RefusesToBuildInvalidControlFrames) {
    std::array<std::byte, wsrv::kMaxHeaderSize> buffer{};
    EXPECT_TRUE(wsrv::encode_header(buffer, frame(Opcode::Ping, 125)).has_value());
    EXPECT_EQ(wsrv::encode_header(buffer, frame(Opcode::Ping, 126)).error().code,
              wsrv::ErrorCode::InvalidArgument);
    FrameHeader unfinished = frame(Opcode::Close, 0);
    unfinished.fin = false;
    EXPECT_FALSE(wsrv::encode_header(buffer, unfinished).has_value());
}

TEST(FrameWriter, RefusesLengthsWithTheTopBitSet) {
    std::array<std::byte, wsrv::kMaxHeaderSize> buffer{};
    EXPECT_FALSE(
        wsrv::encode_header(buffer, frame(Opcode::Binary, 0x8000000000000000ULL)).has_value());
    EXPECT_FALSE(wsrv::encode_header(buffer, frame(Opcode::Binary, ~0ULL)).has_value());
}

TEST(FrameWriter, HeaderSizeAgreesWithTheEncoder) {
    for (const std::uint64_t length :
         {0ULL, 1ULL, 125ULL, 126ULL, 65535ULL, 65536ULL, 1ULL << 40}) {
        for (const bool masked : {false, true}) {
            std::array<std::byte, wsrv::kMaxHeaderSize> buffer{};
            const auto size = wsrv::encode_header(buffer, frame(Opcode::Binary, length, masked));
            ASSERT_TRUE(size.has_value());
            EXPECT_EQ(*size, wsrv::header_size(length, masked)) << length << " " << masked;
        }
    }
}

}  // namespace

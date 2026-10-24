// SPDX-License-Identifier: Apache-2.0
#include "protocol/utf8.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <random>
#include <vector>

namespace {

using wsrv::ByteView;
using wsrv::ErrorCode;
using wsrv::Utf8Validator;

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> out;
    for (const unsigned v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

bool valid(std::initializer_list<unsigned> values) {
    const std::vector<std::byte> data = bytes(values);
    return wsrv::is_valid_utf8(data);
}

// ---- Independent reference: Unicode Table 3-7 "Well-Formed UTF-8 Byte Sequences", row by row
// --------

struct Row {
    std::array<unsigned, 4> lo;  // allowed range per byte position; only the first `count` are used
    std::array<unsigned, 4> hi;
    std::size_t count;
};

constexpr std::array<Row, 9> kTable37{{
    {{0x00}, {0x7F}, 1},
    {{0xC2, 0x80}, {0xDF, 0xBF}, 2},
    {{0xE0, 0xA0, 0x80}, {0xE0, 0xBF, 0xBF}, 3},
    {{0xE1, 0x80, 0x80}, {0xEC, 0xBF, 0xBF}, 3},
    {{0xED, 0x80, 0x80}, {0xED, 0x9F, 0xBF}, 3},
    {{0xEE, 0x80, 0x80}, {0xEF, 0xBF, 0xBF}, 3},
    {{0xF0, 0x90, 0x80, 0x80}, {0xF0, 0xBF, 0xBF, 0xBF}, 4},
    {{0xF1, 0x80, 0x80, 0x80}, {0xF3, 0xBF, 0xBF, 0xBF}, 4},
    {{0xF4, 0x80, 0x80, 0x80}, {0xF4, 0x8F, 0xBF, 0xBF}, 4},
}};

bool reference_valid(ByteView data) {
    std::size_t pos = 0;
    while (pos < data.size()) {
        bool matched = false;
        for (const Row& row : kTable37) {
            if (pos + row.count > data.size()) {
                continue;
            }
            bool ok = true;
            for (std::size_t i = 0; i < row.count && ok; ++i) {
                const auto b = std::to_integer<unsigned>(data[pos + i]);
                ok = b >= row.lo[i] && b <= row.hi[i];
            }
            if (ok) {
                pos += row.count;
                matched = true;
                break;
            }
        }
        if (!matched) {
            return false;
        }
    }
    return true;
}

// ---- EC-U01: valid sequences
// -----------------------------------------------------------------------

TEST(Utf8, AcceptsBoundaryCodePointsOfEveryLength) {
    EXPECT_TRUE(valid({}));
    EXPECT_TRUE(valid({0x00}));                    // U+0000
    EXPECT_TRUE(valid({0x7F}));                    // U+007F
    EXPECT_TRUE(valid({0xC2, 0x80}));              // U+0080
    EXPECT_TRUE(valid({0xDF, 0xBF}));              // U+07FF
    EXPECT_TRUE(valid({0xE0, 0xA0, 0x80}));        // U+0800
    EXPECT_TRUE(valid({0xED, 0x9F, 0xBF}));        // U+D7FF (last before the surrogates)
    EXPECT_TRUE(valid({0xEE, 0x80, 0x80}));        // U+E000 (first after the surrogates)
    EXPECT_TRUE(valid({0xEF, 0xBF, 0xBF}));        // U+FFFF
    EXPECT_TRUE(valid({0xF0, 0x90, 0x80, 0x80}));  // U+10000
    EXPECT_TRUE(valid({0xF4, 0x8F, 0xBF, 0xBF}));  // U+10FFFF
}

// ---- EC-U02 .. EC-U05: invalid sequences
// -----------------------------------------------------------

TEST(Utf8, RejectsOverlongEncodings) {  // EC-U02
    EXPECT_FALSE(valid({0xC0, 0x80}));
    EXPECT_FALSE(valid({0xC1, 0xBF}));
    EXPECT_FALSE(valid({0xE0, 0x80, 0x80}));
    EXPECT_FALSE(valid({0xE0, 0x9F, 0xBF}));
    EXPECT_FALSE(valid({0xF0, 0x80, 0x80, 0x80}));
    EXPECT_FALSE(valid({0xF0, 0x8F, 0xBF, 0xBF}));
}

TEST(Utf8, RejectsSurrogates) {               // EC-U03
    EXPECT_FALSE(valid({0xED, 0xA0, 0x80}));  // U+D800
    EXPECT_FALSE(valid({0xED, 0xBF, 0xBF}));  // U+DFFF
}

TEST(Utf8, RejectsCodePointsAboveTenFFFF) {  // EC-U04
    EXPECT_FALSE(valid({0xF4, 0x90, 0x80, 0x80}));
    EXPECT_FALSE(valid({0xF5, 0x80, 0x80, 0x80}));
    EXPECT_FALSE(valid({0xF8, 0x88, 0x80, 0x80, 0x80}));
    EXPECT_FALSE(valid({0xFF}));
    EXPECT_FALSE(valid({0xFE}));
}

TEST(Utf8, RejectsStrayContinuationAndBrokenSequences) {  // EC-U05
    EXPECT_FALSE(valid({0x80}));
    EXPECT_FALSE(valid({0xBF}));
    EXPECT_FALSE(valid({0xC2, 0x41}));        // lead followed by ASCII
    EXPECT_FALSE(valid({0xE2, 0x82, 0x41}));  // truncated 3-byte then ASCII
    EXPECT_FALSE(valid({0xE2, 0x82}));        // truncated at the end: incomplete is not valid
}

// ---- state machine behaviour
// ------------------------------------------------------------------------

TEST(Utf8, TruncatedTailIsNotAnErrorYetButIsNotABoundary) {
    Utf8Validator v;
    ASSERT_TRUE(v.feed(bytes({'a', 0xE2, 0x82})).has_value());
    EXPECT_FALSE(v.at_boundary());
    EXPECT_EQ(v.pending_bytes(), 2U);
    ASSERT_TRUE(v.feed(bytes({0xAC})).has_value());  // completes U+20AC
    EXPECT_TRUE(v.at_boundary());
    EXPECT_EQ(v.pending_bytes(), 0U);
}

TEST(Utf8, PendingStateSurvivesEmptyFeeds) {
    Utf8Validator v;
    ASSERT_TRUE(v.feed(bytes({0xF0, 0x9F})).has_value());
    ASSERT_TRUE(v.feed({}).has_value());
    ASSERT_TRUE(v.feed(bytes({0x98})).has_value());
    ASSERT_TRUE(v.feed({}).has_value());
    ASSERT_TRUE(v.feed(bytes({0x80})).has_value());  // U+1F600
    EXPECT_TRUE(v.at_boundary());
}

TEST(Utf8, FailsAtTheFirstInvalidByteAndStaysFailedUntilReset) {
    Utf8Validator v;
    const auto result = v.feed(bytes({'o', 'k', 0xED, 0xA0, 0x80}));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidUtf8);
    EXPECT_FALSE(v.at_boundary());
    EXPECT_FALSE(v.feed(bytes({'a'})).has_value());  // valid input does not heal a failed validator
    v.reset();
    EXPECT_TRUE(v.feed(bytes({'a'})).has_value());
    EXPECT_TRUE(v.at_boundary());
}

TEST(Utf8, ResetDiscardsAPendingSequence) {
    Utf8Validator v;
    ASSERT_TRUE(v.feed(bytes({0xE2})).has_value());
    v.reset();
    EXPECT_TRUE(v.at_boundary());
    EXPECT_TRUE(v.feed(bytes({'x'})).has_value());
}

// ---- EC-U06: split at every byte boundary
// -----------------------------------------------------------

TEST(Utf8, ValidMessageIsAcceptedWhereverItIsSplit) {
    // "h", e-acute, "llo ", euro sign, " ", grinning face emoji, " end"
    const auto message = bytes({'h',
                                0xC3,
                                0xA9,
                                'l',
                                'l',
                                'o',
                                ' ',
                                0xE2,
                                0x82,
                                0xAC,
                                ' ',
                                0xF0,
                                0x9F,
                                0x98,
                                0x80,
                                ' ',
                                'e',
                                'n',
                                'd'});
    for (std::size_t split = 0; split <= message.size(); ++split) {
        Utf8Validator v;
        ASSERT_TRUE(v.feed(ByteView{message}.first(split)).has_value()) << "split " << split;
        ASSERT_TRUE(v.feed(ByteView{message}.subspan(split)).has_value()) << "split " << split;
        EXPECT_TRUE(v.at_boundary()) << "split " << split;
    }
}

TEST(Utf8, InvalidMessageFailsInTheChunkHoldingTheFirstInvalidByte) {
    // 'a' 'b' ED A0 80 'c' 'd': the first invalid byte is A0, at index 3.
    const auto message = bytes({'a', 'b', 0xED, 0xA0, 0x80, 'c', 'd'});
    constexpr std::size_t kFirstInvalid = 3;
    for (std::size_t split = 0; split <= message.size(); ++split) {
        Utf8Validator v;
        const bool first_ok = v.feed(ByteView{message}.first(split)).has_value();
        EXPECT_EQ(first_ok, split <= kFirstInvalid)
            << "split " << split;  // fail fast, not at the end
        if (first_ok) {
            EXPECT_FALSE(v.feed(ByteView{message}.subspan(split)).has_value()) << "split " << split;
        }
    }
}

// ---- EC-U09: differential against the reference
// ------------------------------------------------------

TEST(Utf8, AgreesWithTheReferenceOnEveryOneAndTwoByteString) {
    for (unsigned a = 0; a < 256; ++a) {
        const auto one = bytes({a});
        ASSERT_EQ(wsrv::is_valid_utf8(one), reference_valid(one)) << a;
        for (unsigned b = 0; b < 256; ++b) {
            const auto two = bytes({a, b});
            ASSERT_EQ(wsrv::is_valid_utf8(two), reference_valid(two)) << a << " " << b;
        }
    }
}

TEST(Utf8, AgreesWithTheReferenceOnEveryThreeByteStringThatOpensAMultiByteSequence) {
    // First bytes C0..F8: every byte that can open (or wrongly claim to open) a multi-byte
    // sequence. ASCII-led and stray-continuation-led strings are covered by the 1/2-byte and random
    // tests.
    for (unsigned a = 0xC0; a <= 0xF8; ++a) {
        for (unsigned b = 0; b < 256; ++b) {
            for (unsigned c = 0; c < 256; ++c) {
                const std::array<std::byte, 3> v{static_cast<std::byte>(a),
                                                 static_cast<std::byte>(b),
                                                 static_cast<std::byte>(c)};
                ASSERT_EQ(wsrv::is_valid_utf8(v), reference_valid(v)) << a << " " << b << " " << c;
            }
        }
    }
}

TEST(Utf8, AgreesWithTheReferenceOnFourByteStringsAtEveryRangeBoundary) {
    constexpr std::array<unsigned, 10> kEdges{
        0x00, 0x7F, 0x80, 0x8F, 0x90, 0x9F, 0xA0, 0xBF, 0xC0, 0xFF};
    for (unsigned a = 0xF0; a <= 0xF8; ++a) {
        for (unsigned b = 0; b < 256; ++b) {
            for (const unsigned c : kEdges) {
                for (const unsigned d : kEdges) {
                    const auto four = bytes({a, b, c, d});
                    ASSERT_EQ(wsrv::is_valid_utf8(four), reference_valid(four))
                        << a << " " << b << " " << c << " " << d;
                }
            }
        }
    }
}

TEST(Utf8, RandomStringsAgreeWithTheReferenceEvenWhenChunked) {
    std::mt19937 rng{20260101};
    constexpr std::array<unsigned, 14> kInteresting{
        0x00, 0x41, 0x7F, 0x80, 0x8F, 0x90, 0x9F, 0xA0, 0xBF, 0xC2, 0xE0, 0xED, 0xF0, 0xF4};
    for (int round = 0; round < 200'000; ++round) {
        std::vector<std::byte> data(1 + rng() % 8);
        for (std::byte& b : data) {
            b = static_cast<std::byte>(rng() % 4 == 0 ? rng() % 256
                                                      : kInteresting[rng() % kInteresting.size()]);
        }
        const bool expected = reference_valid(data);
        ASSERT_EQ(wsrv::is_valid_utf8(data), expected) << "round " << round;

        // Same verdict when delivered in two chunks, including the boundary condition at the end.
        const std::size_t split = rng() % (data.size() + 1);
        Utf8Validator v;
        const bool ok = v.feed(ByteView{data}.first(split)).has_value() &&
                        v.feed(ByteView{data}.subspan(split)).has_value() && v.at_boundary();
        ASSERT_EQ(ok, expected) << "round " << round << " split " << split;
    }
}

}  // namespace

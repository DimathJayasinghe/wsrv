// SPDX-License-Identifier: Apache-2.0
#include "base/log_format.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <string>
#include <utility>

// ADR-0037: the one-line key=value format. The properties that matter are the safety ones (a peer
// cannot forge or split a line, SECURITY_DESIGN T14) and the bounds (a line has a maximum size).

namespace {

using wsrv::LogLevel;
using wsrv::LogRecord;

wsrv::WallTime at_ms(std::int64_t since_epoch_ms) {
    return wsrv::WallTime{std::chrono::duration_cast<wsrv::WallTime::duration>(
        std::chrono::milliseconds{since_epoch_ms})};
}

std::size_t count_of(const std::string& text, char c) {
    return static_cast<std::size_t>(std::count(text.begin(), text.end(), c));
}

}  // namespace

TEST(LogTimestamp, IsRfc3339UtcWithMilliseconds) {
    EXPECT_EQ(wsrv::format_timestamp(at_ms(0)), "1970-01-01T00:00:00.000Z");
    EXPECT_EQ(wsrv::format_timestamp(at_ms(1'000'000'000'000)), "2001-09-09T01:46:40.000Z");
    EXPECT_EQ(wsrv::format_timestamp(at_ms(1'700'000'000'123)), "2023-11-14T22:13:20.123Z");
    EXPECT_EQ(wsrv::format_timestamp(at_ms(1'767'225'599'999)), "2025-12-31T23:59:59.999Z");
    EXPECT_EQ(wsrv::format_timestamp(at_ms(1'767'225'600'000)), "2026-01-01T00:00:00.000Z");
}

TEST(LogTimestamp, KnowsLeapDays) {
    EXPECT_EQ(wsrv::format_timestamp(at_ms(951'782'400'000)), "2000-02-29T00:00:00.000Z");
    EXPECT_EQ(wsrv::format_timestamp(at_ms(951'868'800'000)), "2000-03-01T00:00:00.000Z");
    EXPECT_EQ(wsrv::format_timestamp(at_ms(1'709'164'800'000)), "2024-02-29T00:00:00.000Z");
}

TEST(LogTimestamp, AClockBeforeTheEpochIsShownAsTheEpochNotGarbage) {
    EXPECT_EQ(wsrv::format_timestamp(at_ms(-5'000)), "1970-01-01T00:00:00.000Z");
}

TEST(LogEscape, PrintableAsciiPassesThrough) {
    std::string out;
    EXPECT_FALSE(wsrv::append_escaped(out, "per-IP limit reached: 100 (x/y) ~!", 256));
    EXPECT_EQ(out, "per-IP limit reached: 100 (x/y) ~!");
}

TEST(LogEscape, QuoteBackslashAndTheWhitespaceControlsGetShortEscapes) {
    std::string out;
    wsrv::append_escaped(out, "a\"b\\c\nd\re\tf", 256);
    EXPECT_EQ(out, "a\\\"b\\\\c\\nd\\re\\tf");
}

TEST(LogEscape, OtherControlsDeleteAndEveryByteAboveSevenBitsBecomeHexEscapes) {
    std::string out;
    wsrv::append_escaped(out, std::string{"\x01\x1b\x7f", 3} + "\xc3\xa9\xff", 256);
    EXPECT_EQ(out, "\\x01\\x1b\\x7f\\xc3\\xa9\\xff")
        << "invalid UTF-8 must not reach a terminal either";
}

TEST(LogEscape, AnEmbeddedNulIsEscapedNotAnEndOfString) {
    std::string out;
    wsrv::append_escaped(out, std::string{"a\0b", 3}, 256);
    EXPECT_EQ(out, "a\\x00b");
}

TEST(LogEscape, TheCapIsOnEscapedCharactersAndNeverSplitsAnEscapeSequence) {
    std::string out;
    EXPECT_TRUE(wsrv::append_escaped(out, std::string(300, 'a'), 256));
    EXPECT_EQ(out.size(), 256U);

    out.clear();
    // 255 plain characters leave room for one more character, but "\n" needs two.
    EXPECT_TRUE(wsrv::append_escaped(out, std::string(255, 'a') + "\n", 256));
    EXPECT_EQ(out, std::string(255, 'a'));

    out.clear();
    EXPECT_FALSE(wsrv::append_escaped(out, std::string(254, 'a') + "\n", 256));
    EXPECT_EQ(out.size(), 256U) << "exactly at the cap is not truncated";
}

TEST(LogEscape, AHostileBodyEscapingToFourTimesItsSizeStaysUnderTheCap) {
    std::string out;
    EXPECT_TRUE(wsrv::append_escaped(out, std::string(10'000, '\xff'), 256));
    EXPECT_LE(out.size(), 256U);
}

TEST(LogToken, KeepsOnlyCharactersAnAddressOrNameNeeds) {
    std::string out;
    wsrv::append_token(out, "[2001:db8::1%eth0]:443", 64);
    EXPECT_EQ(out, "[2001:db8::1%eth0]:443");
    out.clear();
    wsrv::append_token(out, "1.2.3.4\" msg=\"forged\n", 64);
    EXPECT_EQ(out,
              "1.2.3.4?"
              "?msg?"
              "?forged?")
        << "quote, equals sign, space and newline are replaced";
    out.clear();
    wsrv::append_token(out, std::string(200, 'x'), 64);
    EXPECT_EQ(out.size(), 64U);
}

TEST(LogLine, ShowsEveryFieldInTheFixedOrder) {
    const LogRecord record{
        LogLevel::Warn, "ip_limit", "per-IP limit reached, refusing", 17, "203.0.113.9:51234"};
    EXPECT_EQ(
        wsrv::format_line(at_ms(1'700'000'000'123), record, 412, 3),
        "ts=2023-11-14T22:13:20.123Z level=warn event=ip_limit conn=17 peer=203.0.113.9:51234 "
        "suppressed=412 dropped=3 msg=\"per-IP limit reached, refusing\"\n");
}

TEST(LogLine, OmitsWhatIsAbsent) {
    const LogRecord record{LogLevel::Info, "listening", "websocket server listening"};
    EXPECT_EQ(wsrv::format_line(at_ms(0), record, 0, 0),
              "ts=1970-01-01T00:00:00.000Z level=info event=listening "
              "msg=\"websocket server listening\"\n");
}

TEST(LogLine, NamesEveryLevel) {
    for (const auto& [level, name] : {std::pair{LogLevel::Debug, "debug"},
                                      std::pair{LogLevel::Info, "info"},
                                      std::pair{LogLevel::Warn, "warn"},
                                      std::pair{LogLevel::Error, "error"}}) {
        const std::string line = wsrv::format_line(at_ms(0), LogRecord{level, "e", "m"}, 0, 0);
        EXPECT_NE(line.find(std::string{" level="} + name + " "), std::string::npos) << line;
    }
}

// SECURITY_DESIGN T14.
TEST(LogLine, APeerCannotEndTheLineOrAddFields) {
    const LogRecord record{
        LogLevel::Warn,
        "origin_refused",
        "origin \"x\"\nts=1999-01-01T00:00:00.000Z level=error event=forged msg=\"pwned\"",
        5,
        ""};
    const std::string line = wsrv::format_line(at_ms(0), record, 0, 0);
    EXPECT_EQ(count_of(line, '\n'), 1U) << "exactly the terminating newline";
    EXPECT_EQ(line.back(), '\n');
    EXPECT_EQ(count_of(line.substr(0, line.size() - 1), '"'), 2U + 4U)
        << "the only raw quotes are the two around msg; the four inside are escaped";
    EXPECT_NE(line.find("\\nts=1999"), std::string::npos)
        << "the forged field is inert text inside msg";
}

TEST(LogLine, AMissingEventNameStillGivesAWellFormedLine) {
    const std::string line = wsrv::format_line(at_ms(0), LogRecord{LogLevel::Error, "", "x"}, 0, 0);
    EXPECT_NE(line.find(" event=unknown "), std::string::npos) << line;
}

TEST(LogLine, TheWorstCaseLineIsBounded) {
    const std::string huge(10'000, '\xff');
    const LogRecord record{
        LogLevel::Error, huge, huge, std::numeric_limits<std::uint64_t>::max(), huge};
    const std::string line = wsrv::format_line(at_ms(1'700'000'000'123),
                                               record,
                                               std::numeric_limits<std::uint64_t>::max(),
                                               std::numeric_limits<std::uint64_t>::max());
    EXPECT_LE(line.size(), wsrv::kLogMaxLine);
    EXPECT_EQ(line.back(), '\n');
    EXPECT_NE(line.find("...\"\n"), std::string::npos) << "a cut message says so and stays quoted";
    EXPECT_EQ(count_of(line, '\n'), 1U);
}

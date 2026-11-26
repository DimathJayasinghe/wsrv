// SPDX-License-Identifier: Apache-2.0
#include "base/log_format.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>

namespace wsrv {

namespace {

constexpr std::string_view kHex = "0123456789abcdef";

// The longest line the fields can make, with every one at its cap: "ts=" + 24, " level=" + 5,
// " event=" + 32, " conn=" + 20 digits, " peer=" + 64, " suppressed=" + 20, " dropped=" + 20,
// ` msg="` + message + "..." + closing quote + newline.
constexpr std::size_t kWorstCaseLine = 3 + 24 + (7 + 5) + (7 + kLogMaxEvent) + (6 + 20) +
                                       (6 + kLogMaxPeer) + (12 + 20) + (9 + 20) +
                                       (6 + kLogMaxMessage + 3 + 1 + 1);
static_assert(kWorstCaseLine <= kLogMaxLine,
              "a line must fit its bound with every field at its cap");

// The last millisecond of 9999-12-31T23:59:59.999Z.
constexpr std::int64_t kMaxMillis = 253'402'300'799'999;

void put_digits(char* out, std::uint64_t value, int width) {
    for (int i = width - 1; i >= 0; --i) {
        out[i] = static_cast<char>('0' + value % 10);
        value /= 10;
    }
}

void append_number(std::string& out, std::uint64_t value) {
    std::array<char, 20> digits{};
    std::size_t n = 0;
    do {
        digits[n++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (n > 0) {
        out.push_back(digits[--n]);
    }
}

[[nodiscard]] std::string_view level_name(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace:
        return "trace";
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warn:
        return "warn";
    case LogLevel::Error:
        return "error";
    }
    return "info";
}

[[nodiscard]] bool token_character(unsigned char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' ||
           c == '_' || c == ':' || c == '[' || c == ']' || c == '%' || c == '-';
}

}  // namespace

std::string format_timestamp(WallTime time) {
    std::int64_t millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
    millis = std::clamp<std::int64_t>(millis, 0, kMaxMillis);

    const std::int64_t days = millis / 86'400'000;
    const auto in_day = static_cast<std::uint64_t>(millis % 86'400'000);

    // Days since 1970-01-01 to a calendar date (the proleptic Gregorian algorithm of H. Hinnant,
    // "chrono-Compatible Low-Level Date Algorithms"); days is never negative here.
    const std::int64_t shifted = days + 719'468;
    const std::int64_t era = shifted / 146'097;
    const auto day_of_era = static_cast<std::uint64_t>(shifted - era * 146'097);
    const std::uint64_t year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36'524 - day_of_era / 146'096) / 365;
    const std::uint64_t day_of_year =
        day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const std::uint64_t month_index = (5 * day_of_year + 2) / 153;
    const std::uint64_t day = day_of_year - (153 * month_index + 2) / 5 + 1;
    const std::uint64_t month = month_index < 10 ? month_index + 3 : month_index - 9;
    const auto year =
        static_cast<std::uint64_t>(static_cast<std::int64_t>(year_of_era) + era * 400) +
        (month <= 2 ? 1 : 0);

    std::array<char, 24> text{};  // "YYYY-MM-DDTHH:MM:SS.mmmZ"
    put_digits(text.data(), year, 4);
    text[4] = '-';
    put_digits(&text[5], month, 2);
    text[7] = '-';
    put_digits(&text[8], day, 2);
    text[10] = 'T';
    put_digits(&text[11], in_day / 3'600'000, 2);
    text[13] = ':';
    put_digits(&text[14], in_day / 60'000 % 60, 2);
    text[16] = ':';
    put_digits(&text[17], in_day / 1000 % 60, 2);
    text[19] = '.';
    put_digits(&text[20], in_day % 1000, 3);
    text[23] = 'Z';
    return std::string{text.data(), text.size()};
}

bool append_escaped(std::string& out, std::string_view text, std::size_t max_chars) {
    std::size_t used = 0;
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        std::array<char, 4> piece{};
        std::size_t length = 0;
        switch (c) {
        case '"':
            piece = {'\\', '"', 0, 0};
            length = 2;
            break;
        case '\\':
            piece = {'\\', '\\', 0, 0};
            length = 2;
            break;
        case '\n':
            piece = {'\\', 'n', 0, 0};
            length = 2;
            break;
        case '\r':
            piece = {'\\', 'r', 0, 0};
            length = 2;
            break;
        case '\t':
            piece = {'\\', 't', 0, 0};
            length = 2;
            break;
        default:
            if (c >= 0x20 && c < 0x7f) {
                piece[0] = raw;
                length = 1;
            } else {
                piece = {'\\', 'x', kHex[c >> 4U], kHex[c & 0x0FU]};
                length = 4;
            }
        }
        if (used + length > max_chars) {
            return true;
        }
        out.append(piece.data(), length);
        used += length;
    }
    return false;
}

void append_token(std::string& out, std::string_view text, std::size_t max_chars) {
    const std::size_t count = std::min(text.size(), max_chars);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(token_character(static_cast<unsigned char>(text[i])) ? text[i] : '?');
    }
}

std::string format_line(WallTime time,
                        const LogRecord& record,
                        std::uint64_t suppressed,
                        std::uint64_t dropped) {
    std::string line;
    line.reserve(kLogMaxLine);
    line += "ts=";
    line += format_timestamp(time);
    line += " level=";
    line += level_name(record.level);
    line += " event=";
    if (record.event.empty()) {
        line += "unknown";
    } else {
        append_token(line, record.event, kLogMaxEvent);
    }
    if (record.conn != 0) {
        line += " conn=";
        append_number(line, record.conn);
    }
    if (!record.peer.empty()) {
        line += " peer=";
        append_token(line, record.peer, kLogMaxPeer);
    }
    if (suppressed != 0) {
        line += " suppressed=";
        append_number(line, suppressed);
    }
    if (dropped != 0) {
        line += " dropped=";
        append_number(line, dropped);
    }
    line += " msg=\"";
    if (append_escaped(line, record.message, kLogMaxMessage)) {
        line += "...";
    }
    line += "\"\n";
    return line;
}

}  // namespace wsrv

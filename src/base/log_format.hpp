// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/clock.hpp"
#include "base/logger.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace wsrv {

// ADR-0037 section 2. Every size here is a constant: nothing a peer sends can make a line longer.
inline constexpr std::size_t kLogMaxEvent = 32;
inline constexpr std::size_t kLogMaxPeer = 64;
inline constexpr std::size_t kLogMaxMessage = 256;  // escaped characters
inline constexpr std::size_t kLogMaxLine = 512;  // far below PIPE_BUF: one write, never interleaved

/// "2026-11-26T12:00:00.123Z": RFC 3339, UTC, milliseconds. A time before 1970 is shown as the
/// epoch and one after year 9999 as the last millisecond of 9999, so the text always has the same
/// shape.
[[nodiscard]] std::string format_timestamp(WallTime time);

/// Appends `text` as the inside of a double-quoted value: `"` and `\` get a backslash, \n \r \t get
/// their short escapes, and every other byte below 0x20, 0x7f and every byte from 0x80 up becomes
/// `\xNN`. So the result is printable ASCII on one line whatever the input (log injection,
/// SECURITY_DESIGN T14). Stops before the character that would take the output past `max_chars`
/// added characters, never in the middle of an escape.
/// @return true if the text was cut.
bool append_escaped(std::string& out, std::string_view text, std::size_t max_chars);

/// Appends up to `max_chars` of `text` for an unquoted value (an event name, an address): letters,
/// digits and `. _ : [ ] % -` are kept, every other byte becomes `?`.
void append_token(std::string& out, std::string_view text, std::size_t max_chars);

/// One whole line, newline included, at most kLogMaxLine bytes:
/// `ts=... level=... event=... [conn=N] [peer=...] [suppressed=N] [dropped=N] msg="..."`.
/// `suppressed` is how many lines of this event the rate limiter held back since the last one;
/// `dropped` is how many lines the destination could not take. Both are omitted when 0.
[[nodiscard]] std::string format_line(WallTime time,
                                      const LogRecord& record,
                                      std::uint64_t suppressed,
                                      std::uint64_t dropped);

}  // namespace wsrv

// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string_view>
#include <vector>

namespace wsrv {

/// RFC 9110 section 5.6.2 "tchar": DIGIT / ALPHA / one of !#$%&'*+-.^_`|~
/// Thread-affinity: all functions in this header are pure and any-thread.
[[nodiscard]] constexpr bool is_tchar(char c) noexcept {
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
        return true;
    }
    switch (c) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
        return true;
    default:
        return false;
    }
}

/// A non-empty string of tchars (header names, methods, protocol names).
[[nodiscard]] bool is_token(std::string_view text) noexcept;

/// ASCII case-insensitive equality. Folds only A-Z; never consults the C locale (the Turkish
/// dotless-i bug: with a tr_TR locale, tolower('I') is not 'i'), and never folds bytes >= 0x80.
[[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept;

/// Remove leading and trailing SP / HTAB (RFC 9110 OWS) only.
[[nodiscard]] std::string_view trim_ows(std::string_view text) noexcept;

/// Split a comma-separated list (RFC 9110 section 5.6.1): elements are OWS-trimmed and empty
/// elements are dropped. Does NOT understand quoted-strings; do not use on values that may contain
/// quoted commas. The views point into `text`.
[[nodiscard]] std::vector<std::string_view> split_list(std::string_view text);

/// True if the comma-separated `list` contains `token` as a whole element, ASCII
/// case-insensitively.
[[nodiscard]] bool contains_token(std::string_view list, std::string_view token);

}  // namespace wsrv

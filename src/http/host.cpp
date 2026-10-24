// SPDX-License-Identifier: Apache-2.0
#include "http/host.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>

namespace wsrv {

namespace {

constexpr std::size_t kMaxPortDigits = 5;
constexpr unsigned kMaxPort = 65535;
constexpr int kIpv6Groups = 8;

[[nodiscard]] bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

[[nodiscard]] bool is_hex(char c) {
    return is_digit(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

[[nodiscard]] bool is_alpha(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

[[nodiscard]] bool is_name_char(char c) {
    return is_alpha(c) || is_digit(c) || c == '-' || c == '.' || c == '_' || c == '~';
}

[[nodiscard]] bool is_valid_port(std::string_view port) {
    if (port.empty() || port.size() > kMaxPortDigits || !std::ranges::all_of(port, is_digit)) {
        return false;
    }
    unsigned value = 0;
    for (const char c : port) {
        value = value * 10U + static_cast<unsigned>(c - '0');
    }
    return value <= kMaxPort;
}

[[nodiscard]] bool is_valid_reg_name(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (c == '%') {
            if (i + 2 >= name.size() || !is_hex(name[i + 1]) || !is_hex(name[i + 2])) {
                return false;
            }
            i += 2;
        } else if (!is_name_char(c)) {
            return false;
        }
    }
    return true;
}

/// "a.b.c.d" with each part 0..255 and no leading-zero tricks beyond three digits.
[[nodiscard]] bool is_ipv4(std::string_view text) {
    int parts = 0;
    while (!text.empty()) {
        const std::size_t dot = text.find('.');
        const std::string_view part = text.substr(0, dot);
        if (part.empty() || part.size() > 3 || !std::ranges::all_of(part, is_digit)) {
            return false;
        }
        unsigned value = 0;
        for (const char c : part) {
            value = value * 10U + static_cast<unsigned>(c - '0');
        }
        if (value > 255) {
            return false;
        }
        ++parts;
        if (dot == std::string_view::npos) {
            break;
        }
        text.remove_prefix(dot + 1);
        if (text.empty()) {
            return false;  // trailing dot
        }
    }
    return parts == 4;
}

/// Number of 16-bit groups in a colon-separated list ("a:b:c"), counting a trailing IPv4 as two;
/// nullopt if malformed. `allow_ipv4_tail`: the list ends the address, so a dotted quad may close
/// it.
[[nodiscard]] std::optional<int> count_groups(std::string_view list, bool allow_ipv4_tail) {
    if (list.empty()) {
        return 0;
    }
    int groups = 0;
    while (true) {
        const std::size_t colon = list.find(':');
        const std::string_view piece = list.substr(0, colon);
        const bool last = colon == std::string_view::npos;
        if (last && allow_ipv4_tail && piece.find('.') != std::string_view::npos) {
            if (!is_ipv4(piece)) {
                return std::nullopt;
            }
            return groups + 2;
        }
        if (piece.empty() || piece.size() > 4 || !std::ranges::all_of(piece, is_hex)) {
            return std::nullopt;
        }
        ++groups;
        if (last) {
            return groups;
        }
        list.remove_prefix(colon + 1);
        if (list.empty()) {
            return std::nullopt;  // trailing single colon
        }
    }
}

[[nodiscard]] bool is_valid_ipv6(std::string_view text) {
    if (text.find(":::") != std::string_view::npos) {
        return false;
    }
    const std::size_t gap = text.find("::");
    if (gap == std::string_view::npos) {
        return count_groups(text, true) == kIpv6Groups;
    }
    if (text.find("::", gap + 1) != std::string_view::npos) {
        return false;  // two compressions
    }
    const std::optional<int> left = count_groups(text.substr(0, gap), false);
    const std::optional<int> right = count_groups(text.substr(gap + 2), true);
    return left && right &&
           *left + *right <= kIpv6Groups - 1;  // "::" stands for at least one group
}

}  // namespace

bool is_valid_host_header(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    if (value.front() == '[') {
        const std::size_t close = value.find(']');
        if (close == std::string_view::npos || !is_valid_ipv6(value.substr(1, close - 1))) {
            return false;
        }
        const std::string_view rest = value.substr(close + 1);
        return rest.empty() || (rest.front() == ':' && is_valid_port(rest.substr(1)));
    }
    const std::size_t colon = value.find(':');
    if (colon != std::string_view::npos && !is_valid_port(value.substr(colon + 1))) {
        return false;
    }
    return is_valid_reg_name(value.substr(0, colon));
}

}  // namespace wsrv

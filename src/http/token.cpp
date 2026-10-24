// SPDX-License-Identifier: Apache-2.0
#include "http/token.hpp"

#include <algorithm>

namespace wsrv {

namespace {

[[nodiscard]] constexpr char fold(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

[[nodiscard]] constexpr bool is_ows(char c) noexcept {
    return c == ' ' || c == '\t';
}

}  // namespace

bool is_token(std::string_view text) noexcept {
    return !text.empty() && std::ranges::all_of(text, [](char c) { return is_tchar(c); });
}

bool iequals(std::string_view a, std::string_view b) noexcept {
    return std::ranges::equal(a, b, [](char x, char y) { return fold(x) == fold(y); });
}

std::string_view trim_ows(std::string_view text) noexcept {
    while (!text.empty() && is_ows(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_ows(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

std::vector<std::string_view> split_list(std::string_view text) {
    std::vector<std::string_view> out;
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        const std::string_view element = trim_ows(text.substr(0, comma));
        if (!element.empty()) {
            out.push_back(element);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        text.remove_prefix(comma + 1);
    }
    return out;
}

bool contains_token(std::string_view list, std::string_view token) {
    const std::vector<std::string_view> elements = split_list(list);
    return std::ranges::any_of(elements, [token](std::string_view e) { return iequals(e, token); });
}

}  // namespace wsrv

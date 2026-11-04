// SPDX-License-Identifier: Apache-2.0
#include "tls/sni.hpp"

#include <algorithm>

namespace wsrv::sni {

namespace {

constexpr bool is_label_char(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

constexpr char lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

/// Lower-case `name` and check it is dot-separated non-empty labels of letters, digits, '-' and
/// '_', each label at most 63 and the whole at most 253 characters. Anything else (a space, a
/// control byte, a non-ASCII byte, an empty label, a trailing dot) is refused. Internationalised
/// names arrive in their ASCII form ("xn--...") and so pass as ordinary labels.
std::optional<std::string> as_dns_name(std::string_view name) {
    if (name.empty() || name.size() > kMaxNameLength) {
        return std::nullopt;
    }
    std::string out;
    out.reserve(name.size());
    std::size_t label = 0;
    for (const char raw : name) {
        const char c = lower(raw);
        if (c == '.') {
            if (label == 0) {
                return std::nullopt;  // empty label: ".x", "a..b"
            }
            label = 0;
        } else if (is_label_char(c)) {
            if (++label > kMaxLabelLength) {
                return std::nullopt;
            }
        } else {
            return std::nullopt;
        }
        out.push_back(c);
    }
    if (label == 0) {
        return std::nullopt;  // ended with a dot
    }
    return out;
}

}  // namespace

std::optional<std::string> normalise_pattern(std::string_view pattern) {
    if (!pattern.starts_with("*.")) {
        return as_dns_name(pattern);
    }
    const std::string_view rest = pattern.substr(2);
    if (rest.find('.') == std::string_view::npos) {
        return std::nullopt;  // "*.com": a wildcard over a whole top-level domain
    }
    auto name = as_dns_name(rest);
    if (!name) {
        return std::nullopt;  // also catches "**.x", "*.*.x": '*' is not a label character
    }
    return "*." + *name;
}

std::optional<std::string> normalise_host(std::string_view host) {
    return as_dns_name(host);  // a wildcard is not a label character, so '*' is refused here
}

bool matches(std::string_view pattern, std::string_view host) {
    if (!pattern.starts_with("*.")) {
        return pattern == host;
    }
    const std::string_view suffix = pattern.substr(1);  // ".example.com"
    if (host.size() <= suffix.size() || !host.ends_with(suffix)) {
        return false;
    }
    const std::string_view label = host.substr(0, host.size() - suffix.size());
    return label.find('.') == std::string_view::npos;  // exactly one label, and it is non-empty
}

}  // namespace wsrv::sni

// SPDX-License-Identifier: Apache-2.0
#include "protocol/extension_offer.hpp"

#include "http/token.hpp"

#include <algorithm>

namespace wsrv {

namespace {

/// A tiny forward-only reader over the header value.
class Cursor {
public:
    explicit Cursor(std::string_view text) noexcept : rest_{text} {}

    [[nodiscard]] bool at_end() const noexcept {
        return rest_.empty();
    }
    [[nodiscard]] char peek() const noexcept {
        return rest_.front();
    }  // precondition: !at_end()
    void advance() noexcept {
        rest_.remove_prefix(1);
    }

    void skip_ows() noexcept {
        while (!rest_.empty() && (rest_.front() == ' ' || rest_.front() == '\t')) {
            advance();
        }
    }

    [[nodiscard]] std::string_view take_token() {
        std::size_t length = 0;
        while (length < rest_.size() && is_tchar(rest_[length])) {
            ++length;
        }
        const std::string_view token = rest_.substr(0, length);
        rest_.remove_prefix(length);
        return token;
    }

    /// Precondition: peek() == '"'. Returns the unescaped contents, or nullopt if unterminated or
    /// if it contains control characters.
    [[nodiscard]] std::optional<std::string> take_quoted() {
        advance();  // opening quote
        std::string out;
        while (!at_end()) {
            char c = peek();
            advance();
            if (c == '"') {
                return out;
            }
            if (c == '\\') {  // quoted-pair: the next character is taken literally
                if (at_end()) {
                    return std::nullopt;
                }
                c = peek();
                advance();
            }
            const auto byte = static_cast<unsigned char>(c);
            if ((byte < 0x20 && byte != '\t') || byte == 0x7F) {
                return std::nullopt;
            }
            out += c;
        }
        return std::nullopt;  // ran out of input before the closing quote
    }

private:
    std::string_view rest_;
};

/// param = token [ "=" ( token / quoted-string ) ]; appended to `offer`. False if malformed or
/// duplicate.
[[nodiscard]] bool parse_param(Cursor& cursor, ExtensionOffer& offer) {
    const std::string_view name = cursor.take_token();
    if (name.empty()) {
        return false;
    }
    std::string value;
    if (!cursor.at_end() && cursor.peek() == '=') {
        cursor.advance();
        if (cursor.at_end()) {
            return false;
        }
        if (cursor.peek() == '"') {
            std::optional<std::string> quoted = cursor.take_quoted();
            if (!quoted) {
                return false;
            }
            value = std::move(*quoted);
        } else {
            const std::string_view token = cursor.take_token();
            if (token.empty()) {
                return false;
            }
            value = std::string{token};
        }
    }
    const bool duplicate =
        std::ranges::any_of(offer.params, [name](const auto& p) { return p.first == name; });
    if (duplicate) {
        return false;
    }
    offer.params.emplace_back(std::string{name}, std::move(value));
    return true;
}

/// extension = token *( OWS ";" OWS param ). Stops before ',' or at the end.
[[nodiscard]] std::optional<ExtensionOffer> parse_offer(Cursor& cursor) {
    ExtensionOffer offer;
    const std::string_view name = cursor.take_token();
    if (name.empty()) {
        return std::nullopt;
    }
    offer.name = std::string{name};
    for (;;) {
        cursor.skip_ows();
        if (cursor.at_end() || cursor.peek() == ',') {
            return offer;
        }
        if (cursor.peek() != ';') {
            return std::nullopt;
        }
        cursor.advance();
        cursor.skip_ows();
        if (!parse_param(cursor, offer)) {
            return std::nullopt;
        }
    }
}

}  // namespace

std::optional<std::vector<ExtensionOffer>> parse_extension_offers(std::string_view header_value) {
    Cursor cursor{header_value};
    std::vector<ExtensionOffer> offers;
    for (;;) {
        cursor.skip_ows();
        if (cursor.at_end()) {
            return offers;
        }
        if (cursor.peek() == ',') {  // empty list element: ignored (RFC 9110 section 5.6.1)
            cursor.advance();
            continue;
        }
        std::optional<ExtensionOffer> offer = parse_offer(cursor);
        if (!offer) {
            return std::nullopt;
        }
        offers.push_back(std::move(*offer));
    }
}

}  // namespace wsrv

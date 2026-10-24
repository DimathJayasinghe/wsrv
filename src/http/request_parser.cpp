// SPDX-License-Identifier: Apache-2.0
#include "http/request_parser.hpp"

#include "http/token.hpp"

#include <string_view>

namespace wsrv {

namespace {

constexpr std::size_t kMaxMethodLength = 16;
constexpr std::string_view kVersionPrefix = "HTTP/";
constexpr std::size_t kVersionLength = 8;  // "HTTP/1.1"

[[nodiscard]] bool is_hex_digit(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

/// Characters allowed raw in an origin-form target: RFC 3986 unreserved, sub-delims, ':' '@' '/'
/// '?', and
/// "[]{}|^`", which browsers send unencoded (WHATWG URL percent-encodes only controls, space, " < >
/// # and a few more) and which cannot create framing ambiguity. '%' must introduce a valid escape
/// (checked separately). Backslash stays rejected: browsers rewrite it in paths, accepting it
/// invites path confusion.
[[nodiscard]] bool is_target_char(char c) noexcept {
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
        return true;
    }
    return std::string_view{"-._~!$&'()*+,;=:@/?%[]{}|^`"}.find(c) != std::string_view::npos;
}

/// RFC 9112 origin-form: starts with '/', no fragment, no raw spaces/controls/non-ASCII, valid %XX.
[[nodiscard]] bool is_valid_target(std::string_view target) noexcept {
    if (target.empty() || target.front() != '/') {
        return false;
    }
    for (std::size_t i = 0; i < target.size(); ++i) {
        if (!is_target_char(target[i])) {
            return false;
        }
        if (target[i] == '%' && (i + 2 >= target.size() || !is_hex_digit(target[i + 1]) ||
                                 !is_hex_digit(target[i + 2]))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

}  // namespace

std::unexpected<HttpError> HttpRequestParser::fail_with(int status, std::string_view reason) {
    error_ = HttpError{status, reason};
    return std::unexpected{*error_};
}

void HttpRequestParser::reset() noexcept {
    request_ = HttpRequest{};
    line_.clear();
    total_bytes_ = 0;
    pending_cr_ = false;
    in_request_line_ = true;
    complete_ = false;
    error_.reset();
}

HttpResult<ParseProgress> HttpRequestParser::feed(ByteView chunk) {
    if (error_) {
        return std::unexpected{*error_};
    }
    ParseProgress progress;
    for (const std::byte b : chunk) {
        if (complete_) {
            break;  // the rest belongs to the caller (first WebSocket frame bytes, EC-H19)
        }
        if (const HttpResult<void> step = consume_byte(std::to_integer<std::uint8_t>(b)); !step) {
            return std::unexpected{step.error()};
        }
        ++progress.consumed;
    }
    progress.complete = complete_;
    return progress;
}

HttpResult<void> HttpRequestParser::consume_byte(std::uint8_t byte) {
    if (++total_bytes_ > limits_.max_total_bytes) {
        return fail_with(431, "request head too large");
    }
    if (byte == 0) {
        return fail_with(400, "NUL byte");
    }
    const bool control =
        (byte < 0x20 && byte != '\t' && byte != '\r' && byte != '\n') || byte == 0x7F;
    if (control) {
        return fail_with(400, "control character");
    }
    if (byte == '\r') {
        if (pending_cr_) {
            return fail_with(400, "bare CR");
        }
        pending_cr_ = true;
        return {};
    }
    if (byte == '\n') {
        if (!pending_cr_) {
            return fail_with(400, "bare LF");
        }
        pending_cr_ = false;
        return end_of_line();
    }
    if (pending_cr_) {
        return fail_with(400, "bare CR");
    }
    line_.push_back(static_cast<char>(byte));
    if (in_request_line_ && line_.size() > limits_.max_request_line) {
        return fail_with(414, "request line too long");
    }
    return {};
}

HttpResult<void> HttpRequestParser::end_of_line() {
    if (in_request_line_) {
        const HttpResult<void> parsed = parse_request_line();
        line_.clear();
        in_request_line_ = false;
        return parsed;
    }
    if (line_.empty()) {
        complete_ = true;  // the blank line ends the head
        return {};
    }
    const HttpResult<void> parsed = parse_header_line();
    line_.clear();
    return parsed;
}

HttpResult<void> HttpRequestParser::parse_request_line() {
    const std::string_view line{line_};
    const std::size_t first = line.find(' ');
    const std::size_t second = first == std::string_view::npos ? first : line.find(' ', first + 1);
    if (second == std::string_view::npos) {
        return fail_with(400, "malformed request line");
    }
    const std::string_view method = line.substr(0, first);
    const std::string_view target = line.substr(first + 1, second - first - 1);
    const std::string_view version = line.substr(second + 1);

    if (!is_token(method) || method.size() > kMaxMethodLength) {
        return fail_with(400, "invalid method");
    }
    if (!is_valid_target(target)) {
        return fail_with(400, "invalid request target");
    }
    if (version.size() != kVersionLength ||
        version.substr(0, kVersionPrefix.size()) != kVersionPrefix || version[6] != '.' ||
        !is_digit(version[5]) || !is_digit(version[7])) {
        return fail_with(400, "malformed HTTP version");
    }
    if (version[5] != '1') {
        return fail_with(505, "HTTP version not supported");
    }
    request_.method = std::string{method};
    request_.target = std::string{target};
    request_.version_minor = version[7] - '0';
    return {};
}

HttpResult<void> HttpRequestParser::parse_header_line() {
    const std::string_view line{line_};
    if (line.front() == ' ' || line.front() == '\t') {
        return fail_with(400, "obsolete line folding");
    }
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
        return fail_with(400, "header line without colon");
    }
    const std::string_view name = line.substr(0, colon);
    if (!is_token(name)) {
        return fail_with(400, "invalid header name");
    }
    if (request_.headers.size() >= limits_.max_headers) {
        return fail_with(431, "too many headers");
    }
    request_.headers.push_back(
        HttpHeader{std::string{name}, std::string{trim_ows(line.substr(colon + 1))}});
    return {};
}

}  // namespace wsrv

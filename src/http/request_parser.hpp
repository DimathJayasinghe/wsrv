// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/bytes.hpp"
#include "http/request.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace wsrv {

/// Resource limits for the request head (REQUIREMENTS section 4). They bound memory AND the time an
/// attacker can keep a half-parsed request alive (SECURITY_DESIGN T2).
struct HttpLimits {
    std::size_t max_request_line = 4096;  // exceeded -> 414
    std::size_t max_total_bytes = 8192;   // whole head including CRLFs; exceeded -> 431
    std::size_t max_headers = 64;         // exceeded -> 431
};

/// A parse failure: the HTTP status to answer with (then close) and a static, human-readable
/// reason.
struct HttpError {
    int status;
    std::string_view reason;

    friend constexpr bool operator==(const HttpError&, const HttpError&) = default;
};

template <class T>
using HttpResult = std::expected<T, HttpError>;

struct ParseProgress {
    std::size_t consumed = 0;  // bytes taken from the chunk passed to feed()
    bool complete = false;     // the blank line ending the head has been consumed
};

/// Strict, incremental HTTP/1.x request-head parser for the WebSocket opening handshake.
///
/// Accepts only unambiguous input: CRLF line endings, no NUL/control characters, no obs-fold, no
/// whitespace before the colon, token header names, origin-form request targets without fragments,
/// valid percent-escapes. Lenient parsing is the root of request-smuggling bugs, so anything
/// ambiguous is a 400. The result is independent of how the bytes are chunked.
///
/// feed() stops exactly after the final LF of the blank line; anything after it (the first
/// WebSocket frame bytes may already be there, EC-H19) is NOT consumed and belongs to the caller.
///
/// Thread-affinity: loop-thread-only (not synchronised).
class HttpRequestParser {
public:
    explicit HttpRequestParser(HttpLimits limits = {}) noexcept : limits_{limits} {}

    /// Consume bytes. @return how many were consumed and whether the head is complete, or the HTTP
    /// error. After an error the parser stays failed (every call returns the same error) until
    /// reset(). After completion further calls consume 0 bytes.
    [[nodiscard]] HttpResult<ParseProgress> feed(ByteView chunk);

    /// The parsed request; meaningful once feed() reported `complete`.
    [[nodiscard]] const HttpRequest& request() const noexcept {
        return request_;
    }

    [[nodiscard]] bool complete() const noexcept {
        return complete_;
    }
    [[nodiscard]] bool failed() const noexcept {
        return error_.has_value();
    }

    /// Forget everything; ready for a new request.
    void reset() noexcept;

private:
    [[nodiscard]] HttpResult<void> consume_byte(std::uint8_t byte);
    [[nodiscard]] HttpResult<void> end_of_line();
    [[nodiscard]] HttpResult<void> parse_request_line();
    [[nodiscard]] HttpResult<void> parse_header_line();
    [[nodiscard]] std::unexpected<HttpError> fail_with(int status, std::string_view reason);

    HttpLimits limits_;
    HttpRequest request_;
    std::string line_;  // the line being assembled (without CRLF); bounded by the limits
    std::size_t total_bytes_ = 0;
    bool pending_cr_ = false;  // a CR has been seen and must be followed by LF
    bool in_request_line_ = true;
    bool complete_ = false;
    std::optional<HttpError> error_;
};

}  // namespace wsrv

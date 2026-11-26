// SPDX-License-Identifier: Apache-2.0
#include "server/admin_http.hpp"

#include "base/bytes.hpp"
#include "base/constant_time.hpp"
#include "http/bearer.hpp"
#include "http/request_parser.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace wsrv {

namespace {

/// Larger than any page we build (the metrics text is bounded by kMaxMetricsTextBytes); a source
/// that exceeds it is a bug and becomes a 500, never a huge response.
constexpr std::size_t kMaxBodyBytes = std::size_t{32} * 1024;

std::string respond(std::string_view status,
                    std::string_view content_type,
                    std::string_view extra_headers,
                    std::string_view body) {
    std::string out;
    out.reserve(160 + body.size());
    out += "HTTP/1.1 ";
    out += status;
    out += "\r\nContent-Type: ";
    out += content_type;
    out += "\r\nContent-Length: ";
    out += std::to_string(body.size());
    out += "\r\nConnection: close\r\n";
    out += extra_headers;
    out += "\r\n";
    out += body;
    return out;
}

std::string text(std::string_view status, std::string_view body, std::string_view extra = {}) {
    return respond(status, "text/plain; charset=utf-8", extra, body);
}

/// Split `line` at single spaces into exactly three non-empty tokens.
bool split_request_line(std::string_view line, std::array<std::string_view, 3>& parts) {
    std::size_t start = 0;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const std::size_t end = i + 1 == parts.size() ? line.size() : line.find(' ', start);
        if (end == std::string_view::npos || end == start) {
            return false;
        }
        parts[i] = line.substr(start, end - start);
        start = end + 1;
    }
    return parts[2].find(' ') == std::string_view::npos;
}

bool printable(std::string_view line) noexcept {
    return std::ranges::all_of(line, [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return byte >= 0x20 && byte < 0x7F;
    });
}

/// ADR-0041: the answer that keeps a peer from `/metrics`, or nothing when it may have the page.
/// The head is parsed with the same strict parser as every other head in the project (CRLF only, no
/// obs-fold, no control characters, token names), limited to what the admin port reads anyway.
/// Every credential problem is the same answer; a head that is not even well formed is a 400.
std::optional<std::string> refuse_unless_allowed(std::string_view head,
                                                 const AdminSources& sources) {
    HttpLimits limits;
    limits.max_request_line = kMaxAdminHeadBytes;
    limits.max_total_bytes = kMaxAdminHeadBytes;
    HttpRequestParser parser{limits};
    const HttpResult<ParseProgress> progress = parser.feed(as_bytes(head));
    if (!progress || !progress->complete) {
        return admin_bad_request();
    }
    const std::optional<std::string_view> credential = bearer_credential(parser.request());
    if (credential &&
        constant_time_equal(as_bytes(*credential), as_bytes(std::string_view{sources.token}))) {
        return std::nullopt;
    }
    if (sources.unauthorized) {
        sources.unauthorized();
    }
    return text("401 Unauthorized", "credential required\n", "WWW-Authenticate: Bearer\r\n");
}

/// `GET /metrics`: the credential first when a token is set (ADR-0041), and only then the page.
std::string metrics_response(std::string_view head, const AdminSources& sources) {
    if (!sources.token.empty()) {
        if (std::optional<std::string> refusal = refuse_unless_allowed(head, sources)) {
            return std::move(*refusal);
        }
    }
    const std::string body = sources.metrics_text ? sources.metrics_text() : std::string{};
    if (body.size() > kMaxBodyBytes) {
        return text("500 Internal Server Error", "metrics too large\n");
    }
    return respond("200 OK", "text/plain; version=0.0.4; charset=utf-8", {}, body);
}

}  // namespace

std::string admin_bad_request() {
    return text("400 Bad Request", "bad request\n");
}

bool admin_head_complete(std::string_view buffered) noexcept {
    return buffered.find("\r\n\r\n") != std::string_view::npos;
}

std::string admin_response(std::string_view head, const AdminSources& sources) {
    const std::size_t line_end = head.find("\r\n");
    std::array<std::string_view, 3> parts;
    if (line_end == std::string_view::npos || !printable(head.substr(0, line_end)) ||
        !split_request_line(head.substr(0, line_end), parts) ||
        (parts[2] != "HTTP/1.1" && parts[2] != "HTTP/1.0")) {
        return admin_bad_request();
    }
    if (parts[0] != "GET") {
        return text("405 Method Not Allowed", "method not allowed\n", "Allow: GET\r\n");
    }
    if (parts[1] == "/metrics") {
        return metrics_response(head, sources);
    }
    if (parts[1] == "/healthz") {
        return text("200 OK", "ok\n");
    }
    if (parts[1] == "/readyz") {
        const bool ready = sources.ready && sources.ready();
        return ready ? text("200 OK", "ready\n") : text("503 Service Unavailable", "not ready\n");
    }
    return text("404 Not Found", "not found\n");
}

}  // namespace wsrv

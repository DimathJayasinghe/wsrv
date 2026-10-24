// SPDX-License-Identifier: Apache-2.0
#include "http/response.hpp"

#include "http/token.hpp"

#include <algorithm>

namespace wsrv {

namespace {

constexpr int kMinStatus = 100;
constexpr int kMaxStatus = 599;

[[nodiscard]] bool is_valid_value(std::string_view value) noexcept {
    return std::ranges::all_of(value, [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return byte == '\t' ||
               (byte >= 0x20 && byte != 0x7F);  // HTAB, printable ASCII and obs-text
    });
}

}  // namespace

std::string_view reason_phrase(int status) noexcept {
    switch (status) {
    case 101:
        return "Switching Protocols";
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 408:
        return "Request Timeout";
    case 414:
        return "URI Too Long";
    case 426:
        return "Upgrade Required";
    case 429:
        return "Too Many Requests";
    case 431:
        return "Request Header Fields Too Large";
    case 500:
        return "Internal Server Error";
    case 503:
        return "Service Unavailable";
    case 505:
        return "HTTP Version Not Supported";
    default:
        return "";
    }
}

Result<std::string> serialize(const HttpResponse& response) {
    if (response.status < kMinStatus || response.status > kMaxStatus) {
        return fail(ErrorCode::InvalidArgument, "status out of range");
    }
    std::string out = "HTTP/1.1 " + std::to_string(response.status) + " ";
    out += reason_phrase(response.status);
    out += "\r\n";
    for (const HttpHeader& header : response.headers) {
        if (!is_token(header.name)) {
            return fail(ErrorCode::InvalidArgument, "invalid response header name");
        }
        if (!is_valid_value(header.value)) {
            return fail(ErrorCode::InvalidArgument, "control character in response header value");
        }
        out += header.name;
        out += ": ";
        out += header.value;
        out += "\r\n";
    }
    out += "\r\n";
    return out;
}

}  // namespace wsrv

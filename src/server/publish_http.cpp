// SPDX-License-Identifier: Apache-2.0
#include "server/publish_http.hpp"

namespace wsrv {

namespace {

std::string_view reason_of(int status) noexcept {
    switch (status) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 411:
        return "Length Required";
    case 413:
        return "Content Too Large";
    case 415:
        return "Unsupported Media Type";
    case 417:
        return "Expectation Failed";
    case 429:
        return "Too Many Requests";
    case 501:
        return "Not Implemented";
    case 503:
        return "Service Unavailable";
    default:
        return "Internal Server Error";
    }
}

std::string counts(const PublishReport& report) {
    return "subscribers=" + std::to_string(report.subscribers) +
           " accepted=" + std::to_string(report.accepted) +
           " refused=" + std::to_string(report.refused) + "\n";
}

}  // namespace

PublishAnswer publish_answer(const Result<PublishReport>& outcome) {
    if (outcome) {
        const bool all_refused = outcome->subscribers > 0 && outcome->accepted == 0;
        return {all_refused ? 429 : 200, counts(*outcome)};
    }
    switch (outcome.error().code) {
    case ErrorCode::InvalidUtf8:
        return {415, "a text message must be UTF-8 (use binary=1 for other bytes)\n"};
    case ErrorCode::InvalidArgument:
        return {400, "not a valid topic or message\n"};
    case ErrorCode::BufferFull:
    case ErrorCode::Backpressure:
        return {429, "the server is full, try again later\n"};
    default:
        return {503, "cannot publish now\n"};
    }
}

std::string publish_response(int status, std::string_view body) {
    std::string out;
    out.reserve(160 + body.size());
    out += "HTTP/1.1 ";
    out += std::to_string(status);
    out += ' ';
    out += reason_of(status);
    out += "\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: ";
    out += std::to_string(body.size());
    out += "\r\nConnection: close\r\n";
    if (status == 401) {
        out += "WWW-Authenticate: Bearer\r\n";
    }
    out += "\r\n";
    out += body;
    return out;
}

std::optional<Metric> publish_metric(int status) noexcept {
    switch (status) {
    case 200:
        return Metric::PublishHttp200;
    case 400:
        return Metric::PublishHttp400;
    case 401:
        return Metric::PublishHttp401;
    case 404:
        return Metric::PublishHttp404;
    case 405:
        return Metric::PublishHttp405;
    case 411:
        return Metric::PublishHttp411;
    case 413:
        return Metric::PublishHttp413;
    case 415:
        return Metric::PublishHttp415;
    case 417:
        return Metric::PublishHttp417;
    case 429:
        return Metric::PublishHttp429;
    case 501:
        return Metric::PublishHttp501;
    case 503:
        return Metric::PublishHttp503;
    default:
        return std::nullopt;
    }
}

}  // namespace wsrv

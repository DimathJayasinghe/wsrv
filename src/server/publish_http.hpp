// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/bytes.hpp"
#include "base/error.hpp"
#include "base/metrics.hpp"
#include "base/publish_report.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace wsrv {

/// The publish endpoint's logic without sockets (ADR-0028 section 3 and amendment 2).

/// The head of a publish request is at most this long; a longer one is answered 400.
inline constexpr std::size_t kMaxPublishHeadBytes = 4096;
/// A token shorter than this is refused at start: a guessable credential is no credential.
inline constexpr std::size_t kMinPublishTokenBytes = 32;

/// One message to publish, already authenticated and bounded. Views point into the request.
struct PublishMessage {
    std::string_view topic;
    ByteView payload;
    bool binary = false;
    std::uint64_t except = 0;  ///< connection id to leave out; 0 = nobody
};

/// What the endpoint needs from the composition root: the credential and the one action it may
/// take. Borrowed by the server for its whole life; the token is never logged or copied out.
struct PublishSources {
    std::string token;  ///< at least kMinPublishTokenBytes
    /// Called on loop 0 with an authenticated, bounded message. Errors: InvalidUtf8 (a text body
    /// that is not UTF-8), InvalidArgument (a bad topic), anything else means "try again later".
    std::function<Result<PublishReport>(const PublishMessage&)> publish;
};

/// An HTTP status and its text/plain body.
struct PublishAnswer {
    int status = 200;
    std::string body;
};

/// The answer to a publish that reached the broker: 200 with `subscribers=<n> accepted=<n>
/// refused=<n>`; 429 when there were subscribers and every delivery was refused (their loops'
/// queues are full: retry later); 415 for a text body that is not UTF-8; 400 for a bad topic;
/// 503 for anything else.
[[nodiscard]] PublishAnswer publish_answer(const Result<PublishReport>& outcome);

/// The complete HTTP/1.1 response (always `Connection: close`; 401 also says `WWW-Authenticate:
/// Bearer`). `status` must be one of the statuses this endpoint answers.
[[nodiscard]] std::string publish_response(int status, std::string_view body);

/// The counter for an answered status (wsrv_publish_http_total{status}); nothing for a status the
/// endpoint never sends.
[[nodiscard]] std::optional<Metric> publish_metric(int status) noexcept;

}  // namespace wsrv

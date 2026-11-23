// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "http/request.hpp"
#include "http/request_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace wsrv {

/// What `POST /publish` asks for (ADR-0028 section 3 and amendments 1 and 2).
struct PublishRequest {
    std::string topic;               ///< raw, not yet judged: the broker owns the topic rules
    std::uint64_t except = 0;        ///< connection id to leave out; 0 = nobody
    bool binary = false;             ///< false = a text message (the body must be UTF-8)
    std::size_t content_length = 0;  ///< declared body size; NOT yet checked against any bound
    std::string bearer;              ///< the credential after "Bearer "; compare, never log it
};

/// Read a publish request out of an already parsed head. The head parser (HttpRequestParser) has
/// done the syntax: CRLF only, no obs-fold, no control characters, token names. This adds the
/// rules of a request that carries a body, where an ambiguity is a request-smuggling hole
/// (RFC 9112 section 6.3, RFC 9110 section 10.1.1). Nothing here allocates from the declared
/// length, and nothing here decides whether the credential is right: the caller compares it, then
/// applies its size bound, in that order, so an unauthenticated peer can never make it reserve
/// memory.
///
/// Errors, in the order they are checked (the status is what to answer, then close):
///   405 not POST; 404 not exactly /publish; 401 no, repeated or malformed Authorization;
///   501 any Transfer-Encoding; 417 any Expect; 411 no Content-Length; 400 a repeated or
///   non-decimal Content-Length (even two equal ones), or a query that is not exactly
///   topic=...[&except=<digits>][&binary=0|1] with no repeats, no unknown names and no '%'.
/// May throw std::bad_alloc (it copies the topic and credential, both inside the 4 KiB head); the
/// endpoint catches it and drops the client, as the admin listener does.
/// Thread-affinity: pure, any-thread.
[[nodiscard]] HttpResult<PublishRequest> interpret_publish(const HttpRequest& head);

}  // namespace wsrv

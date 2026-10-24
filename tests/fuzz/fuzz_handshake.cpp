// SPDX-License-Identifier: Apache-2.0
// libFuzzer target: for any bytes the parser accepts, the negotiator must decide without crashing
// and the response we would send must be well formed (no injected header lines, correct framing).
#include "base/assert.hpp"
#include "base/bytes.hpp"
#include "crypto/base64.hpp"
#include "http/request_parser.hpp"
#include "protocol/handshake.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace {

std::size_t count_occurrences(std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    for (std::size_t pos = text.find(needle); pos != std::string_view::npos;
         pos = text.find(needle, pos + 1)) {
        ++count;
    }
    return count;
}

/// An application rule built from the raw input: whatever it asks for (any status, any header name
/// or value, including CR/LF/NUL), the negotiator must turn into a safe, well-formed answer.
class HostileRule final : public wsrv::IHandshakeRule {
public:
    explicit HostileRule(wsrv::ByteView raw) : raw_{raw} {}
    [[nodiscard]] std::optional<wsrv::HandshakeReject>
    evaluate(const wsrv::HttpRequest& /*request*/, wsrv::HandshakeContext& context) const override {
        if (raw_.size() < 3) {
            return std::nullopt;
        }
        context.annotations["raw-size"] = std::to_string(raw_.size());
        const auto status =
            100 + (std::to_integer<int>(raw_[0]) * 3);  // 100..865: valid and invalid statuses
        const std::size_t cut =
            std::min<std::size_t>(1 + std::to_integer<std::size_t>(raw_[1]) % 24, raw_.size() - 2);
        wsrv::HandshakeReject refusal;
        refusal.status = status;
        refusal.headers.push_back({std::string{wsrv::as_chars(raw_.subspan(2, cut))},
                                   std::string{wsrv::as_chars(raw_.subspan(2 + cut))}});
        if (std::to_integer<unsigned>(raw_[0]) % 4 == 0) {
            return std::nullopt;  // sometimes the rule just passes
        }
        return refusal;
    }

private:
    wsrv::ByteView raw_;  // valid for the duration of one fuzz iteration
};

void check_response(const wsrv::HandshakeDecision& decision) {
    const wsrv::HttpResponse response = wsrv::to_response(decision);
    const auto text = wsrv::serialize(response);
    WSRV_ASSERT(text.has_value());  // every decision the negotiator makes must be serialisable
    WSRV_ASSERT(wsrv::render_response(decision) == *text);  // ...and rendering agrees
    // Framing: status line + one line per header + blank line, each ended by exactly one CRLF,
    // nothing injected.
    WSRV_ASSERT(count_occurrences(*text, "\r\n") == response.headers.size() + 2);
    WSRV_ASSERT(text->ends_with("\r\n\r\n"));
    WSRV_ASSERT(count_occurrences(*text, "\n") == count_occurrences(*text, "\r\n"));
    if (std::holds_alternative<wsrv::HandshakeAccept>(decision)) {
        WSRV_ASSERT(text->starts_with("HTTP/1.1 101 "));
        WSRV_ASSERT(text->find("Sec-WebSocket-Extensions") ==
                    std::string::npos);  // offers are never echoed
    } else {
        WSRV_ASSERT(text->find("Connection: close\r\n") != std::string::npos ||
                    text->find("Connection: Upgrade, close\r\n") != std::string::npos);
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const wsrv::ByteView all = std::as_bytes(std::span<const std::uint8_t>{data, size});
    wsrv::HttpRequestParser parser;
    const auto progress = parser.feed(all);
    if (!progress || !progress->complete) {
        return 0;
    }
    const wsrv::HttpRequest& request = parser.request();

    wsrv::HandshakeConfig strict;  // secure defaults
    wsrv::HandshakeConfig
        open;  // permissive origin policy, a subprotocol list, strict subprotocol requirement
    open.allow_any_origin = true;
    open.subprotocols = {"chat", "superchat"};
    open.require_subprotocol = true;

    wsrv::HandshakeConfig hostile = open;
    hostile.user_rules.push_back(std::make_shared<HostileRule>(all));

    for (const wsrv::HandshakeConfig& config : {strict, open, hostile}) {
        const wsrv::HandshakeDecision decision =
            wsrv::HandshakeNegotiator{config}.negotiate(request);
        check_response(decision);
        if (const auto* accept = std::get_if<wsrv::HandshakeAccept>(&decision)) {
            // An accept implies a well-formed single key header (cross-check the rule independently
            // here).
            const auto keys = request.values("Sec-WebSocket-Key");
            WSRV_ASSERT(keys.size() == 1);
            const auto decoded = wsrv::base64_decode(keys.front());
            WSRV_ASSERT(decoded.has_value() && decoded->size() == 16);
            WSRV_ASSERT(accept->accept_key == wsrv::compute_accept_key(keys.front()));
        }
    }
    return 0;
}

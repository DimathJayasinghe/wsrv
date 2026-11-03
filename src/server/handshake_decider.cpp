// SPDX-License-Identifier: Apache-2.0
#include "server/handshake_decider.hpp"

#include "http/token.hpp"

#include <algorithm>
#include <new>
#include <utility>

namespace wsrv {

namespace {

[[nodiscard]] std::string first_value(const HttpRequest& request, std::string_view name) {
    const std::vector<std::string_view> values = request.values(name);
    return values.empty() ? std::string{} : std::string{values.front()};
}

[[nodiscard]] std::vector<std::string> offered_subprotocols(const HttpRequest& request) {
    std::vector<std::string> offers;
    for (const std::string_view line : request.values("Sec-WebSocket-Protocol")) {
        for (const std::string_view token : split_list(line)) {
            offers.emplace_back(token);  // the negotiator already refused non-tokens and repeats
        }
    }
    return offers;
}

/// @return false if a listed header is too big; `total` counts every byte copied.
[[nodiscard]] bool copy_forwarded(const HttpRequest& request,
                                  const std::vector<std::string>& names,
                                  std::vector<HttpHeader>& out,
                                  std::size_t& total) {
    for (const std::string& name : names) {
        for (const std::string_view value : request.values(name)) {
            total += name.size() + value.size();
            if (value.size() > kMaxForwardedValueBytes || total > kMaxHandshakeRequestBytes) {
                return false;
            }
            out.push_back(HttpHeader{name, std::string{value}});
        }
    }
    return true;
}

}  // namespace

std::optional<HandshakeRequest> make_handshake_request(const HandshakeFacts& facts,
                                                       const std::vector<std::string>& forward) {
    const HttpRequest& request = facts.request;
    HandshakeRequest out;
    out.id = facts.id;
    out.peer = facts.peer;
    const std::string_view target = request.target;
    const std::size_t question = target.find('?');
    out.path = std::string{target.substr(0, question)};
    if (question != std::string_view::npos) {
        out.query = std::string{target.substr(question + 1)};
    }
    out.origin = first_value(request, "Origin");
    out.host = first_value(request, "Host");
    out.offered_subprotocols = offered_subprotocols(request);
    out.subprotocol = std::string{facts.negotiated_subprotocol};
    std::size_t total = out.path.size() + out.query.size() + out.origin.size() + out.host.size();
    if (!copy_forwarded(request, forward, out.forwarded, total)) {
        return std::nullopt;
    }
    return out;
}

Result<void> PendingHandle::accept(std::optional<std::string_view> subprotocol) const {
    HandshakeVerdict verdict;
    verdict.accept = true;
    if (subprotocol) {
        const bool offered =
            offered_ != nullptr && std::ranges::find(*offered_, *subprotocol) != offered_->end();
        if (!offered) {
            return fail(ErrorCode::InvalidArgument,
                        "handshake: the subprotocol was not offered by the client");
        }
        verdict.subprotocol = std::string{*subprotocol};
    }
    return send(std::move(verdict));
}

Result<void> PendingHandle::reject(int status) const {
    if (!is_decision_status(status)) {
        return fail(ErrorCode::InvalidArgument, "handshake: status is not an allowed refusal");
    }
    HandshakeVerdict verdict;
    verdict.accept = false;
    verdict.status = status;
    return send(std::move(verdict));
}

Result<void> PendingHandle::send(HandshakeVerdict verdict) const {
    if (link_ == nullptr) {
        return fail(ErrorCode::Closing, "handshake: not connected");
    }
    return link_->submit_decision(id_, std::move(verdict));
}

}  // namespace wsrv

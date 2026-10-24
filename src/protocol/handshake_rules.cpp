// SPDX-License-Identifier: Apache-2.0
#include "protocol/handshake_rules.hpp"

#include "crypto/base64.hpp"
#include "http/host.hpp"
#include "http/token.hpp"

#include <algorithm>
#include <functional>
#include <utility>

namespace wsrv {

namespace {

using Verdict = std::optional<HandshakeReject>;

constexpr std::size_t kKeyBytes = 16;        // a Sec-WebSocket-Key is Base64 of 16 random bytes...
constexpr std::size_t kKeyBase64Chars = 24;  // ...which is exactly 24 characters
constexpr std::string_view kSupportedVersion = "13";

[[nodiscard]] Verdict reject(int status, std::vector<HttpHeader> headers = {}) {
    return HandshakeReject{status, std::move(headers)};
}

/// A rule backed by a function: each requirement is a small named function, the chain is a list of
/// them.
class FunctionRule final : public IHandshakeRule {
public:
    using Fn = std::function<Verdict(const HttpRequest&, HandshakeContext&)>;
    explicit FunctionRule(Fn fn) : fn_{std::move(fn)} {}

    [[nodiscard]] Verdict evaluate(const HttpRequest& request,
                                   HandshakeContext& context) const override {
        return fn_(request, context);
    }

private:
    Fn fn_;
};

// ---- request shape (EC-H01..H06, H16)
// ------------------------------------------------------------------

Verdict check_method(const HttpRequest& request, HandshakeContext& /*context*/) {
    return request.method == "GET" ? Verdict{} : reject(405, {{"Allow", "GET"}});
}

Verdict check_http_version(const HttpRequest& request, HandshakeContext& /*context*/) {
    return request.version_minor >= 1 ? Verdict{}
                                      : reject(400);  // the upgrade mechanism needs HTTP/1.1
}

Verdict check_host(const HttpRequest& request, HandshakeContext& /*context*/) {
    const std::vector<std::string_view> hosts = request.values("Host");
    return (hosts.size() == 1 && is_valid_host_header(hosts.front())) ? Verdict{} : reject(400);
}

/// An upgrade request has no body; anything that frames one (or is ambiguous about it) is
/// request-smuggling material (SECURITY_DESIGN T10).
Verdict check_no_body(const HttpRequest& request, HandshakeContext& /*context*/) {
    const std::vector<std::string_view> lengths = request.values("Content-Length");
    if (lengths.size() > 1 || (lengths.size() == 1 && lengths.front() != "0")) {
        return reject(400);
    }
    return request.count("Transfer-Encoding") == 0 ? Verdict{} : reject(400);
}

Verdict
has_list_token(const HttpRequest& request, std::string_view header, std::string_view token) {
    const std::vector<std::string_view> values = request.values(header);
    const bool found = std::ranges::any_of(
        values, [token](std::string_view v) { return contains_token(v, token); });
    return found ? Verdict{} : reject(400);
}

Verdict check_upgrade(const HttpRequest& request, HandshakeContext& /*context*/) {
    return has_list_token(request, "Upgrade", "websocket");
}

Verdict check_connection(const HttpRequest& request, HandshakeContext& /*context*/) {
    return has_list_token(request, "Connection", "upgrade");
}

// ---- key and version (EC-H07, H08)
// ---------------------------------------------------------------------

Verdict check_key(const HttpRequest& request, HandshakeContext& /*context*/) {
    const std::vector<std::string_view> keys = request.values("Sec-WebSocket-Key");
    if (keys.size() != 1) {
        return reject(400);
    }
    if (keys.front().size() != kKeyBase64Chars) {
        return reject(400);  // cheap length check before any decoding
    }
    const Result<std::vector<std::byte>> decoded = base64_decode(keys.front());
    return (decoded.has_value() && decoded->size() == kKeyBytes) ? Verdict{} : reject(400);
}

Verdict check_ws_version(const HttpRequest& request, HandshakeContext& /*context*/) {
    const std::vector<std::string_view> versions = request.values("Sec-WebSocket-Version");
    if (versions.size() != 1) {
        return reject(400);  // missing, or duplicated header lines (ambiguous)
    }
    if (versions.front() != kSupportedVersion) {
        return reject(
            426,
            {{"Sec-WebSocket-Version", std::string{kSupportedVersion}}, {"Upgrade", "websocket"}});
    }
    return {};
}

// ---- policy (EC-H10..H13)
// --------------------------------------------------------------------------------

Verdict check_origin(const HandshakeConfig& config, const HttpRequest& request) {
    const std::vector<std::string_view> origins = request.values("Origin");
    if (origins.size() > 1) {
        return reject(400);
    }
    if (config.allow_any_origin) {
        return {};
    }
    if (origins.empty()) {
        return config.allow_missing_origin ? Verdict{} : reject(403);
    }
    const bool allowed = std::ranges::any_of(
        config.allowed_origins,
        [origin = origins.front()](const std::string& a) { return iequals(a, origin); });
    return allowed ? Verdict{} : reject(403);
}

/// All offered subprotocol tokens across every header line, or nullopt if any is not a token or
/// repeats.
std::optional<std::vector<std::string_view>>
collect_subprotocol_offers(const HttpRequest& request) {
    std::vector<std::string_view> offers;
    for (const std::string_view line : request.values("Sec-WebSocket-Protocol")) {
        for (const std::string_view token : split_list(line)) {
            const bool duplicate = std::ranges::find(offers, token) != offers.end();
            if (!is_token(token) || duplicate || offers.size() >= kMaxSubprotocolOffers) {
                return std::nullopt;
            }
            offers.push_back(token);
        }
    }
    return offers;
}

Verdict check_subprotocol(const HandshakeConfig& config,
                          const HttpRequest& request,
                          HandshakeContext& context) {
    const auto offers = collect_subprotocol_offers(request);
    if (!offers) {
        return reject(400);
    }
    for (const std::string& mine : config.subprotocols) {  // the server's order of preference wins
        if (std::ranges::find(*offers, mine) != offers->end()) {
            context.subprotocol = mine;
            return {};
        }
    }
    return config.require_subprotocol ? reject(400) : Verdict{};
}

/// Origin policy (cross-site WebSocket hijacking defence); owns its copy of the policy.
class OriginRule final : public IHandshakeRule {
public:
    explicit OriginRule(HandshakeConfig config) : config_{std::move(config)} {}
    [[nodiscard]] Verdict evaluate(const HttpRequest& request,
                                   HandshakeContext& /*context*/) const override {
        return check_origin(config_, request);
    }

private:
    HandshakeConfig config_;
};

/// Subprotocol selection by server preference; owns its copy of the policy.
class SubprotocolRule final : public IHandshakeRule {
public:
    explicit SubprotocolRule(HandshakeConfig config) : config_{std::move(config)} {}
    [[nodiscard]] Verdict evaluate(const HttpRequest& request,
                                   HandshakeContext& context) const override {
        return check_subprotocol(config_, request, context);
    }

private:
    HandshakeConfig config_;
};

Verdict check_extensions(const HttpRequest& request, HandshakeContext& context) {
    for (const std::string_view line : request.values("Sec-WebSocket-Extensions")) {
        std::optional<std::vector<ExtensionOffer>> offers = parse_extension_offers(line);
        if (!offers) {
            return reject(400);
        }
        std::ranges::move(*offers, std::back_inserter(context.extension_offers));
        if (context.extension_offers.size() > kMaxExtensionOffers) {
            return reject(400);  // work cap across all header lines
        }
    }
    return {};
}

}  // namespace

std::vector<std::unique_ptr<IHandshakeRule>> make_builtin_rules(const HandshakeConfig& config) {
    std::vector<std::unique_ptr<IHandshakeRule>> rules;
    rules.push_back(std::make_unique<FunctionRule>(check_method));
    rules.push_back(std::make_unique<FunctionRule>(check_http_version));
    rules.push_back(std::make_unique<FunctionRule>(check_host));
    rules.push_back(std::make_unique<FunctionRule>(check_no_body));
    rules.push_back(std::make_unique<FunctionRule>(check_upgrade));
    rules.push_back(std::make_unique<FunctionRule>(check_connection));
    rules.push_back(std::make_unique<FunctionRule>(check_key));
    rules.push_back(std::make_unique<FunctionRule>(check_ws_version));
    rules.push_back(std::make_unique<OriginRule>(config));
    rules.push_back(std::make_unique<SubprotocolRule>(config));
    rules.push_back(std::make_unique<FunctionRule>(check_extensions));
    return rules;
}

}  // namespace wsrv

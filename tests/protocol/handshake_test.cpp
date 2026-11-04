// SPDX-License-Identifier: Apache-2.0
#include "protocol/handshake.hpp"

#include "http/response.hpp"
#include "http/token.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using wsrv::HandshakeAccept;
using wsrv::HandshakeConfig;
using wsrv::HandshakeContext;
using wsrv::HandshakeDecision;
using wsrv::HandshakeNegotiator;
using wsrv::HandshakeReject;
using wsrv::HttpHeader;
using wsrv::HttpRequest;

constexpr std::string_view kRfcKey = "dGhlIHNhbXBsZSBub25jZQ==";
constexpr std::string_view kRfcAccept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

HttpRequest valid_request() {
    HttpRequest request;
    request.method = "GET";
    request.target = "/chat";
    request.version_minor = 1;
    request.headers = {{"Host", "example.com"},
                       {"Upgrade", "websocket"},
                       {"Connection", "Upgrade"},
                       {"Sec-WebSocket-Key", std::string{kRfcKey}},
                       {"Sec-WebSocket-Version", "13"},
                       {"Origin", "https://app.example.com"}};
    return request;
}

HandshakeConfig default_config() {
    HandshakeConfig config;
    config.allowed_origins = {"https://app.example.com"};
    return config;
}

/// Remove every header called `name` (case-insensitive).
HttpRequest without(HttpRequest request, std::string_view name) {
    std::erase_if(request.headers,
                  [name](const HttpHeader& h) { return wsrv::iequals(h.name, name); });
    return request;
}

HttpRequest with(HttpRequest request, std::string name, std::string value) {
    request.headers.push_back({std::move(name), std::move(value)});
    return request;
}

HttpRequest replaced(HttpRequest request, const std::string& name, std::string value) {
    return with(without(std::move(request), name), name, std::move(value));
}

int status_of(const HandshakeDecision& decision) {
    if (std::holds_alternative<HandshakeAccept>(decision)) {
        return 101;
    }
    return std::get<HandshakeReject>(decision).status;
}

int run_status(const HttpRequest& request, const HandshakeConfig& config = default_config()) {
    return status_of(HandshakeNegotiator{config}.negotiate(request));
}

bool has_header(const HandshakeReject& reject, std::string_view name, std::string_view value) {
    return std::ranges::any_of(reject.headers, [&](const HttpHeader& h) {
        return wsrv::iequals(h.name, name) && h.value == value;
    });
}

// ---- accepting
// ------------------------------------------------------------------------------------------

TEST(Handshake, AcceptKeyMatchesTheRfcSample) {  // EC-H09
    EXPECT_EQ(wsrv::compute_accept_key(kRfcKey), kRfcAccept);
}

TEST(Handshake, AcceptsAValidRequestAndProducesTheRfcResponse) {
    const HandshakeDecision decision =
        HandshakeNegotiator{default_config()}.negotiate(valid_request());
    ASSERT_TRUE(std::holds_alternative<HandshakeAccept>(decision));
    const auto& accept = std::get<HandshakeAccept>(decision);
    EXPECT_EQ(accept.accept_key, kRfcAccept);
    EXPECT_TRUE(accept.subprotocol.empty());

    const auto text = wsrv::serialize(wsrv::to_response(decision));
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(*text,
              "HTTP/1.1 101 Switching Protocols\r\n"
              "Upgrade: websocket\r\n"
              "Connection: Upgrade\r\n"
              "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
              "\r\n");
}

// ---- EC-H01..H06: request shape
// --------------------------------------------------------------------------

TEST(Handshake, NonGetMethodsAreRejectedWith405AndAllowGet) {  // EC-H01
    for (const char* method : {"POST", "HEAD", "PUT", "OPTIONS", "get"}) {
        HttpRequest request = valid_request();
        request.method = method;
        const HandshakeDecision decision = HandshakeNegotiator{default_config()}.negotiate(request);
        ASSERT_EQ(status_of(decision), 405) << method;
        EXPECT_TRUE(has_header(std::get<HandshakeReject>(decision), "Allow", "GET")) << method;
    }
}

TEST(Handshake, HttpOneZeroIsRejected) {  // EC-H02
    HttpRequest request = valid_request();
    request.version_minor = 0;
    EXPECT_EQ(run_status(request), 400);
}

TEST(Handshake, HostMustBePresentExactlyOnceAndNonEmpty) {  // EC-H03, EC-H04
    EXPECT_EQ(run_status(without(valid_request(), "Host")), 400);
    EXPECT_EQ(run_status(replaced(valid_request(), "Host", "")), 400);
    EXPECT_EQ(run_status(with(valid_request(), "Host", "evil.example")), 400);
}

TEST(Handshake, UpgradeHeaderMustNameWebSocketInAnyCase) {  // EC-H05
    EXPECT_EQ(run_status(without(valid_request(), "Upgrade")), 400);
    EXPECT_EQ(run_status(replaced(valid_request(), "Upgrade", "h2c")), 400);
    EXPECT_EQ(run_status(replaced(valid_request(), "Upgrade", "websockets")), 400);
    EXPECT_EQ(run_status(replaced(valid_request(), "Upgrade", "WebSocket")), 101);
    EXPECT_EQ(run_status(replaced(valid_request(), "Upgrade", "WEBSOCKET")), 101);
    EXPECT_EQ(run_status(replaced(valid_request(), "Upgrade", "foo, websocket")), 101);
}

TEST(Handshake, ConnectionHeaderMustContainTheUpgradeToken) {  // EC-H06
    EXPECT_EQ(run_status(without(valid_request(), "Connection")), 400);
    EXPECT_EQ(run_status(replaced(valid_request(), "Connection", "keep-alive")), 400);
    EXPECT_EQ(run_status(replaced(valid_request(), "Connection", "upgraded")), 400);
    EXPECT_EQ(run_status(replaced(valid_request(), "Connection", "keep-alive, Upgrade")), 101);
    EXPECT_EQ(run_status(replaced(valid_request(), "Connection", "UPGRADE")), 101);
}

TEST(Handshake, RulesRunInOrderSoTheMethodIsCheckedBeforeEverythingElse) {
    HttpRequest request = without(valid_request(), "Sec-WebSocket-Key");
    request.method = "POST";
    EXPECT_EQ(run_status(request), 405);
}

// ---- EC-H07, EC-H08: key and version
// ----------------------------------------------------------------------

TEST(Handshake, KeyMustBeBase64OfExactlySixteenBytes) {  // EC-H07
    EXPECT_EQ(run_status(without(valid_request(), "Sec-WebSocket-Key")), 400);
    EXPECT_EQ(run_status(replaced(valid_request(), "Sec-WebSocket-Key", "")), 400);
    EXPECT_EQ(run_status(replaced(valid_request(), "Sec-WebSocket-Key", "not base64!")), 400);
    EXPECT_EQ(run_status(replaced(valid_request(), "Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25j")),
              400);  // 15 bytes
    EXPECT_EQ(
        run_status(replaced(valid_request(), "Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZWU=")),
        400);  // 17 bytes
    EXPECT_EQ(
        run_status(replaced(valid_request(), "Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZR==")),
        400);  // non-canonical
    EXPECT_EQ(run_status(with(valid_request(), "Sec-WebSocket-Key", std::string{kRfcKey})),
              400);  // duplicate
}

TEST(Handshake, OtherVersionsGet426WithTheSupportedVersion) {  // EC-H08
    for (const char* value : {"8", "12", "14", "013", "13, 8", "abc", ""}) {
        const HandshakeDecision decision = HandshakeNegotiator{default_config()}.negotiate(
            replaced(valid_request(), "Sec-WebSocket-Version", value));
        ASSERT_EQ(status_of(decision), 426) << value;
        EXPECT_TRUE(has_header(std::get<HandshakeReject>(decision), "Sec-WebSocket-Version", "13"))
            << value;
    }
    EXPECT_EQ(run_status(without(valid_request(), "Sec-WebSocket-Version")), 400);
    EXPECT_EQ(run_status(with(valid_request(), "Sec-WebSocket-Version", "13")),
              400);  // duplicate header line
}

// ---- EC-H10, EC-H11: Origin (cross-site WebSocket hijacking)
// ---------------------------------------------

TEST(Handshake, OriginMustBeAllowListed) {  // EC-H10
    EXPECT_EQ(run_status(replaced(valid_request(), "Origin", "https://evil.example")), 403);
    EXPECT_EQ(run_status(replaced(valid_request(), "Origin", "http://app.example.com")),
              403);  // other scheme
    EXPECT_EQ(run_status(replaced(valid_request(), "Origin", "https://app.example.com:8443")), 403);
    EXPECT_EQ(run_status(replaced(valid_request(), "Origin", "null")), 403);
    EXPECT_EQ(run_status(replaced(valid_request(), "Origin", "HTTPS://APP.EXAMPLE.COM")), 101);
}

TEST(Handshake, ThePolicyIsSecureByDefault) {
    EXPECT_EQ(run_status(valid_request(), HandshakeConfig{}),
              403);  // Origin present, nothing allowed
}

TEST(Handshake, AllowAnyOriginIsAnExplicitOptIn) {
    HandshakeConfig config;
    config.allow_any_origin = true;
    EXPECT_EQ(run_status(replaced(valid_request(), "Origin", "https://evil.example"), config), 101);
    EXPECT_EQ(run_status(replaced(valid_request(), "Origin", "null"), config), 101);
    EXPECT_EQ(run_status(without(valid_request(), "Origin"), config), 101);
}

TEST(Handshake, MissingOriginIsRejectedUnlessAllowed) {  // EC-H11
    EXPECT_EQ(run_status(without(valid_request(), "Origin")), 403);
    HandshakeConfig config = default_config();
    config.allow_missing_origin = true;
    EXPECT_EQ(run_status(without(valid_request(), "Origin"), config), 101);
    EXPECT_EQ(run_status(replaced(valid_request(), "Origin", "https://evil.example"), config),
              403);  // still checked
}

TEST(Handshake, MultipleOriginHeadersAreAmbiguousAndRejected) {
    EXPECT_EQ(run_status(with(valid_request(), "Origin", "https://app.example.com")), 400);
}

// ---- EC-H12: subprotocols
// ----------------------------------------------------------------------------------

HandshakeConfig chat_config() {
    HandshakeConfig config = default_config();
    config.subprotocols = {"chat", "superchat"};
    return config;
}

std::string selected(const HttpRequest& request, const HandshakeConfig& config) {
    const HandshakeDecision decision = HandshakeNegotiator{config}.negotiate(request);
    return std::holds_alternative<HandshakeAccept>(decision)
               ? std::get<HandshakeAccept>(decision).subprotocol
               : "<rejected>";
}

TEST(Handshake, ServerPreferenceWinsOverClientOrder) {
    EXPECT_EQ(
        selected(with(valid_request(), "Sec-WebSocket-Protocol", "superchat, chat"), chat_config()),
        "chat");
    EXPECT_EQ(selected(with(valid_request(), "Sec-WebSocket-Protocol", "superchat"), chat_config()),
              "superchat");
}

TEST(Handshake, OffersAcrossSeveralHeaderLinesAreCombined) {
    HttpRequest request = with(valid_request(), "Sec-WebSocket-Protocol", "other");
    request = with(std::move(request), "Sec-WebSocket-Protocol", "superchat");
    EXPECT_EQ(selected(request, chat_config()), "superchat");
}

TEST(Handshake, NoMatchMeansNoSubprotocolUnlessRequired) {
    EXPECT_EQ(selected(with(valid_request(), "Sec-WebSocket-Protocol", "unknown"), chat_config()),
              "");
    EXPECT_EQ(selected(valid_request(), chat_config()), "");  // client offered nothing
    HandshakeConfig strict = chat_config();
    strict.require_subprotocol = true;
    EXPECT_EQ(selected(with(valid_request(), "Sec-WebSocket-Protocol", "unknown"), strict),
              "<rejected>");
    EXPECT_EQ(selected(valid_request(), strict), "<rejected>");
    EXPECT_EQ(selected(with(valid_request(), "Sec-WebSocket-Protocol", "chat"), strict), "chat");
}

TEST(Handshake, InvalidOrDuplicateSubprotocolTokensAreRejected) {
    for (const char* offer : {"ch@t", "chat, ch at", "chat;x=1", "chat, chat", "\"chat\""}) {
        EXPECT_EQ(run_status(with(valid_request(), "Sec-WebSocket-Protocol", offer), chat_config()),
                  400)
            << offer;
    }
}

TEST(Handshake, SelectedSubprotocolIsEchoedAndNothingElse) {
    const HandshakeDecision decision = HandshakeNegotiator{chat_config()}.negotiate(
        with(valid_request(), "Sec-WebSocket-Protocol", "superchat, chat"));
    const auto text = wsrv::serialize(wsrv::to_response(decision));
    ASSERT_TRUE(text.has_value());
    EXPECT_NE(text->find("Sec-WebSocket-Protocol: chat\r\n"), std::string::npos);
}

// ---- EC-H13: extension offers
// -----------------------------------------------------------------------------

TEST(Handshake, ExtensionOffersAreNotAcceptedUnlessConfigured) {
    const HandshakeDecision decision = HandshakeNegotiator{default_config()}.negotiate(
        with(valid_request(),
             "Sec-WebSocket-Extensions",
             "permessage-deflate; client_max_window_bits, x-unknown"));
    ASSERT_TRUE(std::holds_alternative<HandshakeAccept>(decision));
    const auto& accept = std::get<HandshakeAccept>(decision);
    ASSERT_EQ(accept.extension_offers.size(), 2U);
    EXPECT_EQ(accept.extension_offers[0].name, "permessage-deflate");
    const auto text = wsrv::serialize(wsrv::to_response(decision));
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(text->find("Sec-WebSocket-Extensions"),
              std::string::npos);  // no policy configured: no extension
}

TEST(Handshake, PermessageDeflateIsAnsweredWhenConfiguredAndOffered) {  // EC-D01
    HandshakeConfig config = default_config();
    config.permessage_deflate = wsrv::DeflatePolicy{};
    const HandshakeDecision decision = HandshakeNegotiator{config}.negotiate(
        with(valid_request(), "Sec-WebSocket-Extensions", "x-unknown, permessage-deflate"));
    ASSERT_TRUE(std::holds_alternative<HandshakeAccept>(decision));
    const auto& accept = std::get<HandshakeAccept>(decision);
    ASSERT_TRUE(accept.deflate.has_value());
    EXPECT_TRUE(accept.deflate->params.server_no_context_takeover);
    const auto text = wsrv::serialize(wsrv::to_response(decision));
    ASSERT_TRUE(text.has_value());
    EXPECT_NE(
        text->find("Sec-WebSocket-Extensions: permessage-deflate; server_no_context_takeover; "
                   "server_max_window_bits=12\r\n"),
        std::string::npos);
    EXPECT_EQ(text->find("x-unknown"), std::string::npos);
}

TEST(Handshake, NoDeflateAnswerWhenTheClientDidNotOfferItOrNoOfferIsAcceptable) {
    HandshakeConfig config = default_config();
    config.permessage_deflate = wsrv::DeflatePolicy{};
    for (const char* offers : {"x-unknown", "permessage-deflate; bogus"}) {
        const HandshakeDecision decision = HandshakeNegotiator{config}.negotiate(
            with(valid_request(), "Sec-WebSocket-Extensions", offers));
        ASSERT_TRUE(std::holds_alternative<HandshakeAccept>(decision))
            << offers;  // never a refusal
        EXPECT_FALSE(std::get<HandshakeAccept>(decision).deflate.has_value()) << offers;
        const auto text = wsrv::serialize(wsrv::to_response(decision));
        ASSERT_TRUE(text.has_value());
        EXPECT_EQ(text->find("Sec-WebSocket-Extensions"), std::string::npos) << offers;
    }
    const HandshakeDecision plain = HandshakeNegotiator{config}.negotiate(valid_request());
    ASSERT_TRUE(std::holds_alternative<HandshakeAccept>(plain));
    EXPECT_FALSE(std::get<HandshakeAccept>(plain).deflate.has_value());
}

TEST(Handshake, AnInvalidDeflatePolicyIsAConfigurationError) {
    HandshakeConfig config = default_config();
    wsrv::DeflatePolicy policy;
    policy.server_max_window_bits = 8;
    config.permessage_deflate = policy;
    EXPECT_THROW(HandshakeNegotiator{config}, std::invalid_argument);
}

TEST(Handshake, MalformedExtensionOffersAreRejected) {
    for (const char* value : {"permessage-deflate;", "a; b=c; b=d", "a; b=\"open", ";x"}) {
        EXPECT_EQ(run_status(with(valid_request(), "Sec-WebSocket-Extensions", value)), 400)
            << value;
    }
}

// ---- EC-H16: request bodies and smuggling shapes
// ---------------------------------------------------------

TEST(Handshake, UpgradeRequestsMustNotCarryABody) {
    EXPECT_EQ(run_status(with(valid_request(), "Content-Length", "0")), 101);
    EXPECT_EQ(run_status(with(valid_request(), "Content-Length", "5")), 400);
    EXPECT_EQ(run_status(with(valid_request(), "Content-Length", "abc")), 400);
    EXPECT_EQ(run_status(with(valid_request(), "Content-Length", "-0")), 400);
    EXPECT_EQ(run_status(with(valid_request(), "Content-Length", "")), 400);
    EXPECT_EQ(run_status(with(with(valid_request(), "Content-Length", "0"), "Content-Length", "0")),
              400);
    EXPECT_EQ(run_status(with(valid_request(), "Transfer-Encoding", "chunked")), 400);
    EXPECT_EQ(run_status(with(
                  with(valid_request(), "Content-Length", "0"), "Transfer-Encoding", "chunked")),
              400);
}

// ---- EC-H25: application hooks
// -----------------------------------------------------------------------------

class LambdaRule final : public wsrv::IHandshakeRule {
public:
    using Fn = std::function<std::optional<HandshakeReject>(const HttpRequest&)>;
    explicit LambdaRule(Fn fn) : fn_{std::move(fn)} {}
    [[nodiscard]] std::optional<HandshakeReject>
    evaluate(const HttpRequest& request, HandshakeContext& /*context*/) const override {
        return fn_(request);
    }

private:
    Fn fn_;
};

TEST(Handshake, UserRuleCanRejectWithItsOwnStatusAndHeaders) {
    HandshakeConfig config = default_config();
    config.user_rules.push_back(std::make_shared<LambdaRule>(
        [](const HttpRequest&) { return HandshakeReject{401, {{"WWW-Authenticate", "Bearer"}}}; }));
    const HandshakeDecision decision = HandshakeNegotiator{config}.negotiate(valid_request());
    ASSERT_EQ(status_of(decision), 401);
    EXPECT_TRUE(has_header(std::get<HandshakeReject>(decision), "WWW-Authenticate", "Bearer"));
}

struct AuthBackendDown {};  // an application-defined exception type, unrelated to std::exception

TEST(Handshake, UserRuleThatThrowsAnythingBecomesA500AndTheServerSurvives) {
    // The barrier must contain every kind of exception: a custom type and a primitive.
    HandshakeConfig custom = default_config();
    custom.user_rules.push_back(std::make_shared<LambdaRule>(
        [](const HttpRequest&) -> std::optional<HandshakeReject> { throw AuthBackendDown{}; }));
    EXPECT_EQ(run_status(valid_request(), custom), 500);

    HandshakeConfig primitive = default_config();
    primitive.user_rules.push_back(std::make_shared<LambdaRule>(
        [](const HttpRequest&) -> std::optional<HandshakeReject> { throw 42; }));
    EXPECT_EQ(run_status(valid_request(), primitive), 500);
}

TEST(Handshake, UserRuleThrowingAStdExceptionBecomesA500) {
    HandshakeConfig config = default_config();
    config.user_rules.push_back(
        std::make_shared<LambdaRule>([](const HttpRequest&) -> std::optional<HandshakeReject> {
            throw std::runtime_error("auth backend down");
        }));
    EXPECT_EQ(run_status(valid_request(), config), 500);
}

TEST(Handshake, UserRulesSeeOnlyRequestsThatPassedEveryBuiltInRule) {
    int calls = 0;
    HandshakeConfig config = default_config();
    config.user_rules.push_back(std::make_shared<LambdaRule>([&calls](const HttpRequest&) {
        ++calls;
        return std::optional<HandshakeReject>{};
    }));
    EXPECT_EQ(run_status(without(valid_request(), "Host"), config), 400);
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(run_status(valid_request(), config), 101);
    EXPECT_EQ(calls, 1);
}

TEST(Handshake, UserRulesRunInOrderAndTheFirstRejectionWins) {
    HandshakeConfig config = default_config();
    config.user_rules.push_back(std::make_shared<LambdaRule>([](const HttpRequest&) {
        return std::optional<HandshakeReject>{HandshakeReject{401, {}}};
    }));
    config.user_rules.push_back(std::make_shared<LambdaRule>([](const HttpRequest&) {
        return std::optional<HandshakeReject>{HandshakeReject{403, {}}};
    }));
    EXPECT_EQ(run_status(valid_request(), config), 401);
}

// ---- rejection responses
// ----------------------------------------------------------------------------------

TEST(Handshake, EveryRejectionClosesTheConnectionAndHasNoBody) {
    const HandshakeDecision decision =
        HandshakeNegotiator{default_config()}.negotiate(without(valid_request(), "Host"));
    const auto text = wsrv::serialize(wsrv::to_response(decision));
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(*text, "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
}

TEST(Handshake, RejectionKeepsTheRulesExtraHeaders) {
    const HandshakeDecision decision = HandshakeNegotiator{default_config()}.negotiate(
        replaced(valid_request(), "Sec-WebSocket-Version", "8"));
    const auto text = wsrv::serialize(wsrv::to_response(decision));
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(*text,
              "HTTP/1.1 426 Upgrade Required\r\nSec-WebSocket-Version: 13\r\nUpgrade: websocket\r\n"
              "Connection: Upgrade, close\r\nContent-Length: 0\r\n\r\n");  // RFC 9110 15.5.22
}

// ---- review findings: Host validity, user-rule boundary, caps, config
// -------------------------------------

TEST(Handshake, HostMustBeAValidHostValue) {  // FR-H1, RFC 9112 section 3.2
    EXPECT_EQ(run_status(replaced(valid_request(), "Host", "a b, c")), 400);
    EXPECT_EQ(run_status(replaced(valid_request(), "Host", "ex@mple.com")), 400);
    EXPECT_EQ(run_status(replaced(valid_request(), "Host", "example.com:99999")), 400);
    EXPECT_EQ(run_status(replaced(valid_request(), "Host", "[::1]:8080")), 101);
    EXPECT_EQ(run_status(replaced(valid_request(), "Host", "example.com:8080")), 101);
}

struct RefusalCase {
    HandshakeReject refusal;
    const char* why;
};

TEST(Handshake, AUserRulesRefusalIsValidatedBeforeItCanBeSent) {
    const std::vector<RefusalCase> invalid{
        {HandshakeReject{999, {}}, "status out of range"},
        {HandshakeReject{101, {}}, "a refusal must not claim Switching Protocols"},
        {HandshakeReject{200, {}}, "a refusal must not claim success"},
        {HandshakeReject{302, {}}, "redirects are not offered to user rules"},
        {HandshakeReject{401, {{"X", "a\r\nSet-Cookie: stolen=1"}}}, "CRLF in a header value"},
        {HandshakeReject{401, {{"Bad Name", "v"}}}, "header name is not a token"},
        {HandshakeReject{401, {{"Content-Length", "5"}}}, "would duplicate the framing header"},
        {HandshakeReject{401, {{"content-length", "0"}}}, "framing headers belong to the server"},
        {HandshakeReject{401, {{"Connection", "keep-alive"}}}, "the server decides about closing"},
        {HandshakeReject{401, {{"Transfer-Encoding", "chunked"}}}, "framing header"},
        {HandshakeReject{401, {{"Upgrade", "websocket"}}}, "upgrade headers belong to the server"},
        {HandshakeReject{401, {{"Sec-WebSocket-Accept", "x"}}},
         "handshake headers belong to the server"},
        {HandshakeReject{401, {{"sec-websocket-protocol", "x"}}},
         "handshake headers belong to the server"},
    };
    for (const RefusalCase& c : invalid) {
        HandshakeConfig config = default_config();
        config.user_rules.push_back(
            std::make_shared<LambdaRule>([refusal = c.refusal](const HttpRequest&) {
                return std::optional<HandshakeReject>{refusal};
            }));
        const HandshakeDecision decision = HandshakeNegotiator{config}.negotiate(valid_request());
        ASSERT_EQ(status_of(decision), 500) << c.why;
        EXPECT_FALSE(std::get<HandshakeReject>(decision).from_exception) << c.why;
    }
}

TEST(Handshake, AUserRuleMayUseOrdinaryHeadersOnItsRefusal) {
    HandshakeConfig config = default_config();
    config.user_rules.push_back(std::make_shared<LambdaRule>([](const HttpRequest&) {
        return std::optional<HandshakeReject>{
            HandshakeReject{429, {{"Retry-After", "30"}, {"Set-Cookie", "a=b"}}}};
    }));
    const HandshakeDecision decision = HandshakeNegotiator{config}.negotiate(valid_request());
    ASSERT_EQ(status_of(decision), 429);
    EXPECT_TRUE(has_header(std::get<HandshakeReject>(decision), "Retry-After", "30"));
}

class ContextRule final : public wsrv::IHandshakeRule {
public:
    using Fn = std::function<void(HandshakeContext&)>;
    explicit ContextRule(Fn fn) : fn_{std::move(fn)} {}
    [[nodiscard]] std::optional<HandshakeReject>
    evaluate(const HttpRequest& /*request*/, HandshakeContext& context) const override {
        fn_(context);
        return std::nullopt;
    }

private:
    Fn fn_;
};

TEST(Handshake, AUserRuleCannotInventOrChangeTheNegotiatedSubprotocolOrExtensions) {
    HandshakeConfig config = chat_config();
    config.user_rules.push_back(std::make_shared<ContextRule>(
        [](HandshakeContext& context) { context.subprotocol = "notoffered"; }));
    EXPECT_EQ(run_status(with(valid_request(), "Sec-WebSocket-Protocol", "chat"), config), 500);

    HandshakeConfig extensions = default_config();
    extensions.user_rules.push_back(std::make_shared<ContextRule>([](HandshakeContext& context) {
        context.extension_offers.emplace_back(wsrv::ExtensionOffer{"x-injected", {}});
    }));
    EXPECT_EQ(run_status(valid_request(), extensions), 500);
}

TEST(Handshake, AUserRuleCanAnnotateTheAcceptedConnection) {  // FR-H6
    HandshakeConfig config = default_config();
    config.user_rules.push_back(std::make_shared<ContextRule>([](HandshakeContext& context) {
        context.annotations["user"] = "alice";
        context.annotations["scope"] = "chat:read";
    }));
    const HandshakeDecision decision = HandshakeNegotiator{config}.negotiate(valid_request());
    ASSERT_TRUE(std::holds_alternative<HandshakeAccept>(decision));
    const auto& accept = std::get<HandshakeAccept>(decision);
    EXPECT_EQ(accept.annotations.at("user"), "alice");
    EXPECT_EQ(accept.annotations.at("scope"), "chat:read");
    const std::string text = wsrv::render_response(decision);
    EXPECT_EQ(text.find("alice"),
              std::string::npos);  // annotations are for the application, never sent
}

TEST(Handshake, AThrownExceptionIsFlaggedSoTheConnectionLayerCanLogIt) {
    HandshakeConfig config = default_config();
    config.user_rules.push_back(std::make_shared<LambdaRule>(
        [](const HttpRequest&) -> std::optional<HandshakeReject> { throw 1; }));
    const HandshakeDecision decision = HandshakeNegotiator{config}.negotiate(valid_request());
    ASSERT_EQ(status_of(decision), 500);
    EXPECT_TRUE(std::get<HandshakeReject>(decision).from_exception);
}

TEST(Handshake, SubprotocolOffersAreCappedAgainstAmplification) {
    std::string at_cap;
    for (std::size_t i = 0; i < wsrv::kMaxSubprotocolOffers; ++i) {
        at_cap += (i == 0 ? "p" : ", p") + std::to_string(i);
    }
    EXPECT_EQ(run_status(with(valid_request(), "Sec-WebSocket-Protocol", at_cap), chat_config()),
              101);
    EXPECT_EQ(run_status(with(valid_request(), "Sec-WebSocket-Protocol", at_cap + ", one-too-many"),
                         chat_config()),
              400);
}

TEST(Handshake, ExtensionOffersAcrossHeaderLinesAreCappedToo) {
    HttpRequest request = valid_request();
    for (std::size_t i = 0; i <= wsrv::kMaxExtensionOffers;
         ++i) {  // one offer per header line, one too many
        request = with(std::move(request), "Sec-WebSocket-Extensions", "e" + std::to_string(i));
    }
    EXPECT_EQ(run_status(request), 400);
}

TEST(Handshake, ARenderedResponseExistsForEveryDecision) {
    EXPECT_EQ(
        wsrv::render_response(HandshakeNegotiator{default_config()}.negotiate(valid_request())),
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n");
    // A decision that cannot be serialised (built by hand here) still yields a well-formed 500.
    const HandshakeDecision broken = HandshakeReject{1234, {}};
    EXPECT_EQ(
        wsrv::render_response(broken),
        "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
}

TEST(Handshake, ABadConfigurationFailsAtConstructionNotAtRequestTime) {  // ADR-0006: config errors
                                                                         // are startup errors
    auto bad = [](auto mutate) {
        HandshakeConfig config = default_config();
        mutate(config);
        return config;
    };
    EXPECT_NO_THROW(HandshakeNegotiator{default_config()});
    EXPECT_NO_THROW(HandshakeNegotiator{chat_config()});
    EXPECT_NO_THROW(HandshakeNegotiator{
        bad([](HandshakeConfig& c) { c.allowed_origins.emplace_back("null"); })});
    EXPECT_THROW(
        HandshakeNegotiator{bad([](HandshakeConfig& c) { c.user_rules.push_back(nullptr); })},
        std::invalid_argument);
    EXPECT_THROW(
        HandshakeNegotiator{bad([](HandshakeConfig& c) { c.subprotocols = {"ok", "not ok"}; })},
        std::invalid_argument);
    EXPECT_THROW(
        HandshakeNegotiator{bad([](HandshakeConfig& c) { c.subprotocols = {"chat", "chat"}; })},
        std::invalid_argument);
    for (const char* origin : {"https://a.example/",
                               "*",
                               "",
                               "app.example.com",
                               "https://a b.example",
                               "https://a.example\r\n"}) {
        EXPECT_THROW(HandshakeNegotiator{bad(
                         [origin](HandshakeConfig& c) { c.allowed_origins = {origin}; })},
                     std::invalid_argument)
            << origin;
    }
}

}  // namespace

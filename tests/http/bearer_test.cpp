// SPDX-License-Identifier: Apache-2.0
// ADR-0041 section 4: one definition of "the credential of this request", shared by the publish
// endpoint and the admin listener. A credential is exactly one Authorization header, the scheme
// Bearer (any case, RFC 9110 section 11.1), exactly one space, and a value with no space and no
// control character. Anything else is "no credential": the endpoints answer 401 to all of it, so a
// peer learns nothing about which rule it broke. Each rule below is one way a looser reading would
// let a request through that a stricter reading refuses.
#include "http/bearer.hpp"

#include "base/bytes.hpp"
#include "http/request.hpp"
#include "http/request_parser.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

using wsrv::bearer_credential;
using wsrv::HttpRequest;

/// A request head with the given header lines, run through the real head parser.
HttpRequest parsed(std::string_view header_lines) {
    const std::string text =
        "GET /metrics HTTP/1.1\r\nHost: wsrvd\r\n" + std::string{header_lines} + "\r\n";
    wsrv::HttpRequestParser parser;
    const auto progress = parser.feed(wsrv::as_bytes(std::string_view{text}));
    EXPECT_TRUE(progress.has_value()) << header_lines;
    EXPECT_TRUE(progress.has_value() && progress->complete) << header_lines;
    return parser.request();
}

/// A request whose header value is exactly `value`, without the parser's own syntax checks, to
/// reach bytes the parser would already have refused (defence in depth: this function must not
/// depend on the parser having done its job).
HttpRequest with_raw_value(std::string value) {
    HttpRequest request;
    request.method = "GET";
    request.target = "/metrics";
    request.headers.push_back({"Authorization", std::move(value)});
    return request;
}

std::optional<std::string_view> credential_of(const HttpRequest& request) {
    return bearer_credential(request);
}

TEST(BearerCredential, AGoodHeaderGivesTheCredentialAsWritten) {
    const HttpRequest request = parsed("Authorization: Bearer wsrv1_abc-DEF.123~+/=\r\n");
    EXPECT_EQ(credential_of(request), std::optional<std::string_view>{"wsrv1_abc-DEF.123~+/="});
}

TEST(BearerCredential, TheSchemeAndTheHeaderNameAreCaseInsensitive) {
    EXPECT_EQ(credential_of(parsed("authorization: bearer tok\r\n")), "tok");
    EXPECT_EQ(credential_of(parsed("AUTHORIZATION: BEARER tok\r\n")), "tok");
    EXPECT_EQ(credential_of(parsed("Authorization: bEaReR tok\r\n")), "tok");
}

TEST(BearerCredential, NoHeaderIsNoCredential) {
    EXPECT_FALSE(credential_of(parsed("")).has_value());
    EXPECT_FALSE(credential_of(parsed("X-Authorization: Bearer tok\r\n")).has_value())
        << "a header that merely contains the name is another header";
}

// Two Authorization headers are ambiguous: which one does a proxy in front forward, and which does
// the server judge? Even two equal ones are refused, so there is never a choice to get wrong.
TEST(BearerCredential, ARepeatedHeaderIsNoCredentialEvenWhenBothAreTheSame) {
    EXPECT_FALSE(credential_of(parsed("Authorization: Bearer tok\r\nAuthorization: Bearer tok\r\n"))
                     .has_value());
    EXPECT_FALSE(
        credential_of(parsed("Authorization: Bearer good\r\nauthorization: Bearer other\r\n"))
            .has_value());
}

TEST(BearerCredential, AnotherSchemeIsNoCredential) {
    EXPECT_FALSE(credential_of(parsed("Authorization: Basic dXNlcjpwYXNz\r\n")).has_value());
    EXPECT_FALSE(credential_of(parsed("Authorization: Token tok\r\n")).has_value());
    EXPECT_FALSE(credential_of(parsed("Authorization: Bearertok\r\n")).has_value())
        << "no space after the scheme";
}

TEST(BearerCredential, TheSchemeIsFollowedByExactlyOneSpaceAndAnEmptyCredentialIsNone) {
    EXPECT_FALSE(credential_of(parsed("Authorization: Bearer\r\n")).has_value());
    EXPECT_FALSE(credential_of(with_raw_value("Bearer ")).has_value()) << "scheme and space only";
    EXPECT_FALSE(credential_of(with_raw_value("Bearer  tok")).has_value()) << "two spaces";
    EXPECT_FALSE(credential_of(with_raw_value("Bearer\ttok")).has_value())
        << "a tab is not a space";
}

TEST(BearerCredential, ASpaceOrControlCharacterInsideTheCredentialMakesItNone) {
    EXPECT_FALSE(credential_of(with_raw_value("Bearer to ken")).has_value());
    EXPECT_FALSE(credential_of(with_raw_value("Bearer tok ")).has_value()) << "trailing space";
    EXPECT_FALSE(
        credential_of(with_raw_value(std::string{"Bearer to"} + '\0' + "ken")).has_value());
    EXPECT_FALSE(credential_of(with_raw_value("Bearer to\nken")).has_value());
    EXPECT_FALSE(credential_of(with_raw_value("Bearer to\x7f"
                                              "ken"))
                     .has_value());
}

TEST(BearerCredential, TheViewPointsIntoTheRequestItWasReadFrom) {
    const HttpRequest request = parsed("Authorization: Bearer tok\r\n");
    const std::optional<std::string_view> credential = credential_of(request);
    ASSERT_TRUE(credential.has_value());
    const std::string_view value = request.headers.back().value;
    EXPECT_GE(credential->data(), value.data());
    EXPECT_LE(credential->data() + credential->size(), value.data() + value.size());
}

}  // namespace

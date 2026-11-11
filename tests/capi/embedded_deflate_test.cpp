// SPDX-License-Identifier: Apache-2.0
// permessage-deflate through the embedded server (RFC 7692, ADR-0020 section 6): the host turns it
// on with text keys, and sees INFLATED messages and sends plain ones; compression stays inside the
// server. Real zlib on a real loopback port. Built only with zlib.
#include "base/bytes.hpp"
#include "support/peer_deflate.hpp"
#include "support/tcp_client.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "capi/embedded_server.hpp"

namespace {

using namespace wsrv;
using namespace wsrv::test;
using namespace std::chrono_literals;

constexpr std::string_view kOffer =
    "Sec-WebSocket-Extensions: permessage-deflate; client_max_window_bits\r\n";

EmbeddedConfig base_config() {
    EmbeddedConfig config;
    EXPECT_TRUE(config.set("allow_origin", "https://app.example.com").has_value());
    EXPECT_TRUE(config.set("shutdown_grace_ms", "2000").has_value());
    return config;
}

std::unique_ptr<EmbeddedServer> started(const EmbeddedConfig& config) {
    auto server = EmbeddedServer::create(config);
    EXPECT_TRUE(server.has_value());
    EXPECT_TRUE((*server)->start().has_value());
    return std::move(*server);
}

/// Letters from a fixed pseudo-random sequence: compresses only about 2:1, so an honest message
/// stays well under the ratio limit.
std::string varied_text(std::size_t size) {
    std::string text(size, ' ');
    std::uint32_t state = 2463534242U;
    for (char& c : text) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        c = static_cast<char>('a' + (state % 16U));
    }
    return text;
}

std::string as_string(const std::vector<std::byte>& bytes) {
    return std::string{as_chars(bytes)};
}

TEST(EmbeddedDeflate, WithTheKeyOnTheHostSeesInflatedTextAndSendsPlainText) {
    EmbeddedConfig config = base_config();
    ASSERT_TRUE(config.set("permessage_deflate", "true").has_value());
    const auto server = started(config);
    TcpClient client{server->port()};
    ASSERT_TRUE(client.valid());
    const std::string head = client.upgrade_head(kOffer);
    ASSERT_EQ(head.rfind("HTTP/1.1 101 ", 0), 0U) << head;
    EXPECT_NE(head.find("Sec-WebSocket-Extensions: permessage-deflate"), std::string::npos);
    const auto open = server->poll(3000ms);
    ASSERT_TRUE(open.has_value());
    ASSERT_EQ(open->type, HostEventType::Open);

    const std::string text = varied_text(4000);
    PeerCompressor compressor;
    ASSERT_TRUE(client.send_frame(Opcode::Text, compressor.compress(text), true, true));
    const auto received = server->poll(3000ms);
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(as_string(received->data), text) << "the host never sees compressed bytes";

    ASSERT_TRUE(server->send_text(received->conn, text).has_value());  // the host sends plain text
    const auto reply = client.read_frame();
    ASSERT_TRUE(reply.has_value());
    EXPECT_TRUE(reply->rsv1) << "the server compresses on the way out";
    EXPECT_LT(reply->payload.size(), text.size());
    EXPECT_EQ(PeerDecompressor{}.decompress(reply->payload), text);
}

TEST(EmbeddedDeflate, WithoutTheKeyTheExtensionIsNotOffered) {
    const auto server = started(base_config());
    TcpClient client{server->port()};
    ASSERT_TRUE(client.valid());
    const std::string head = client.upgrade_head(kOffer);
    ASSERT_EQ(head.rfind("HTTP/1.1 101 ", 0), 0U) << head;
    EXPECT_EQ(head.find("permessage-deflate"), std::string::npos) << "off by default";
}

TEST(EmbeddedDeflate, TheTuningKeysReachTheNegotiation) {
    EmbeddedConfig config = base_config();
    ASSERT_TRUE(config.set("permessage_deflate", "true").has_value());
    ASSERT_TRUE(config.set("deflate_server_max_window_bits", "10").has_value());
    ASSERT_TRUE(config.set("deflate_server_no_context_takeover", "false").has_value());
    const auto server = started(config);
    TcpClient client{server->port()};
    ASSERT_TRUE(client.valid());
    const std::string head = client.upgrade_head(kOffer);
    ASSERT_EQ(head.rfind("HTTP/1.1 101 ", 0), 0U) << head;
    EXPECT_NE(head.find("server_max_window_bits=10"), std::string::npos) << head;
    EXPECT_EQ(head.find("server_no_context_takeover"), std::string::npos) << head;
}

TEST(EmbeddedDeflate, ValuesOutsideTheirRangeAreRefusedByStartWithTheReason) {
    // set() only checks that a number is well formed; the ranges are checked once, where the
    // settings are used (ServerBuilder::build), so the rule has one home.
    const std::pair<const char*, const char*> bad[] = {{"deflate_server_max_window_bits", "8"},
                                                       {"deflate_server_max_window_bits", "16"},
                                                       {"deflate_level", "0"},
                                                       {"deflate_level", "10"},
                                                       {"deflate_max_ratio", "0"},
                                                       {"deflate_max_ratio", "1001"}};
    for (const auto& [key, value] : bad) {
        SCOPED_TRACE(std::string{key} + "=" + value);
        EmbeddedConfig config = base_config();
        ASSERT_TRUE(config.set("permessage_deflate", "true").has_value());
        ASSERT_TRUE(config.set(key, value).has_value());
        auto server = EmbeddedServer::create(config);
        ASSERT_TRUE(server.has_value());
        const Result<void> result = (*server)->start();
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, ErrorCode::InvalidConfig);
        EXPECT_FALSE((*server)->start_error().empty());
        EXPECT_FALSE((*server)->running());
    }
}

TEST(EmbeddedDeflate, TheSettingsAreIgnoredWhileTheSwitchIsOff) {
    // A tuning key alone does not turn compression on (and a bad one is not even looked at):
    // only permessage_deflate=true makes the server offer the extension.
    EmbeddedConfig config = base_config();
    ASSERT_TRUE(config.set("deflate_level", "0").has_value());
    const auto server = started(config);
    TcpClient client{server->port()};
    ASSERT_TRUE(client.valid());
    EXPECT_EQ(client.upgrade_head(kOffer).find("permessage-deflate"), std::string::npos);
}

}  // namespace

// SPDX-License-Identifier: Apache-2.0
#include "capi/embedded_config.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

namespace {

using namespace wsrv;
using namespace std::chrono_literals;

TEST(EmbeddedConfig, DefaultsAreTheServerDefaultsOnLoopback) {
    const EmbeddedConfig config;
    EXPECT_EQ(config.host(), "127.0.0.1");
    EXPECT_EQ(config.port(), 0U);
    EXPECT_EQ(config.server().threads, 1U);
    EXPECT_TRUE(config.server().handshake.allowed_origins.empty());  // the safe default: reject
}

TEST(EmbeddedConfig, EveryDocumentedKeyIsAccepted) {
    EmbeddedConfig config;
    EXPECT_TRUE(config.set("listen_host", "0.0.0.0").has_value());
    EXPECT_TRUE(config.set("listen_port", "8080").has_value());
    EXPECT_TRUE(config.set("threads", "4").has_value());
    EXPECT_TRUE(config.set("allow_origin", "https://a.example").has_value());
    EXPECT_TRUE(config.set("allow_origin", "https://b.example").has_value());
    EXPECT_TRUE(config.set("allow_missing_origin", "true").has_value());
    EXPECT_TRUE(config.set("max_message_size", "65536").has_value());
    EXPECT_TRUE(config.set("max_connections", "500").has_value());
    EXPECT_TRUE(config.set("shutdown_grace_ms", "2500").has_value());
    EXPECT_TRUE(config.set("event_queue_events", "1000").has_value());
    EXPECT_TRUE(config.set("event_queue_bytes", "1048576").has_value());

    EXPECT_EQ(config.host(), "0.0.0.0");
    EXPECT_EQ(config.port(), 8080U);
    EXPECT_EQ(config.server().threads, 4U);
    EXPECT_EQ(config.server().handshake.allowed_origins.size(), 2U);  // repeatable
    EXPECT_TRUE(config.server().handshake.allow_missing_origin);
    EXPECT_EQ(config.server().session.assembler.max_message_size, 65536U);
    EXPECT_EQ(config.server().session.outbound.max_queued_bytes, 2U * 65536U);  // an echo fits
    EXPECT_EQ(config.server().max_connections, 500U);
    EXPECT_EQ(config.server().shutdown_grace, 2500ms);
    EXPECT_EQ(config.queue().max_events, 1000U);
    EXPECT_EQ(config.queue().max_bytes, 1048576U);
}

TEST(EmbeddedConfig, TheTlsKeysTakeFilePathsAndAreOffByDefault) {  // ADR-0019 section 7
    EmbeddedConfig config;
    EXPECT_TRUE(config.tls_cert().empty());
    EXPECT_TRUE(config.tls_key().empty());
    EXPECT_FALSE(config.tls_requested());

    EXPECT_FALSE(config.set("tls_cert", "").has_value());  // an empty path is never meant
    EXPECT_FALSE(config.set("tls_key", "").has_value());
    EXPECT_FALSE(config.tls_requested());

    ASSERT_TRUE(config.set("tls_cert", "/etc/wsrv/cert.pem").has_value());
    EXPECT_TRUE(config.tls_requested());  // one key alone already asks for TLS, so start() can
    ASSERT_TRUE(config.set("tls_key", "/etc/wsrv/key.pem").has_value());  // refuse half a setup
    EXPECT_EQ(config.tls_cert(), "/etc/wsrv/cert.pem");
    EXPECT_EQ(config.tls_key(), "/etc/wsrv/key.pem");
}

TEST(EmbeddedConfig, AnUnknownKeyIsRefusedSoATypoCannotSilentlyLoseASetting) {
    EmbeddedConfig config;
    const Result<void> set = config.set("allow_orign", "https://a.example");
    ASSERT_FALSE(set.has_value());
    EXPECT_EQ(set.error().code, ErrorCode::InvalidConfig);
}

TEST(EmbeddedConfig, ValuesThatAreNotCleanNumbersOrBooleansAreRefusedAndChangeNothing) {
    EmbeddedConfig config;
    for (const char* bad : {"", "12x", " 5", "-1", "1e3", "0x10", "99999999999999999999"}) {
        const Result<void> set = config.set("threads", bad);
        ASSERT_FALSE(set.has_value()) << "'" << bad << "'";
        EXPECT_EQ(set.error().code, ErrorCode::InvalidConfig);
    }
    EXPECT_FALSE(config.set("listen_port", "65536").has_value());
    EXPECT_FALSE(config.set("allow_missing_origin", "yes").has_value());
    EXPECT_FALSE(config.set("allow_origin", "").has_value());
    EXPECT_EQ(config.server().threads, 1U);
    EXPECT_EQ(config.port(), 0U);
    EXPECT_FALSE(config.server().handshake.allow_missing_origin);
}

TEST(EmbeddedConfig, BooleansAcceptOnlyTrueFalseOneZero) {
    EmbeddedConfig config;
    for (const char* yes : {"true", "1"}) {
        ASSERT_TRUE(config.set("allow_missing_origin", yes).has_value());
        EXPECT_TRUE(config.server().handshake.allow_missing_origin);
        ASSERT_TRUE(config.set("allow_missing_origin", "false").has_value());
        EXPECT_FALSE(config.server().handshake.allow_missing_origin);
    }
    ASSERT_TRUE(config.set("allow_missing_origin", "0").has_value());
    EXPECT_FALSE(config.server().handshake.allow_missing_origin);
}

TEST(EmbeddedConfig, TheEventQueueLimitsAreCheckedWhenTheQueueIsBuiltNotHere) {
    // A value that parses is accepted by set(); whether it is usable is decided once, by
    // EventQueue::create at start, so the rule lives in one place.
    EmbeddedConfig config;
    EXPECT_TRUE(config.set("event_queue_events", "1").has_value());
    EXPECT_EQ(config.queue().max_events, 1U);
}

TEST(EmbeddedConfig, TheHandshakeHookIsOffByDefaultAndItsKeysAreChecked) {  // ADR-0018
    EmbeddedConfig config;
    EXPECT_FALSE(config.handshake_hook());
    ASSERT_TRUE(config.set("handshake_hook", "true").has_value());
    EXPECT_TRUE(config.handshake_hook());
    ASSERT_TRUE(config.set("handshake_forward_header", "Cookie").has_value());
    ASSERT_TRUE(config.set("handshake_forward_header", "Authorization").has_value());
    ASSERT_TRUE(config.set("handshake_decision_timeout_ms", "1500").has_value());
    ASSERT_TRUE(config.set("max_pending_handshakes", "32").has_value());
    EXPECT_EQ(config.server().handshake_forward_headers,
              (std::vector<std::string>{"Cookie", "Authorization"}));
    EXPECT_EQ(config.server().handshake_decision_timeout, 1500ms);
    EXPECT_EQ(config.server().max_pending_handshakes, 32U);

    EXPECT_FALSE(config.set("handshake_hook", "maybe").has_value());
    EXPECT_FALSE(config.set("handshake_forward_header", "").has_value());
    EXPECT_FALSE(config.set("handshake_forward_header", "Bad Header").has_value());
    EXPECT_FALSE(config.set("handshake_decision_timeout_ms", "0").has_value())
        << "a zero wait would refuse everyone";
    EXPECT_FALSE(config.set("handshake_decision_timeout_ms", "x").has_value());
    EXPECT_FALSE(config.set("max_pending_handshakes", "0").has_value());
    EXPECT_EQ(config.server().handshake_decision_timeout, 1500ms) << "a refusal changes nothing";
}

}  // namespace

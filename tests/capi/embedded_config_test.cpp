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

// ADR-0037 section 8.
TEST(EmbeddedConfig, LoggingIsAtInfoWithoutTheClientAddressByDefault) {
    const EmbeddedConfig config;
    EXPECT_EQ(config.log_level(), LogLevel::Info);
    EXPECT_FALSE(config.server().log_peer) << "an address is personal data (NFR-O1)";
}

TEST(EmbeddedConfig, LogLevelTakesTheFourLevelsAndNothingElse) {
    EmbeddedConfig config;
    for (const auto& [text, level] : {std::pair{"error", LogLevel::Error},
                                      std::pair{"warn", LogLevel::Warn},
                                      std::pair{"info", LogLevel::Info},
                                      std::pair{"debug", LogLevel::Debug}}) {
        ASSERT_TRUE(config.set("log_level", text).has_value()) << text;
        EXPECT_EQ(config.log_level(), level) << text;
    }
    // "trace" is not offered (it would log payloads); names are exact, so a typo cannot silently
    // mean "info".
    for (const char* bad : {"trace", "verbose", "INFO", " info", "info ", "", "0"}) {
        const auto set = config.set("log_level", bad);
        ASSERT_FALSE(set.has_value()) << "'" << bad << "'";
        EXPECT_EQ(set.error().code, ErrorCode::InvalidConfig);
    }
    EXPECT_EQ(config.log_level(), LogLevel::Debug) << "a refused value changes nothing";
}

TEST(EmbeddedConfig, LogPeerIsABoolean) {
    EmbeddedConfig config;
    ASSERT_TRUE(config.set("log_peer", "true").has_value());
    EXPECT_TRUE(config.server().log_peer);
    ASSERT_TRUE(config.set("log_peer", "false").has_value());
    EXPECT_FALSE(config.server().log_peer);
    EXPECT_FALSE(config.set("log_peer", "yes").has_value());
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

TEST(EmbeddedConfig, TlsSniTakesNameCertAndKeyAndIsRepeatable) {  // ADR-0019 section 7
    EmbeddedConfig config;
    EXPECT_TRUE(config.tls_sni().empty());
    ASSERT_TRUE(config.set("tls_sni", "app.example.com=/c/app.pem:/k/app.pem").has_value());
    ASSERT_TRUE(config.set("tls_sni", "*.example.com=/c/wild.pem:/k/wild.pem").has_value());
    ASSERT_EQ(config.tls_sni().size(), 2U);
    EXPECT_EQ(config.tls_sni()[0].name, "app.example.com");
    EXPECT_EQ(config.tls_sni()[0].certificate, "/c/app.pem");
    EXPECT_EQ(config.tls_sni()[0].key, "/k/app.pem");
    EXPECT_EQ(config.tls_sni()[1].name, "*.example.com");
    EXPECT_TRUE(config.tls_requested());  // SNI alone already asks for TLS (and needs a default)
}

TEST(EmbeddedConfig, MalformedTlsSniValuesAreRefusedAndChangeNothing) {
    for (const char* bad :
         {"",
          "app.example.com",
          "app.example.com=",
          "app.example.com=/c.pem",
          "app.example.com=/c.pem:",
          "app.example.com=:/k.pem",
          "=/c.pem:/k.pem",
          "app.example.com=/a:b.pem:/k.pem"}) {  // a ':' inside a path is ambiguous
        EmbeddedConfig config;
        EXPECT_FALSE(config.set("tls_sni", bad).has_value()) << '"' << bad << '"';
        EXPECT_TRUE(config.tls_sni().empty()) << '"' << bad << '"';
        EXPECT_FALSE(config.tls_requested());
    }
}

TEST(EmbeddedConfig, TlsSniIsBoundedSoTheHostCannotGrowTheConfigWithoutLimit) {
    EmbeddedConfig config;
    for (std::size_t i = 0; i < kMaxTlsSniSettings; ++i) {
        ASSERT_TRUE(
            config.set("tls_sni", "n" + std::to_string(i) + ".example=/c.pem:/k.pem").has_value());
    }
    EXPECT_FALSE(config.set("tls_sni", "one-too-many.example=/c.pem:/k.pem").has_value());
    EXPECT_EQ(config.tls_sni().size(), kMaxTlsSniSettings);
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

TEST(EmbeddedConfig, ThePerIpLimitKeysAreStoredAndZeroSwitchesThemOff) {  // ADR-0021 section 5
    EmbeddedConfig config;
    EXPECT_EQ(config.server().max_connections_per_ip, 100U);  // the documented defaults
    EXPECT_EQ(config.server().max_new_connections_per_ip_per_second, 50U);
    EXPECT_TRUE(config.set("max_connections_per_ip", "7").has_value());
    EXPECT_TRUE(config.set("max_new_connections_per_ip_per_second", "9").has_value());
    EXPECT_EQ(config.server().max_connections_per_ip, 7U);
    EXPECT_EQ(config.server().max_new_connections_per_ip_per_second, 9U);
    EXPECT_TRUE(config.set("max_connections_per_ip", "0").has_value());
    EXPECT_EQ(config.server().max_connections_per_ip, 0U);
}

TEST(EmbeddedConfig, MalformedPerIpLimitsAreRefusedAndChangeNothing) {
    EmbeddedConfig config;
    for (const char* key : {"max_connections_per_ip", "max_new_connections_per_ip_per_second"}) {
        for (const char* bad : {"", "-1", "12x", " 5"}) {
            EXPECT_FALSE(config.set(key, bad).has_value()) << key << "='" << bad << "'";
        }
    }
    EXPECT_FALSE(config.set("max_new_connections_per_ip_per_second", "4294967296").has_value())
        << "does not fit the 32-bit field: refused, not wrapped";
    EXPECT_EQ(config.server().max_connections_per_ip, 100U);
    EXPECT_EQ(config.server().max_new_connections_per_ip_per_second, 50U);
}

TEST(EmbeddedConfig, TrustedProxiesAreOffByDefaultAndCollectRangesInOrder) {  // ADR-0029
    EmbeddedConfig config;
    EXPECT_TRUE(config.server().trusted_proxies.empty());
    EXPECT_EQ(config.server().client_ip_header, "X-Forwarded-For");
    EXPECT_TRUE(config.set("trusted_proxy", "10.0.0.0/8").has_value());
    EXPECT_TRUE(config.set("trusted_proxy", "::1").has_value());
    ASSERT_EQ(config.server().trusted_proxies.size(), 2U);
    EXPECT_EQ(config.server().trusted_proxies[0].prefix, 96U + 8U);  // IPv4 kept as IPv4-mapped
    EXPECT_TRUE(config.set("client_ip_header", "CF-Connecting-IP").has_value());
    EXPECT_EQ(config.server().client_ip_header, "CF-Connecting-IP");
}

TEST(EmbeddedConfig, MalformedTrustedProxySettingsAreRefusedAndChangeNothing) {
    EmbeddedConfig config;
    for (const char* bad : {"", "10.0.0.0/33", "localhost", " 10.0.0.1", "10.0.0.0/8/8", "[::1]"}) {
        EXPECT_FALSE(config.set("trusted_proxy", bad).has_value()) << "'" << bad << "'";
    }
    for (const char* bad : {"", "X Forwarded", "X-Forwarded-For:", "X\r\nFor"}) {
        EXPECT_FALSE(config.set("client_ip_header", bad).has_value()) << "'" << bad << "'";
    }
    EXPECT_TRUE(config.server().trusted_proxies.empty());
    EXPECT_EQ(config.server().client_ip_header, "X-Forwarded-For");
}

TEST(EmbeddedConfig, AtMostSixtyFourTrustedProxiesAreAccepted) {
    EmbeddedConfig config;
    for (int i = 0; i < 64; ++i) {
        ASSERT_TRUE(config.set("trusted_proxy", "10.0." + std::to_string(i) + ".0/24").has_value())
            << i;
    }
    EXPECT_FALSE(config.set("trusted_proxy", "10.1.0.0/24").has_value());
    EXPECT_EQ(config.server().trusted_proxies.size(), 64U);
}

TEST(EmbeddedConfig, TheAdminListenerIsOffUntilAPortIsGiven) {  // ADR-0023 section 3
    EmbeddedConfig config;
    EXPECT_FALSE(config.admin_requested());
    EXPECT_TRUE(config.set("admin_host", "10.0.0.5").has_value());
    EXPECT_FALSE(config.admin_requested()) << "a host alone does not switch it on";
    EXPECT_TRUE(config.set("admin_port", "9100").has_value());
    EXPECT_TRUE(config.admin_requested());
    EXPECT_EQ(config.admin_host(), "10.0.0.5");
    EXPECT_EQ(config.admin_port(), 9100);
}

TEST(EmbeddedConfig, MalformedAdminSettingsAreRefusedAndChangeNothing) {
    EmbeddedConfig config;
    for (const char* bad : {"", "-1", "65536", "80x", " 80"}) {
        EXPECT_FALSE(config.set("admin_port", bad).has_value()) << "'" << bad << "'";
    }
    EXPECT_FALSE(config.set("admin_host", "").has_value());
    EXPECT_FALSE(config.admin_requested());
    EXPECT_EQ(config.admin_host(), "127.0.0.1") << "loopback unless the operator opts out";
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

// A live connection holds one slot of the event queue for its Close event (event_queue.hpp), so a
// queue of 4096 events silently capped the server at 4095 connections whatever max_connections said
// (found by the 1.0 benchmark: 5000 idle connections lost 905). The defaults now follow
// max_connections; a value the operator sets stays exactly as set.
TEST(EmbeddedConfig, TheDefaultEventQueueHoldsEveryConnectionTheServerAccepts) {
    EmbeddedConfig config;  // max_connections 10000
    EXPECT_GT(config.queue().max_events, config.server().max_connections)
        << "every live connection reserves a slot for its Close";
    EXPECT_GE(config.queue().max_bytes,
              config.server().max_connections * EventQueue::kReservedCloseBytes)
        << "and the bytes set aside for it";
}

TEST(EmbeddedConfig, TheDefaultEventQueueGrowsWithMaxConnectionsInEitherOrderOfSetting) {
    EmbeddedConfig config;
    ASSERT_TRUE(config.set("max_connections", "50000").has_value());
    EXPECT_GT(config.queue().max_events, 50000U);
    EXPECT_GE(config.queue().max_bytes, 50000U * EventQueue::kReservedCloseBytes);

    EmbeddedConfig small;
    ASSERT_TRUE(small.set("max_connections", "10").has_value());
    // One slot per connection, plus the old default (4096 events, 8 MiB) as room for events in
    // flight.
    EXPECT_EQ(small.queue().max_events, 10U + 4096U);
    EXPECT_EQ(small.queue().max_bytes,
              10U * EventQueue::kReservedCloseBytes + (std::size_t{8} << 20));
}

TEST(EmbeddedConfig, AnEventQueueTheOperatorSetsIsNeverChanged) {
    EmbeddedConfig config;
    ASSERT_TRUE(config.set("max_connections", "50000").has_value());
    ASSERT_TRUE(config.set("event_queue_events", "100").has_value());
    ASSERT_TRUE(config.set("event_queue_bytes", "4096").has_value());
    EXPECT_EQ(config.queue().max_events, 100U)
        << "an explicit value wins: it is the operator's call";
    EXPECT_EQ(config.queue().max_bytes, 4096U);
}

TEST(EmbeddedConfig, SettingOnlyOneEventQueueLimitLeavesTheOtherDerived) {
    EmbeddedConfig config;
    ASSERT_TRUE(config.set("max_connections", "50000").has_value());
    ASSERT_TRUE(config.set("event_queue_events", "70000").has_value());
    EXPECT_EQ(config.queue().max_events, 70000U);
    EXPECT_GE(config.queue().max_bytes, 50000U * EventQueue::kReservedCloseBytes);
}

TEST(EmbeddedConfig, PermessageDeflateIsOffByDefaultAndItsKeysAreStored) {  // ADR-0020 section 6
    EmbeddedConfig config;
    EXPECT_FALSE(config.permessage_deflate()) << "compression is opt-in (CRIME/BREACH class risk)";
    ASSERT_TRUE(config.set("permessage_deflate", "true").has_value());
    ASSERT_TRUE(config.set("deflate_server_max_window_bits", "11").has_value());
    ASSERT_TRUE(config.set("deflate_server_no_context_takeover", "false").has_value());
    ASSERT_TRUE(config.set("deflate_level", "3").has_value());
    ASSERT_TRUE(config.set("deflate_min_size", "256").has_value());
    ASSERT_TRUE(config.set("deflate_max_ratio", "50").has_value());
    EXPECT_TRUE(config.permessage_deflate());
    const DeflateOptions& options = config.deflate_options();
    EXPECT_EQ(options.policy.server_max_window_bits, 11);
    EXPECT_FALSE(options.policy.server_no_context_takeover);
    EXPECT_EQ(options.level, 3);
    EXPECT_EQ(options.min_compress_size, 256U);
    EXPECT_EQ(options.max_inflate_ratio, 50U);
}

TEST(EmbeddedConfig, TheDeflateDefaultsAreTheAdrDefaults) {  // ADR-0020 section 3
    EmbeddedConfig config;
    ASSERT_TRUE(config.set("permessage_deflate", "1").has_value());
    const DeflateOptions& options = config.deflate_options();
    EXPECT_TRUE(options.policy.server_no_context_takeover);
    EXPECT_EQ(options.policy.server_max_window_bits, 12);
    EXPECT_EQ(options.level, 6);
    EXPECT_EQ(options.min_compress_size, 128U);
    EXPECT_EQ(options.max_inflate_ratio, 100U);
}

TEST(EmbeddedConfig, DeflateTuningKeysDoNotDependOnTheOrderTheyAreGivenIn) {
    EmbeddedConfig config;
    ASSERT_TRUE(config.set("deflate_level", "9").has_value());  // before the switch
    ASSERT_TRUE(config.set("permessage_deflate", "true").has_value());
    EXPECT_EQ(config.deflate_options().level, 9);
    ASSERT_TRUE(config.set("permessage_deflate", "false").has_value());
    EXPECT_FALSE(config.permessage_deflate());
    EXPECT_EQ(config.deflate_options().level, 9) << "switching off keeps the tuning";
}

TEST(EmbeddedConfig, MalformedDeflateValuesAreRefusedAndChangeNothing) {
    EmbeddedConfig config;
    // Each value must fit the field it is stored in: a wrapped number would be a silent setting.
    for (const char* key : {"deflate_server_max_window_bits", "deflate_level"}) {
        for (const char* bad : {"", "x", "-1", " 9", "256", "99999999999999999999"}) {
            const Result<void> set = config.set(key, bad);
            ASSERT_FALSE(set.has_value()) << key << " '" << bad << "'";
            EXPECT_EQ(set.error().code, ErrorCode::InvalidConfig);
        }
    }
    for (const char* bad : {"", "-1", "4294967296", "1e2"}) {
        EXPECT_FALSE(config.set("deflate_max_ratio", bad).has_value()) << "'" << bad << "'";
    }
    EXPECT_FALSE(config.set("deflate_min_size", "-1").has_value());
    EXPECT_FALSE(config.set("permessage_deflate", "yes").has_value());
    EXPECT_FALSE(config.set("deflate_server_no_context_takeover", "maybe").has_value());
    const DeflateOptions defaults;
    EXPECT_FALSE(config.permessage_deflate());
    EXPECT_EQ(config.deflate_options().level, defaults.level);
    EXPECT_EQ(config.deflate_options().max_inflate_ratio, defaults.max_inflate_ratio);
    EXPECT_EQ(config.deflate_options().policy.server_max_window_bits,
              defaults.policy.server_max_window_bits);
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

TEST(EmbeddedConfig, ThePendingSendBudgetKeyIsCheckedAndANoOpOnRefusal) {  // ADR-0032
    EmbeddedConfig config;
    EXPECT_EQ(config.server().max_pending_send_bytes, std::size_t{64} << 20);
    ASSERT_TRUE(config.set("max_pending_send_bytes", "1048576").has_value());
    EXPECT_EQ(config.server().max_pending_send_bytes, 1048576U);
    EXPECT_FALSE(config.set("max_pending_send_bytes", "0").has_value()) << "no unlimited value";
    EXPECT_FALSE(config.set("max_pending_send_bytes", "x").has_value());
    EXPECT_FALSE(config.set("max_pending_send_bytes", "2147483648").has_value()) << "above 1 GiB";
    EXPECT_EQ(config.server().max_pending_send_bytes, 1048576U) << "a refusal changes nothing";
}

// ---- ADR-0016 A1: the three limits a conformance run raises ---------------------------------

TEST(EmbeddedConfig, TheThreeLimitKeysDefaultToTheLibraryDefaults) {
    const EmbeddedConfig config;
    EXPECT_EQ(config.server().session.parser.max_frame_payload, std::uint64_t{1} << 20);
    EXPECT_EQ(config.server().session.assembler.max_fragments, 1024U);
    EXPECT_EQ(config.server().session.max_data_frames_per_second, 10000U);
}

TEST(EmbeddedConfig, TheThreeLimitKeysAreStored) {
    EmbeddedConfig config;
    ASSERT_TRUE(config.set("max_frame_payload", "20971520").has_value());
    ASSERT_TRUE(config.set("max_fragments", "1048576").has_value());
    ASSERT_TRUE(config.set("max_data_frames_per_second", "2000000").has_value());
    EXPECT_EQ(config.server().session.parser.max_frame_payload, std::uint64_t{20} << 20);
    EXPECT_EQ(config.server().session.assembler.max_fragments, std::size_t{1} << 20);
    EXPECT_EQ(config.server().session.max_data_frames_per_second, 2'000'000U);
}

TEST(EmbeddedConfig, TheThreeLimitKeysRefuseZeroAndAnythingAboveTheirCeiling) {
    EmbeddedConfig config;
    // Zero would mean "off" for the frame rate and is meaningless for the others: no unlimited
    // value (SECURITY_DESIGN section 3).
    for (const char* key : {"max_frame_payload", "max_fragments", "max_data_frames_per_second"}) {
        EXPECT_FALSE(config.set(key, "0").has_value()) << key;
        EXPECT_FALSE(config.set(key, "-1").has_value()) << key;
        EXPECT_FALSE(config.set(key, "12x").has_value()) << key;
        EXPECT_FALSE(config.set(key, "").has_value()) << key;
    }
    EXPECT_TRUE(config.set("max_frame_payload", "67108864").has_value());  // 64 MiB: the ceiling
    EXPECT_FALSE(config.set("max_frame_payload", "67108865").has_value());
    EXPECT_TRUE(config.set("max_fragments", "16777216").has_value());
    EXPECT_FALSE(config.set("max_fragments", "16777217").has_value());
    EXPECT_TRUE(config.set("max_data_frames_per_second", "10000000").has_value());
    EXPECT_FALSE(config.set("max_data_frames_per_second", "10000001").has_value());
}

TEST(EmbeddedConfig, ARefusedLimitChangesNothing) {
    EmbeddedConfig config;
    ASSERT_TRUE(config.set("max_fragments", "5000").has_value());
    EXPECT_FALSE(config.set("max_fragments", "0").has_value());
    EXPECT_EQ(config.server().session.assembler.max_fragments, 5000U);
}

TEST(EmbeddedConfig, TheBrokerIsOffByDefaultWithTheLibraryBounds) {  // ADR-0027 section 1
    const EmbeddedConfig config;
    EXPECT_FALSE(config.broker_enabled());
    const BrokerConfig defaults;
    EXPECT_EQ(config.broker_config().max_topics, defaults.max_topics);
    EXPECT_EQ(config.broker_config().max_subscribers_per_topic, defaults.max_subscribers_per_topic);
    EXPECT_EQ(config.broker_config().max_topics_per_connection, defaults.max_topics_per_connection);
    EXPECT_EQ(config.broker_config().max_subscriptions, defaults.max_subscriptions);
    EXPECT_EQ(config.broker_config().max_payload_bytes, defaults.max_payload_bytes);
}

TEST(EmbeddedConfig, TheBrokerKeysAreStored) {
    EmbeddedConfig config;
    EXPECT_TRUE(config.set("broker", "true").has_value());
    EXPECT_TRUE(config.set("broker_max_topics", "10").has_value());
    EXPECT_TRUE(config.set("broker_max_subscribers_per_topic", "20").has_value());
    EXPECT_TRUE(config.set("broker_max_topics_per_connection", "3").has_value());
    EXPECT_TRUE(config.set("broker_max_subscriptions", "400").has_value());
    EXPECT_TRUE(config.set("broker_max_payload_bytes", "2048").has_value());
    EXPECT_TRUE(config.broker_enabled());
    EXPECT_EQ(config.broker_config().max_topics, 10U);
    EXPECT_EQ(config.broker_config().max_subscribers_per_topic, 20U);
    EXPECT_EQ(config.broker_config().max_topics_per_connection, 3U);
    EXPECT_EQ(config.broker_config().max_subscriptions, 400U);
    EXPECT_EQ(config.broker_config().max_payload_bytes, 2048U);
}

TEST(EmbeddedConfig, TheBrokerBoundsRefuseZeroAndAnythingAboveTheirCeiling) {
    struct Case {
        const char* key;
        const char* ceiling;
        const char* above;
    };
    const Case cases[] = {{"broker_max_topics", "1000000", "1000001"},
                          {"broker_max_subscribers_per_topic", "1000000", "1000001"},
                          {"broker_max_topics_per_connection", "1024", "1025"},
                          {"broker_max_subscriptions", "10000000", "10000001"},
                          {"broker_max_payload_bytes", "1048576", "1048577"}};
    for (const Case& c : cases) {
        EmbeddedConfig config;
        EXPECT_FALSE(config.set(c.key, "0").has_value()) << c.key;  // never "unlimited"
        EXPECT_FALSE(config.set(c.key, c.above).has_value()) << c.key;
        EXPECT_FALSE(config.set(c.key, "-1").has_value()) << c.key;
        EXPECT_FALSE(config.set(c.key, "10 ").has_value()) << c.key;
        EXPECT_TRUE(config.set(c.key, c.ceiling).has_value()) << c.key;
    }
    EmbeddedConfig bad;
    EXPECT_FALSE(bad.set("broker", "yes").has_value());
    EXPECT_FALSE(bad.broker_enabled());
}

}  // namespace

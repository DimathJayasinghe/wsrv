// SPDX-License-Identifier: Apache-2.0
#include "server/config.hpp"

#include "server/handler.hpp"
#include "server/server_builder.hpp"
#include "server/session_handle.hpp"
#include "support/identity_codecs.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string_view>

namespace {

using namespace std::chrono_literals;
using wsrv::ErrorCode;
using wsrv::ServerConfig;
using wsrv::validate;

/// The context of the error validate() returns, or "" if the config is valid.
std::string_view complaint(const ServerConfig& config) {
    const wsrv::Result<void> result = validate(config);
    return result ? std::string_view{} : result.error().context;
}

TEST(ServerConfig, DefaultsMatchTheRequirementsTable) {  // REQUIREMENTS section 4
    const ServerConfig config;
    EXPECT_EQ(config.max_connections, 10000U);
    EXPECT_EQ(config.handshake_timeout, 5s);
    EXPECT_EQ(config.http.max_total_bytes, 8U * 1024U);
    EXPECT_EQ(config.http.max_headers, 64U);
    EXPECT_EQ(config.session.parser.max_frame_payload, 1U << 20U);
    EXPECT_EQ(config.session.assembler.max_message_size, 1U << 20U);
    EXPECT_EQ(config.session.assembler.max_fragments, 1024U);
    EXPECT_EQ(config.session.outbound.max_frame_payload, 64U * 1024U);
    EXPECT_EQ(config.session.outbound.max_queued_bytes, 1U << 20U);
    EXPECT_EQ(config.session.idle_timeout, 60s);
    EXPECT_EQ(config.session.ping_interval, 30s);
    EXPECT_EQ(config.session.pong_timeout, 10s);
    EXPECT_EQ(config.session.incomplete_message_timeout, 30s);
    EXPECT_EQ(config.session.close_timeout, 5s);
    EXPECT_EQ(config.session.max_control_frames_per_second, 100U);
    EXPECT_EQ(config.session.max_data_frames_per_second, 10000U);
    EXPECT_EQ(config.slow_consumer, wsrv::SlowConsumerPolicy::Close);  // EC-N09 default
    EXPECT_FALSE(config.session.assembler.streaming);  // buffered delivery by default
}

TEST(ServerConfig, TheDefaultsAreValid) {
    EXPECT_TRUE(validate(ServerConfig{}).has_value());
}

TEST(ServerConfig, TheHandshakeHookDefaultsAreFailClosedAndBounded) {  // ADR-0018
    const ServerConfig config;
    EXPECT_EQ(config.handshake_decision_timeout, 3000ms);
    EXPECT_EQ(config.max_pending_handshakes, 256U);
    EXPECT_TRUE(config.handshake_forward_headers.empty()) << "secrets are forwarded only if listed";
}

TEST(ServerConfig, TheDecisionTimeoutMustBeBetween100MsAnd60Seconds) {
    ServerConfig config;
    config.handshake_decision_timeout = 99ms;
    EXPECT_EQ(complaint(config), "handshake_decision_timeout must be in [100 ms, 60 s] (ADR-0018)");
    config.handshake_decision_timeout = 60001ms;
    EXPECT_NE(complaint(config), "");
    config.handshake_decision_timeout = 100ms;
    EXPECT_EQ(complaint(config), "");
    config.handshake_decision_timeout = 60000ms;
    EXPECT_EQ(complaint(config), "");
}

TEST(ServerConfig, ThePendingSendBudgetHasABoundedDefaultAndIsRangeChecked) {  // ADR-0032
    wsrv::ServerConfig config;
    EXPECT_EQ(config.max_pending_send_bytes, std::size_t{64} << 20) << "per loop";
    EXPECT_TRUE(wsrv::validate(config).has_value());
    config.max_pending_send_bytes = 0;
    EXPECT_FALSE(wsrv::validate(config).has_value()) << "no unlimited value";
    config.max_pending_send_bytes = wsrv::kMaxConfiguredQueue + 1;
    EXPECT_FALSE(wsrv::validate(config).has_value());
    config.max_pending_send_bytes = wsrv::kMaxConfiguredQueue;
    EXPECT_TRUE(wsrv::validate(config).has_value());
    config.max_pending_send_bytes = 1;
    EXPECT_TRUE(wsrv::validate(config).has_value()) << "tiny is allowed: it only refuses sends";
}

TEST(ServerConfig, PendingHandshakesNeedAPositiveBoundThatNeverExceedsTheConnectionCap) {
    ServerConfig config;
    config.max_pending_handshakes = 0;
    EXPECT_EQ(complaint(config), "max_pending_handshakes must be in (0, 1048576] (ADR-0018)");
    config.max_pending_handshakes = wsrv::kMaxConfiguredConnections + 1;
    EXPECT_NE(complaint(config), "");
    // A small server is valid with the default: the effective bound is clamped, not refused.
    config.max_pending_handshakes = 256;
    config.max_connections = 10;
    EXPECT_EQ(complaint(config), "");
    EXPECT_EQ(wsrv::effective_max_pending_handshakes(config), 10U);
    config.max_connections = 1000;
    EXPECT_EQ(wsrv::effective_max_pending_handshakes(config), 256U);
}

TEST(ServerConfig, ForwardedHeaderNamesMustBeHeaderTokens) {
    ServerConfig config;
    config.handshake_forward_headers = {"Cookie", "X-Session"};
    EXPECT_EQ(complaint(config), "");
    config.handshake_forward_headers = {"Bad Name"};
    EXPECT_EQ(complaint(config), "handshake_forward_headers has a name that is not a header token");
    config.handshake_forward_headers = {""};
    EXPECT_NE(complaint(config), "");
    config.handshake_forward_headers.assign(65, "X-A");
    EXPECT_NE(complaint(config), "") << "the list itself is bounded";
}

// ADR-0020: the handshake policy and the session codecs belong together.
ServerConfig deflate_config() {
    ServerConfig config;
    config.handshake.permessage_deflate = wsrv::DeflatePolicy{};
    wsrv::SessionDeflate deflate;
    deflate.codecs = std::make_shared<wsrv::test::IdentityCodecFactory>();
    config.session.deflate = deflate;
    return config;
}

TEST(ServerConfig, PermessageDeflateIsValidWithBothHalvesOrNeither) {
    EXPECT_EQ(complaint(ServerConfig{}), "");
    EXPECT_EQ(complaint(deflate_config()), "");
}

TEST(ServerConfig, APolicyWithoutCodecsIsRefusedBecauseItWouldAdvertiseWhatItCannotServe) {
    ServerConfig config = deflate_config();
    config.session.deflate.reset();
    EXPECT_NE(complaint(config).find("permessage_deflate"), std::string_view::npos);
}

TEST(ServerConfig, CodecsWithoutAPolicyAreRefusedBecauseTheyLookLikeAWorkingFeature) {
    ServerConfig config = deflate_config();
    config.handshake.permessage_deflate.reset();
    EXPECT_NE(complaint(config).find("permessage_deflate"), std::string_view::npos);
}

TEST(ServerConfig, ADeflatePolicyWithAnImpossibleWindowNamesItsField) {
    for (const int bits : {8, 16}) {
        ServerConfig server_window = deflate_config();
        server_window.handshake.permessage_deflate->server_max_window_bits = bits;
        EXPECT_NE(complaint(server_window).find("permessage_deflate"), std::string_view::npos)
            << "server window " << bits;
        ServerConfig client_window = deflate_config();
        client_window.handshake.permessage_deflate->client_max_window_bits = bits;
        EXPECT_NE(complaint(client_window).find("permessage_deflate"), std::string_view::npos)
            << "client window " << bits;
    }
}

#if !WSRV_TEST_HAS_DEFLATE
// Fail closed (ADR-0020): a build without zlib must refuse the feature, never serve without it.
TEST(ServerBuilder, PermessageDeflateIsRefusedWhenTheBuildHasNoZlib) {
    wsrv::ServerBuilder builder;
    builder.listen("127.0.0.1", 0).permessage_deflate(wsrv::DeflateOptions{});
    try {
        static_cast<void>(builder.build());
        FAIL() << "build() accepted permessage_deflate without zlib";
    } catch (const wsrv::ConfigError& error) {
        EXPECT_NE(std::string_view{error.what()}.find("no zlib"), std::string_view::npos);
    }
}
#endif

TEST(ServerConfig, ARejectedValueNamesItsField) {
    ServerConfig config;
    config.max_connections = 0;
    EXPECT_NE(complaint(config).find("max_connections"), std::string_view::npos);

    config = ServerConfig{};
    config.max_connections = wsrv::kMaxConfiguredConnections + 1;
    EXPECT_NE(complaint(config).find("max_connections"), std::string_view::npos);

    config = ServerConfig{};
    config.handshake_timeout = 0ms;  // slowloris defence (T2) is not optional
    EXPECT_NE(complaint(config).find("handshake_timeout"), std::string_view::npos);

    config = ServerConfig{};
    config.write_stall_timeout = 0ms;  // slow reader defence (T3) is not optional
    EXPECT_NE(complaint(config).find("write_stall_timeout"), std::string_view::npos);

    config = ServerConfig{};
    config.shutdown_grace = -1ms;
    EXPECT_NE(complaint(config).find("shutdown_grace"), std::string_view::npos);

    config = ServerConfig{};
    config.http.max_total_bytes = 0;
    EXPECT_NE(complaint(config).find("http"), std::string_view::npos);

    config = ServerConfig{};
    config.read.max_reads_per_event = 0;
    EXPECT_NE(complaint(config).find("read"), std::string_view::npos);

    config = ServerConfig{};
    config.read.max_bytes_per_event = 0;
    EXPECT_NE(complaint(config).find("read"), std::string_view::npos);
}

TEST(ServerConfig, ADurationAboveTheCeilingIsRejected) {
    ServerConfig config;
    config.handshake_timeout = wsrv::kMaxConfiguredDuration + 1ms;
    EXPECT_NE(complaint(config).find("handshake_timeout"), std::string_view::npos);
}

TEST(ServerConfig,
     TheSessionConfigIsValidatedToo) {  // the hard ceilings of SECURITY_DESIGN section 3
    ServerConfig config;
    config.session.parser.max_frame_payload = wsrv::kMaxConfiguredFrame + 1;
    const wsrv::Result<void> result = validate(config);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidConfig);
}

TEST(ServerConfig, ThePerIpLimitsDefaultToTheRequirementsTableAndZeroMeansOff) {  // ADR-0021
    ServerConfig config;
    EXPECT_EQ(config.max_connections_per_ip, 100U);
    EXPECT_EQ(config.max_new_connections_per_ip_per_second, 50U);
    config.max_connections_per_ip = 0;
    config.max_new_connections_per_ip_per_second = 0;
    EXPECT_EQ(complaint(config), "") << "0 switches a limit off; it is not an error";
}

TEST(ServerConfig, ThePerIpLimitsCannotExceedTheirCeilings) {  // ADR-0021 section 4
    ServerConfig cap;
    cap.max_connections_per_ip = wsrv::kMaxConfiguredConnections + 1;
    EXPECT_NE(complaint(cap).find("max_connections_per_ip"), std::string_view::npos);
    ServerConfig rate;
    rate.max_new_connections_per_ip_per_second = wsrv::kMaxConfiguredNewConnectionsPerSecond + 1;
    EXPECT_NE(complaint(rate).find("max_new_connections_per_ip_per_second"),
              std::string_view::npos);
    rate.max_new_connections_per_ip_per_second = wsrv::kMaxConfiguredNewConnectionsPerSecond;
    EXPECT_EQ(complaint(rate), "");
}

TEST(ServerConfig, TheThreadCountMustBeBetweenOneAndTheCeiling) {  // ADR-0014
    wsrv::ServerConfig config;
    EXPECT_EQ(config.threads, 1U);
    config.threads = 0;
    ASSERT_FALSE(wsrv::validate(config).has_value());
    EXPECT_EQ(wsrv::validate(config).error().context, "threads out of range");
    config.threads = wsrv::kMaxConfiguredThreads + 1;
    EXPECT_FALSE(wsrv::validate(config).has_value());
    config.threads = wsrv::kMaxConfiguredThreads;
    EXPECT_TRUE(wsrv::validate(config).has_value());
}

TEST(ServerConfig, TheReadBudgetCannotExceedItsCeiling) {
    ServerConfig config;
    config.read.max_bytes_per_event = wsrv::kMaxConfiguredReadBytes + 1;
    EXPECT_NE(complaint(config).find("read"), std::string_view::npos);
}

/// A Session that records what a handler asked of it.
class StubSession final : public wsrv::Session {
public:
    [[nodiscard]] wsrv::ConnectionId id() const noexcept override {
        return 7;
    }
    [[nodiscard]] const wsrv::Endpoint& peer() const noexcept override {
        return peer_;
    }
    wsrv::Result<void> send_text(std::string_view /*text*/) override {
        ++sends;
        return {};
    }
    wsrv::Result<void> send_binary(wsrv::ByteView /*data*/) override {
        ++sends;
        return {};
    }
    [[nodiscard]] wsrv::Result<wsrv::MessageWriter> begin_text() override {
        return wsrv::fail(wsrv::ErrorCode::Closing, "stub");
    }
    [[nodiscard]] wsrv::Result<wsrv::MessageWriter> begin_binary() override {
        return wsrv::fail(wsrv::ErrorCode::Closing, "stub");
    }
    void pause_reads() override {}
    void resume_reads() override {}
    void close(wsrv::CloseCode /*code*/, std::string_view /*reason*/) override {
        ++closes;
    }
    [[nodiscard]] wsrv::SessionHandle handle() const override {
        return {};
    }
    int sends = 0;
    int closes = 0;

private:
    wsrv::Endpoint peer_;
};

TEST(Handler, EveryCallbackDefaultsToDoingNothing) {  // ISP: implement only what you use
    struct Silent final : wsrv::IWebSocketHandler {};
    Silent handler;
    StubSession session;
    handler.on_open(session);
    handler.on_text(session, "hello");
    handler.on_binary(session, wsrv::ByteView{});
    handler.on_message_part(session, wsrv::MessageType::Text, wsrv::ByteView{}, true);
    handler.on_error(session, wsrv::Error{ErrorCode::Io, "test"});
    handler.on_close(session, wsrv::CloseInfo{wsrv::CloseCode::Normal, {}, true});
    EXPECT_EQ(session.sends, 0);  // a silent handler never answers
    EXPECT_EQ(session.closes, 0);
}

}  // namespace

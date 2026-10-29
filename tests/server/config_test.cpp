// SPDX-License-Identifier: Apache-2.0
#include "server/config.hpp"

#include "server/handler.hpp"
#include "server/session_handle.hpp"

#include <gtest/gtest.h>

#include <chrono>
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
    EXPECT_EQ(config.session.close_timeout, 5s);
    EXPECT_EQ(config.session.max_control_frames_per_second, 100U);
    EXPECT_EQ(config.session.max_data_frames_per_second, 10000U);
    EXPECT_EQ(config.slow_consumer, wsrv::SlowConsumerPolicy::Close);  // EC-N09 default
    EXPECT_FALSE(config.session.assembler.streaming);  // buffered delivery by default
}

TEST(ServerConfig, TheDefaultsAreValid) {
    EXPECT_TRUE(validate(ServerConfig{}).has_value());
}

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

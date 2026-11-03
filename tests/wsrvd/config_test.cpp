// SPDX-License-Identifier: Apache-2.0
#include "wsrvd/config.hpp"

#include <gtest/gtest.h>

#include <string>

#include "ipc/message.hpp"

namespace {

using namespace std::chrono_literals;
using wsrv::ErrorCode;
using wsrv::wsrvd::DaemonConfig;

DaemonConfig with_socket() {
    DaemonConfig config;
    EXPECT_TRUE(config.set("ipc_socket", "/run/wsrv/ipc.sock").has_value());
    return config;
}

TEST(DaemonConfig, DefaultsAfterTheSocketIsGiven) {
    DaemonConfig config = with_socket();
    ASSERT_TRUE(config.finish().has_value());
    EXPECT_EQ(config.socket_path(), "/run/wsrv/ipc.sock");
    EXPECT_EQ(config.hello_timeout(), 5000ms);
    EXPECT_TRUE(config.token_file().empty());
    const std::size_t message = config.embedded().server().session.assembler.max_message_size;
    EXPECT_EQ(config.max_frame(), message + DaemonConfig::kDefaultFrameHeadroom);
}

TEST(DaemonConfig, TheSocketPathIsRequired) {
    DaemonConfig config;
    const auto done = config.finish();
    ASSERT_FALSE(done.has_value());
    EXPECT_EQ(done.error().code, ErrorCode::InvalidConfig);
}

TEST(DaemonConfig, OtherKeysGoToTheEmbeddedConfigWithItsRules) {
    DaemonConfig config = with_socket();
    ASSERT_TRUE(config.set("listen_port", "8080").has_value());
    ASSERT_TRUE(config.set("max_message_size", "65536").has_value());
    ASSERT_TRUE(config.finish().has_value());
    EXPECT_EQ(config.embedded().port(), 8080);
    EXPECT_EQ(config.max_frame(), 65536U + DaemonConfig::kDefaultFrameHeadroom);
    const auto bad = config.set("listen_port", "not-a-port");
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code, ErrorCode::InvalidConfig);
}

TEST(DaemonConfig, AnUnknownKeyIsRefused) {
    DaemonConfig config;
    const auto done = config.set("no_such_key", "1");
    ASSERT_FALSE(done.has_value());
    EXPECT_EQ(done.error().code, ErrorCode::InvalidConfig);
}

TEST(DaemonConfig, ThereIsNoWayToPutTheTokenOnTheCommandLineOrInAConfigValue) {
    DaemonConfig config;
    EXPECT_FALSE(config.set("ipc_token", "s3cret").has_value());
    EXPECT_FALSE(config.apply_argument("--ipc-token=s3cret").has_value());
    EXPECT_FALSE(config.apply_file_text("ipc_token = s3cret\n").has_value());
}

TEST(DaemonConfig, TheTokenFileIsRecordedNotRead) {
    DaemonConfig config = with_socket();
    ASSERT_TRUE(config.set("ipc_token_file", "/etc/wsrv/token").has_value());
    ASSERT_TRUE(config.finish().has_value());
    EXPECT_EQ(config.token_file(), "/etc/wsrv/token");
}

TEST(DaemonConfig, AnExplicitFrameLimitIsUsed) {
    DaemonConfig config = with_socket();
    ASSERT_TRUE(config.set("ipc_max_frame", "2097152").has_value());
    ASSERT_TRUE(config.finish().has_value());
    EXPECT_EQ(config.max_frame(), 2097152U);
}

TEST(DaemonConfig, AnExplicitFrameLimitMustHoldTheBiggestMessagePlusItsFrameFields) {
    // An EVENT frame is 6 header bytes + 12 body fields + the payload. A smaller limit would let
    // the WebSocket side accept a message the daemon can never forward, a path ADR-0017 does not
    // define; so the daemon refuses to start instead.
    constexpr std::size_t kOverhead = wsrv::ipc::kFrameHeaderSize + 12U;
    DaemonConfig small = with_socket();
    ASSERT_TRUE(small.set("max_message_size", "65536").has_value());
    ASSERT_TRUE(small.set("ipc_max_frame", std::to_string(65536U + kOverhead - 1U)).has_value());
    const auto refused = small.finish();
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code, ErrorCode::InvalidConfig);

    DaemonConfig exact = with_socket();
    ASSERT_TRUE(exact.set("max_message_size", "65536").has_value());
    ASSERT_TRUE(exact.set("ipc_max_frame", std::to_string(65536U + kOverhead)).has_value());
    EXPECT_TRUE(exact.finish().has_value());
}

TEST(DaemonConfig, TheStallTimeoutIsBounded) {
    for (const char* bad : {"0", "99", "600001", "-1", "abc", ""}) {
        DaemonConfig config = with_socket();
        EXPECT_FALSE(config.set("ipc_stall_ms", bad).has_value()) << bad;
    }
    DaemonConfig defaults = with_socket();
    ASSERT_TRUE(defaults.finish().has_value());
    EXPECT_EQ(defaults.stall_timeout(), 10000ms);

    DaemonConfig config = with_socket();
    ASSERT_TRUE(config.set("ipc_stall_ms", "100").has_value());
    ASSERT_TRUE(config.finish().has_value());
    EXPECT_EQ(config.stall_timeout(), 100ms);
}

TEST(DaemonConfig, TheFrameLimitHasASaneMinimumAndTheHardCeiling) {
    for (const char* bad :
         {"0", "100", "4095", "67108865", "99999999999999999999", "12x", "-5", ""}) {
        DaemonConfig config = with_socket();
        EXPECT_FALSE(config.set("ipc_max_frame", bad).has_value()) << bad;
    }
    DaemonConfig low = with_socket();
    EXPECT_TRUE(low.set("ipc_max_frame", "4096").has_value());
    DaemonConfig high = with_socket();
    EXPECT_TRUE(high.set("ipc_max_frame", "67108864").has_value());  // exactly 64 MiB
    EXPECT_EQ(wsrv::ipc::kMaxFrameCeiling, std::size_t{67108864});
}

TEST(DaemonConfig, ADefaultFrameLimitThatWouldPassTheCeilingIsRefusedAtFinish) {
    // A message this big could be received but never forwarded in one frame: say so at startup.
    DaemonConfig config = with_socket();
    ASSERT_TRUE(config.set("max_message_size", "67108864").has_value());
    const auto done = config.finish();
    ASSERT_FALSE(done.has_value());
    EXPECT_EQ(done.error().code, ErrorCode::InvalidConfig);
}

TEST(DaemonConfig, TheHelloTimeoutIsBounded) {
    for (const char* bad : {"0", "60001", "-1", "abc", ""}) {
        DaemonConfig config = with_socket();
        EXPECT_FALSE(config.set("ipc_hello_timeout_ms", bad).has_value()) << bad;
    }
    DaemonConfig config = with_socket();
    ASSERT_TRUE(config.set("ipc_hello_timeout_ms", "250").has_value());
    ASSERT_TRUE(config.finish().has_value());
    EXPECT_EQ(config.hello_timeout(), 250ms);
}

TEST(DaemonConfig, CommandLineFlagsAreKeyEqualsValue) {
    DaemonConfig config;
    ASSERT_TRUE(config.apply_argument("--ipc-socket=/run/x.sock").has_value());
    ASSERT_TRUE(config.apply_argument("--listen_port=9000").has_value());
    ASSERT_TRUE(config.finish().has_value());
    EXPECT_EQ(config.socket_path(), "/run/x.sock");
    EXPECT_EQ(config.embedded().port(), 9000);
    EXPECT_FALSE(config.apply_argument("ipc_socket=/x").has_value());  // missing dashes
    EXPECT_FALSE(config.apply_argument("--ipc-socket").has_value());   // missing value
    EXPECT_FALSE(config.apply_argument("--=x").has_value());           // missing key
}

TEST(DaemonConfig, ConfigFilesAreKeyValueLinesWithComments) {
    DaemonConfig config;
    const auto done = config.apply_file_text("# the daemon\n"
                                             "\n"
                                             "  ipc_socket = /run/wsrv/ipc.sock   \n"
                                             "listen_port=7000\n"
                                             "\t# indented comment\n"
                                             "allow_origin = https://example.com\n");
    ASSERT_TRUE(done.has_value()) << done.error().context;
    ASSERT_TRUE(config.finish().has_value());
    EXPECT_EQ(config.socket_path(), "/run/wsrv/ipc.sock");
    EXPECT_EQ(config.embedded().port(), 7000);
}

TEST(DaemonConfig, ABadLineInAConfigFileIsAnError) {
    DaemonConfig config;
    EXPECT_FALSE(config.apply_file_text("ipc_socket /run/x.sock\n").has_value());  // no '='
    EXPECT_FALSE(config.apply_file_text("= value\n").has_value());
    EXPECT_FALSE(config.apply_file_text("bogus_key = 1\n").has_value());
}

}  // namespace

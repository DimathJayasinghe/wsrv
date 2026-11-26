// SPDX-License-Identifier: Apache-2.0
#include "wsrvd/config.hpp"

#include "net/endpoint.hpp"

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

// ---- the TCP transport (ADR-0038) ---------------------------------------------------------------

DaemonConfig with_port(std::string_view port = "9300") {
    DaemonConfig config;
    EXPECT_TRUE(config.set("ipc_port", port).has_value());
    EXPECT_TRUE(config.set("ipc_token_file", "/run/secrets/ipc.token").has_value());
    return config;
}

TEST(DaemonConfigTcp, APortWithATokenFileSwitchesTheTcpTransportOn) {
    DaemonConfig config = with_port();
    ASSERT_TRUE(config.finish().has_value());
    EXPECT_TRUE(config.tcp());
    EXPECT_TRUE(config.socket_path().empty());
    EXPECT_EQ(config.ipc_endpoint().port, 9300);
    EXPECT_EQ(wsrv::format_endpoint(config.ipc_endpoint()),
              "127.0.0.1:9300");  // loopback by default
}

TEST(DaemonConfigTcp, PortZeroLetsTheKernelChoose) {  // like admin_port and publish_port
    DaemonConfig config = with_port("0");
    ASSERT_TRUE(config.finish().has_value());
    EXPECT_TRUE(config.tcp());
    EXPECT_EQ(config.ipc_endpoint().port, 0);
}

TEST(DaemonConfigTcp, TheHostIsAnAddressLiteralIpv4OrIpv6) {
    DaemonConfig v4 = with_port();
    ASSERT_TRUE(v4.set("ipc_host", "0.0.0.0").has_value());
    ASSERT_TRUE(v4.finish().has_value());
    EXPECT_EQ(wsrv::format_endpoint(v4.ipc_endpoint()), "0.0.0.0:9300");
    DaemonConfig v6 = with_port();
    ASSERT_TRUE(v6.set("ipc_host", "::1").has_value());
    ASSERT_TRUE(v6.finish().has_value());
    EXPECT_TRUE(v6.ipc_endpoint().v6);
}

TEST(DaemonConfigTcp, AHostnameIsRefusedNoLookupOnTheLoopThread) {
    DaemonConfig config = with_port();
    ASSERT_TRUE(config.set("ipc_host", "localhost").has_value());  // checked once, in finish()
    const auto done = config.finish();
    ASSERT_FALSE(done.has_value());
    EXPECT_EQ(done.error().code, ErrorCode::InvalidConfig);
}

TEST(DaemonConfigTcp, ThePortMustBeANumberFrom0To65535) {
    for (const char* bad :
         {"", "-1", "65536", "93x0", " 9300", "9300 ", "0x10", "99999999999999999999"}) {
        DaemonConfig config;
        const auto done = config.set("ipc_port", bad);
        ASSERT_FALSE(done.has_value()) << bad;
        EXPECT_EQ(done.error().code, ErrorCode::InvalidConfig);
    }
    DaemonConfig edge;
    EXPECT_TRUE(edge.set("ipc_port", "65535").has_value());
}

TEST(DaemonConfigTcp, BothTransportsAtOnceIsAnErrorThatNamesBothKeys) {
    DaemonConfig config = with_port();
    ASSERT_TRUE(config.set("ipc_socket", "/run/wsrv/ipc.sock").has_value());
    const auto done = config.finish();
    ASSERT_FALSE(done.has_value());
    EXPECT_EQ(done.error().code, ErrorCode::InvalidConfig);
    const std::string message{done.error().context};
    EXPECT_NE(message.find("ipc_socket"), std::string::npos);
    EXPECT_NE(message.find("ipc_port"), std::string::npos);
}

TEST(DaemonConfigTcp,
     WithoutATokenFileThereIsNoTcpTransport) {  // the token is the only authentication
    DaemonConfig config;
    ASSERT_TRUE(config.set("ipc_port", "9300").has_value());
    const auto done = config.finish();
    ASSERT_FALSE(done.has_value());
    EXPECT_EQ(done.error().code, ErrorCode::InvalidConfig);
    EXPECT_NE(std::string{done.error().context}.find("ipc_token_file"), std::string::npos);
}

TEST(DaemonConfigTcp, AHostWithoutAPortIsMostLikelyAMistypedKey) {
    DaemonConfig config = with_socket();
    ASSERT_TRUE(config.set("ipc_host", "0.0.0.0").has_value());
    const auto done = config.finish();
    ASSERT_FALSE(done.has_value());
    EXPECT_NE(std::string{done.error().context}.find("ipc_port"), std::string::npos);
}

TEST(DaemonConfigTcp, NeitherTransportNamesBothKeysInTheError) {
    DaemonConfig config;
    const auto done = config.finish();
    ASSERT_FALSE(done.has_value());
    const std::string message{done.error().context};
    EXPECT_NE(message.find("ipc_socket"), std::string::npos);
    EXPECT_NE(message.find("ipc_port"), std::string::npos);
}

TEST(DaemonConfigTcp, ABuiltInDefaultSocketAppliesOnlyWhenNeitherKeyIsSet) {  // the official image
    DaemonConfig neither;
    neither.set_default_socket("/run/wsrv/ipc.sock");
    ASSERT_TRUE(neither.finish().has_value());
    EXPECT_FALSE(neither.tcp());
    EXPECT_EQ(neither.socket_path(), "/run/wsrv/ipc.sock");

    DaemonConfig port_only = with_port();  // `docker run -e WSRVD_IPC_PORT=9300 ...`
    port_only.set_default_socket("/run/wsrv/ipc.sock");
    ASSERT_TRUE(port_only.finish().has_value());
    EXPECT_TRUE(port_only.tcp());
    EXPECT_TRUE(port_only.socket_path().empty());

    DaemonConfig socket_only = with_socket();
    socket_only.set_default_socket("/run/wsrv/other.sock");
    ASSERT_TRUE(socket_only.finish().has_value());
    EXPECT_EQ(socket_only.socket_path(), "/run/wsrv/ipc.sock");  // the explicit key wins

    DaemonConfig both = with_port();
    ASSERT_TRUE(both.set("ipc_socket", "/run/wsrv/x.sock").has_value());
    both.set_default_socket("/run/wsrv/ipc.sock");
    EXPECT_FALSE(
        both.finish().has_value());  // explicit both stays an error, the default is no excuse
}

TEST(DaemonConfigTcp, TheKeysComeInAsFlagsAndFileLinesLikeAnyOther) {
    DaemonConfig config;
    ASSERT_TRUE(config.apply_argument("--ipc-port=9300").has_value());
    ASSERT_TRUE(config.apply_file_text("ipc_host = 10.0.0.5\nipc_token_file = /t\n").has_value());
    ASSERT_TRUE(config.finish().has_value());
    EXPECT_EQ(wsrv::format_endpoint(config.ipc_endpoint()), "10.0.0.5:9300");
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

TEST(DaemonConfig,
     TheTlsSettingsComeInAsFlagsLikeAnyOtherKey) {  // ADR-0019: same parser as the library
    DaemonConfig config = with_socket();
    ASSERT_TRUE(config.apply_argument("--tls-cert=/etc/wsrv/cert.pem").has_value());
    ASSERT_TRUE(config.apply_argument("--tls-key=/etc/wsrv/key.pem").has_value());
    // The value of tls_sni itself contains '=' and ':': only the first '=' belongs to the flag.
    ASSERT_TRUE(
        config.apply_argument("--tls-sni=app.example.com=/c/app.pem:/k/app.pem").has_value());
    ASSERT_TRUE(config.finish().has_value());
    EXPECT_EQ(config.embedded().tls_cert(), "/etc/wsrv/cert.pem");
    EXPECT_EQ(config.embedded().tls_key(), "/etc/wsrv/key.pem");
    ASSERT_EQ(config.embedded().tls_sni().size(), 1U);
    EXPECT_EQ(config.embedded().tls_sni()[0].name, "app.example.com");
    EXPECT_EQ(config.embedded().tls_sni()[0].key, "/k/app.pem");
}

TEST(DaemonConfig, ThePerIpLimitsComeInAsFlagsLikeAnyOtherKey) {  // ADR-0021 section 5
    DaemonConfig config = with_socket();
    ASSERT_TRUE(config.apply_argument("--max-connections-per-ip=20").has_value());
    ASSERT_TRUE(config.apply_argument("--max-new-connections-per-ip-per-second=0").has_value());
    ASSERT_TRUE(config.finish().has_value());
    EXPECT_EQ(config.embedded().server().max_connections_per_ip, 20U);
    EXPECT_EQ(config.embedded().server().max_new_connections_per_ip_per_second, 0U);
}

TEST(DaemonConfig, TheAdminListenerComesInAsFlags) {  // ADR-0023 section 3
    DaemonConfig config = with_socket();
    ASSERT_TRUE(config.apply_argument("--admin-host=127.0.0.1").has_value());
    ASSERT_TRUE(config.apply_argument("--admin-port=9100").has_value());
    ASSERT_TRUE(config.finish().has_value());
    EXPECT_TRUE(config.embedded().admin_requested());
    EXPECT_EQ(config.embedded().admin_port(), 9100);
}

TEST(DaemonConfig, PermessageDeflateComesInAsFlagsLikeAnyOtherKey) {  // ADR-0020 section 6
    DaemonConfig config = with_socket();
    ASSERT_TRUE(config.apply_argument("--permessage-deflate=true").has_value());
    ASSERT_TRUE(config.apply_argument("--deflate-level=4").has_value());
    ASSERT_TRUE(config.apply_argument("--deflate-server-max-window-bits=11").has_value());
    ASSERT_TRUE(config.apply_argument("--deflate-max-ratio=50").has_value());
    ASSERT_TRUE(config.finish().has_value());
    EXPECT_TRUE(config.embedded().permessage_deflate());
    EXPECT_EQ(config.embedded().deflate_options().level, 4);
    EXPECT_EQ(config.embedded().deflate_options().policy.server_max_window_bits, 11);
    EXPECT_EQ(config.embedded().deflate_options().max_inflate_ratio, 50U);
}

TEST(DaemonConfig, DeflateIsOffUnlessAskedFor) {
    DaemonConfig config = with_socket();
    ASSERT_TRUE(config.apply_argument("--deflate-level=4").has_value());  // tuning alone is inert
    ASSERT_TRUE(config.finish().has_value());
    EXPECT_FALSE(config.embedded().permessage_deflate());
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

// ADR-0041: the admin token file is a path recorded here and read later, never a value, and it
// belongs to an admin listener: a file without a port is most likely a mistyped key.
TEST(DaemonConfig, TheAdminTokenIsAFileNeverAValue) {
    DaemonConfig config;
    EXPECT_FALSE(config.set("admin_token", "s3cret").has_value());
    EXPECT_FALSE(config.apply_argument("--admin-token=s3cret").has_value());
    EXPECT_FALSE(config.apply_file_text("admin_token = s3cret\n").has_value());
    EXPECT_FALSE(config.set("admin_token_file", "").has_value()) << "an empty path is no path";
}

TEST(DaemonConfig, TheAdminTokenFileIsRecordedNotReadAndNeedsTheAdminPort) {
    DaemonConfig without_port = with_socket();
    ASSERT_TRUE(without_port.set("admin_token_file", "/etc/wsrv/admin.token").has_value());
    const wsrv::Result<void> refused = without_port.finish();
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code, ErrorCode::InvalidConfig);
    EXPECT_NE(std::string{refused.error().context}.find("admin_port"), std::string::npos);

    DaemonConfig with_port = with_socket();
    ASSERT_TRUE(with_port.apply_argument("--admin-port=9100").has_value());
    ASSERT_TRUE(with_port.apply_argument("--admin-token-file=/etc/wsrv/admin.token").has_value());
    ASSERT_TRUE(with_port.finish().has_value());
    EXPECT_EQ(with_port.admin_token_file(), "/etc/wsrv/admin.token");
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

// SPDX-License-Identifier: Apache-2.0
#include "wsrvd/startup.hpp"

#include "net/endpoint.hpp"
#include "server/config.hpp"
#include "support/fake_syscalls.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <sys/stat.h>

namespace {

namespace fs = std::filesystem;
using wsrv::ErrorCode;
using wsrv::test::FakeSyscalls;
using wsrv::wsrvd::prepare;
using wsrv::wsrvd::usage;

/// A token in the form `wsrvd --generate-token` writes (computed independently, see
/// token_format_test).
constexpr std::string_view kGenerated =
    "wsrv1_000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f_27994e12";

struct StartupTest : public ::testing::Test {
    FakeSyscalls sys;
    std::string dir;
    void SetUp() override {
        std::string pattern = (fs::temp_directory_path() / "wsrv-start-XXXXXX").string();
        ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
        dir = pattern;
    }
    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(dir, ignored);
    }
    [[nodiscard]] std::string
    write(const std::string& name, const std::string& content, mode_t mode = 0600) const {
        const std::string path = dir + "/" + name;
        {
            std::ofstream file{path, std::ios::binary};
            file << content;
        }
        EXPECT_EQ(::chmod(path.c_str(), mode), 0);
        return path;
    }
    [[nodiscard]] wsrv::Result<wsrv::wsrvd::Startup> run(std::vector<std::string> args) {
        const std::vector<std::string_view> views{args.begin(), args.end()};
        return prepare(views, sys);
    }
};

TEST_F(StartupTest, FlagsAloneAreEnough) {
    const auto startup = run({"--ipc-socket=/run/wsrv/ipc.sock", "--listen_port=9000"});
    ASSERT_TRUE(startup.has_value());
    EXPECT_EQ(startup->config.socket_path(), "/run/wsrv/ipc.sock");
    EXPECT_EQ(startup->config.embedded().port(), 9000);
    EXPECT_TRUE(startup->token.empty());
    EXPECT_FALSE(startup->help);
}

TEST_F(StartupTest, HelpNeedsNoSocket) {
    const auto startup = run({"--help"});
    ASSERT_TRUE(startup.has_value());
    EXPECT_TRUE(startup->help);
}

TEST_F(StartupTest, ConfigFilesAreReadInOrderAndLaterInputWins) {
    const std::string file = write("wsrvd.conf", "ipc_socket = /run/a.sock\nlisten_port = 7000\n");
    const auto startup = run({"--config=" + file, "--listen-port=7001"});
    ASSERT_TRUE(startup.has_value());
    EXPECT_EQ(startup->config.socket_path(), "/run/a.sock");
    EXPECT_EQ(startup->config.embedded().port(), 7001);  // the flag came after the file
}

TEST_F(StartupTest, AMissingOrOversizedConfigFileIsAnError) {
    const auto missing = run({"--config=" + dir + "/nope.conf"});
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code, ErrorCode::InvalidConfig);
    const std::string huge(std::size_t{1} << 20U, '#');
    const auto big = run({"--config=" + write("big.conf", huge)});
    ASSERT_FALSE(big.has_value());
    EXPECT_EQ(big.error().code, ErrorCode::InvalidConfig);
}

TEST_F(StartupTest, TheTokenIsReadFromItsFile) {
    const std::string token = write("token", "s3cret\n");
    const auto startup = run({"--ipc-socket=/run/x.sock", "--ipc-token-file=" + token});
    ASSERT_TRUE(startup.has_value());
    EXPECT_EQ(startup->token, "s3cret");
}

TEST_F(StartupTest, AnUnsafeTokenFileStopsTheStartup) {
    const std::string token = write("token", "s3cret\n", 0644);
    const auto startup = run({"--ipc-socket=/run/x.sock", "--ipc-token-file=" + token});
    ASSERT_FALSE(startup.has_value());
    EXPECT_EQ(startup.error().code, ErrorCode::InvalidConfig);
}

// ADR-0038 section 3: the TCP transport. The token is the only authentication, so it must be one
// the generator made: a length floor lets "MyCompanyWebsocketToken2026!!!!!" through.
TEST_F(StartupTest, TheTcpTransportNeedsATokenTheGeneratorMade) {
    const std::string generated = std::string{kGenerated};
    const std::string good = write("good", generated + "\n");
    const auto startup = run({"--ipc-port=9300", "--ipc-token-file=" + good});
    ASSERT_TRUE(startup.has_value());
    EXPECT_TRUE(startup->config.tcp());
    EXPECT_EQ(startup->token, generated);
}

TEST_F(StartupTest, ALongButChosenTokenIsRefusedOverTcpAndTheMessageSaysWhatToRun) {
    for (const std::string& weak : {std::string(31, 't'),
                                    std::string(32, 't'),
                                    std::string{"MyCompanyWebsocketToken2026!!!!!"},
                                    std::string(200, 'x')}) {
        const std::string file = write("weak", weak + "\n");
        const auto result = run({"--ipc-port=9300", "--ipc-token-file=" + file});
        ASSERT_FALSE(result.has_value()) << weak;
        EXPECT_EQ(result.error().code, ErrorCode::InvalidConfig);
        const std::string message{result.error().context};
        EXPECT_NE(message.find("--generate-token"), std::string::npos) << message;
        EXPECT_EQ(message.find(weak), std::string::npos) << "the token must never be echoed";
    }
}

TEST_F(StartupTest, ATokenWithOneDigitChangedIsRefusedOverTcp) {
    std::string typo = std::string{kGenerated};
    typo[20] = typo[20] == '0' ? '1' : '0';
    const auto result = run({"--ipc-port=9300", "--ipc-token-file=" + write("typo", typo + "\n")});
    EXPECT_FALSE(result.has_value());
}

TEST_F(StartupTest, GenerateTokenIsAModeThatNeedsNoConfigAndNoSecrets) {
    const auto startup = run({"--generate-token=/data/ipc.token"});
    ASSERT_TRUE(startup.has_value());
    EXPECT_EQ(startup->generate_token, "/data/ipc.token");
    EXPECT_FALSE(startup->healthcheck);
}

TEST_F(StartupTest, GenerateTokenTakesNoOtherArguments) {
    // Anything else on the line is a mistake worth stopping for, not something to ignore quietly.
    EXPECT_FALSE(run({"--generate-token=/data/t", "--ipc-port=9300"}).has_value());
    EXPECT_FALSE(run({"--ipc-port=9300", "--generate-token=/data/t"}).has_value());
}

TEST_F(StartupTest, GenerateTokenNeedsAPath) {
    EXPECT_FALSE(run({"--generate-token="}).has_value());
    EXPECT_FALSE(run({"--generate-token"}).has_value());
}

TEST_F(StartupTest, TheUsageTextNamesTheGeneratorAndTheTokenForm) {
    const std::string text{usage()};
    EXPECT_NE(text.find("--generate-token=FILE"), std::string::npos);
    EXPECT_NE(text.find("wsrv1_"), std::string::npos);
}

TEST_F(StartupTest, ASocketTokenKeepsItsOldRulesWhateverItsLength) {  // 1.0 behaviour is unchanged
    const std::string token = write("token", "s3cret\n");
    const auto startup = run({"--ipc-socket=/run/x.sock", "--ipc-token-file=" + token});
    ASSERT_TRUE(startup.has_value());
    EXPECT_FALSE(startup->config.tcp());
}

TEST_F(StartupTest, ANamedBinaryHasNoDefaultSocketUnlessItWasBuiltWithOne) {
    // Tests are built without WSRVD_DEFAULT_IPC_SOCKET, like the release binaries: neither key is
    // an error, not a guessed path.
    const auto startup = run({"--listen-port=9000"});
    ASSERT_FALSE(startup.has_value());
    EXPECT_EQ(startup.error().code, ErrorCode::InvalidConfig);
}

// ADR-0028: the publish endpoint. Its token is a file like the IPC token, never a key or a flag.
TEST_F(StartupTest, ThePublishTokenIsReadFromItsFileAndGivenToTheEmbeddedConfig) {
    const std::string token = write("publish", std::string{kGenerated} + "\n");
    const auto startup = run({"--ipc-socket=/run/x.sock",
                              "--broker=true",
                              "--publish-port=9200",
                              "--publish-token-file=" + token});
    ASSERT_TRUE(startup.has_value());
    EXPECT_TRUE(startup->config.embedded().publish_requested());
    EXPECT_EQ(startup->config.embedded().publish_token(), std::string{kGenerated});
    EXPECT_TRUE(startup->config.embedded().check_publish().has_value());
}

TEST_F(StartupTest, ThePublishTokenHasNoKeyAndNoFlag) {
    EXPECT_FALSE(
        run({"--ipc-socket=/run/x.sock", "--publish-token=" + std::string(40, 'p')}).has_value());
}

TEST_F(StartupTest, APublishPortAndItsTokenFileComeAsAPair) {
    const std::string token = write("publish", std::string{kGenerated});
    EXPECT_FALSE(
        run({"--ipc-socket=/run/x.sock", "--broker=true", "--publish-port=9200"}).has_value());
    EXPECT_FALSE(run({"--ipc-socket=/run/x.sock", "--publish-token-file=" + token}).has_value());
}

TEST_F(StartupTest, AShortOrUnsafePublishTokenStopsTheStartup) {
    const std::string shortened = write("short", std::string(31, 'p'));
    const std::string open = write("open", std::string{kGenerated}, 0644);
    for (const std::string& file : {shortened, open}) {
        const auto startup = run({"--ipc-socket=/run/x.sock",
                                  "--broker=true",
                                  "--publish-port=9200",
                                  "--publish-token-file=" + file});
        ASSERT_FALSE(startup.has_value()) << file;
        EXPECT_EQ(startup.error().code, ErrorCode::InvalidConfig);
    }
}

// ADR-0039: the publish endpoint is plain HTTP and reachable from the network, and a length floor
// cannot tell a random token from a long guessable one, so only the generated form starts it. The
// rule holds on loopback too, and the message says it is new in 1.1 (a 1.0 configuration that
// worked must be told why it stops) and names the fix without printing the token.
TEST_F(StartupTest, ALongButChosenPublishTokenStopsTheStartupAndTheMessageNamesTheFix) {
    const std::string chosen = "MyCompanyWebsocketToken2026!!!!!MyCompany";  // 40 bytes, guessable
    const std::string hex64 = write("hex64", std::string(64, 'a'));          // what 1.0 docs made
    for (const std::string& file : {write("chosen", chosen), hex64}) {
        for (const char* host : {"--publish-host=127.0.0.1", "--publish-host=0.0.0.0"}) {
            const auto startup = run({"--ipc-socket=/run/x.sock",
                                      "--broker=true",
                                      "--publish-port=9200",
                                      host,
                                      "--publish-token-file=" + file});
            ASSERT_FALSE(startup.has_value()) << file << " " << host;
            EXPECT_EQ(startup.error().code, ErrorCode::InvalidConfig);
            const std::string message{startup.error().context};
            EXPECT_NE(message.find("--generate-token"), std::string::npos) << message;
            EXPECT_NE(message.find("1.1"), std::string::npos) << message;
            EXPECT_EQ(message.find(chosen), std::string::npos) << "the token must not be echoed";
        }
    }
}

TEST_F(StartupTest, ATypoInAGeneratedPublishTokenStopsTheStartup) {
    std::string typo = std::string{kGenerated};
    typo[10] = typo[10] == 'a' ? 'b' : 'a';
    const auto startup = run({"--ipc-socket=/run/x.sock",
                              "--broker=true",
                              "--publish-port=9200",
                              "--publish-token-file=" + write("typo", typo)});
    ASSERT_FALSE(startup.has_value());
    EXPECT_EQ(startup.error().code, ErrorCode::InvalidConfig);
}

// The Unix-socket IPC token keeps its old rules: the publish rule must not leak into it.
TEST_F(StartupTest, TheUnixSocketIpcTokenStillAcceptsAnyToken) {
    const auto startup = run({"--ipc-socket=/run/x.sock",
                              "--ipc-token-file=" + write("ipc", "s3cret-chosen-by-a-person\n")});
    ASSERT_TRUE(startup.has_value());
    EXPECT_EQ(startup->token, "s3cret-chosen-by-a-person");
}

TEST_F(StartupTest, BadInputIsRefusedBeforeAnythingStarts) {
    EXPECT_FALSE(run({}).has_value());                                          // no socket
    EXPECT_FALSE(run({"--ipc-socket=/run/x.sock", "--bogus=1"}).has_value());   // unknown key
    EXPECT_FALSE(run({"--ipc-socket=/run/x.sock", "stray-word"}).has_value());  // not a flag
    EXPECT_FALSE(run({"--ipc-socket=/run/x.sock", "--ipc-token=s3cret"}).has_value());
}

// ADR-0030: environment variables are the base layer; the arguments override them.
struct EnvironmentTest : public StartupTest {
    std::string detail;
    [[nodiscard]] wsrv::Result<wsrv::wsrvd::Startup> run_with(std::vector<std::string> args,
                                                              std::vector<std::string> env) {
        const std::vector<std::string_view> arg_views{args.begin(), args.end()};
        const std::vector<std::string_view> env_views{env.begin(), env.end()};
        detail.clear();
        return prepare(arg_views, env_views, sys, &detail);
    }
};

TEST_F(EnvironmentTest, AnEnvironmentAloneIsEnoughAndNamesAreTheUpperCasedKeys) {
    const auto startup = run_with({},
                                  {"WSRVD_IPC_SOCKET=/run/wsrv/ipc.sock",
                                   "WSRVD_LISTEN_PORT=9100",
                                   "WSRVD_MAX_CONNECTIONS_PER_IP=7"});
    ASSERT_TRUE(startup.has_value());
    EXPECT_EQ(startup->config.socket_path(), "/run/wsrv/ipc.sock");
    EXPECT_EQ(startup->config.embedded().port(), 9100);
    EXPECT_EQ(startup->config.embedded().server().max_connections_per_ip, 7U);
}

// ADR-0040: the budget is one more key, so it is a flag and a variable with no extra code; this
// pins that a container operator can set it (the Kubernetes manifest does) and that a bad value is
// refused at start-up rather than silently ignored.
TEST_F(EnvironmentTest, TheBufferedBytesBudgetIsAVariableAndAFlagAndABadOneIsRefused) {
    const auto defaults = run_with({}, {"WSRVD_IPC_SOCKET=/run/a.sock"});
    ASSERT_TRUE(defaults.has_value());
    EXPECT_EQ(defaults->config.embedded().server().max_buffered_bytes,
              wsrv::kDefaultMaxBufferedBytes);

    const auto from_env =
        run_with({}, {"WSRVD_IPC_SOCKET=/run/a.sock", "WSRVD_MAX_BUFFERED_BYTES=268435456"});
    ASSERT_TRUE(from_env.has_value());
    EXPECT_EQ(from_env->config.embedded().server().max_buffered_bytes, 268435456U);

    const auto flag =
        run_with({"--max-buffered-bytes=1048576"},
                 {"WSRVD_IPC_SOCKET=/run/a.sock", "WSRVD_MAX_BUFFERED_BYTES=268435456"});
    ASSERT_TRUE(flag.has_value());
    EXPECT_EQ(flag->config.embedded().server().max_buffered_bytes, 1048576U)
        << "a flag wins over the environment";

    for (const char* bad : {"WSRVD_MAX_BUFFERED_BYTES=0",
                            "WSRVD_MAX_BUFFERED_BYTES=-1",
                            "WSRVD_MAX_BUFFERED_BYTES=lots",
                            "WSRVD_MAX_BUFFERED_BYTES=1099511627777"}) {
        EXPECT_FALSE(run_with({}, {"WSRVD_IPC_SOCKET=/run/a.sock", bad}).has_value())
            << bad << " (0, a sign, text and one byte over 1 TiB are all refused)";
    }
    const auto top =
        run_with({}, {"WSRVD_IPC_SOCKET=/run/a.sock", "WSRVD_MAX_BUFFERED_BYTES=1099511627776"});
    ASSERT_TRUE(top.has_value()) << "exactly 1 TiB is the largest value";
    EXPECT_EQ(top->config.embedded().server().max_buffered_bytes, std::size_t{1} << 40);
}

TEST_F(EnvironmentTest, TheLogSettingsAreFlagsAndVariablesLikeAnyOther) {  // ADR-0037
    const auto from_env = run_with(
        {}, {"WSRVD_IPC_SOCKET=/run/a.sock", "WSRVD_LOG_LEVEL=debug", "WSRVD_LOG_PEER=true"});
    ASSERT_TRUE(from_env.has_value());
    EXPECT_EQ(from_env->config.embedded().log_level(), wsrv::LogLevel::Debug);
    EXPECT_TRUE(from_env->config.embedded().server().log_peer);

    const auto flag =
        run_with({"--log-level=error"}, {"WSRVD_IPC_SOCKET=/run/a.sock", "WSRVD_LOG_LEVEL=debug"});
    ASSERT_TRUE(flag.has_value());
    EXPECT_EQ(flag->config.embedded().log_level(), wsrv::LogLevel::Error)
        << "a flag wins over the environment";

    const auto misspelled =
        run_with({}, {"WSRVD_IPC_SOCKET=/run/a.sock", "WSRVD_LOG_LEVEL=verbose"});
    EXPECT_FALSE(misspelled.has_value()) << "a level that does not exist is not silently info";
}

TEST_F(EnvironmentTest, ArgumentsAndFilesOverrideTheEnvironmentAndFlagsStillWinOverFiles) {
    const std::string file = write("wsrvd.conf", "listen_port = 7000\n");
    const std::vector<std::string> env{"WSRVD_IPC_SOCKET=/run/a.sock", "WSRVD_LISTEN_PORT=9100"};
    const auto from_env = run_with({}, env);
    ASSERT_TRUE(from_env.has_value());
    EXPECT_EQ(from_env->config.embedded().port(), 9100);
    const auto from_file = run_with({"--config=" + file}, env);
    ASSERT_TRUE(from_file.has_value());
    EXPECT_EQ(from_file->config.embedded().port(), 7000);
    const auto from_flag = run_with({"--config=" + file, "--listen-port=7001"}, env);
    ASSERT_TRUE(from_flag.has_value());
    EXPECT_EQ(from_flag->config.embedded().port(), 7001);
}

TEST_F(EnvironmentTest, RepeatableKeysTakeACommaListAndAddToWhatFlagsAdd) {
    const auto startup = run_with(
        {"--allow-origin=https://flag.example"},
        {"WSRVD_IPC_SOCKET=/run/a.sock", "WSRVD_ALLOW_ORIGIN=https://a.example,https://b.example"});
    ASSERT_TRUE(startup.has_value());
    const auto& origins = startup->config.embedded().server().handshake.allowed_origins;
    ASSERT_EQ(origins.size(), 3U);
    EXPECT_EQ(origins[0], "https://a.example");
    EXPECT_EQ(origins[1], "https://b.example");
    EXPECT_EQ(origins[2], "https://flag.example");
}

TEST_F(EnvironmentTest, AnEmptyVariableCountsAsUnsetAndOtherPrefixesAreNeverRead) {
    const auto startup = run_with({},
                                  {"WSRVD_IPC_SOCKET=/run/a.sock",
                                   "WSRVD_LISTEN_PORT=",
                                   "WSRV_PORT=1",
                                   "WSRV_VERSION=0.1.0",
                                   "PATH=/usr/bin",
                                   "WSRVDX=1"});
    ASSERT_TRUE(startup.has_value());
    EXPECT_EQ(startup->config.embedded().port(), 0) << "the default, not an error";
}

TEST_F(EnvironmentTest, AnUnknownWsrvdVariableStopsTheStartupAndIsNamed) {
    const auto startup =
        run_with({"--ipc-socket=/run/a.sock"}, {"WSRVD_ALLOW_ORIGINS=https://a.example"});
    ASSERT_FALSE(startup.has_value());
    EXPECT_EQ(startup.error().code, ErrorCode::InvalidConfig);
    EXPECT_EQ(detail, "WSRVD_ALLOW_ORIGINS")
        << "a typo must not start a server without an Origin list";
}

// ADR-0030 A1: for every Service in its namespace Kubernetes injects <SERVICE>_SERVICE_HOST,
// <SERVICE>_SERVICE_PORT[_<NAME>], <SERVICE>_PORT and
// <SERVICE>_PORT_<n>_<PROTO>[_PROTO|_PORT|_ADDR]. A Service named "wsrvd" (the natural name) makes
// WSRVD_* names that are not settings; they must not stop the daemon or its --healthcheck, which
// reads the same environment.
TEST_F(EnvironmentTest, TheVariablesKubernetesInjectsForAServiceNamedWsrvdAreNotSettings) {
    const auto startup = run_with({"--ipc-socket=/run/a.sock"},
                                  {"WSRVD_SERVICE_HOST=10.96.0.12",
                                   "WSRVD_SERVICE_PORT=8080",
                                   "WSRVD_SERVICE_PORT_WS=8080",
                                   "WSRVD_SERVICE_PORT_ADMIN_METRICS=9101",
                                   "WSRVD_PORT=tcp://10.96.0.12:8080",
                                   "WSRVD_PORT_8080_TCP=tcp://10.96.0.12:8080",
                                   "WSRVD_PORT_8080_TCP_PROTO=tcp",
                                   "WSRVD_PORT_8080_TCP_PORT=8080",
                                   "WSRVD_PORT_8080_TCP_ADDR=10.96.0.12",
                                   "WSRVD_LISTEN_PORT=9100"});
    ASSERT_TRUE(startup.has_value()) << detail;
    EXPECT_EQ(startup->config.embedded().port(), 9100)
        << "a real setting beside them still applies";
}

TEST_F(EnvironmentTest, OnlyTheExactShapesOfServiceLinksAreIgnoredATypoStillStops) {
    for (const char* entry : {"WSRVD_PORT=8080",  // a person meaning the listen port
                              "WSRVD_PORT=",      // empty: skipped like any empty value
                              "WSRVD_SERVICE_HOSTS=10.0.0.1",  // not the injected name
                              "WSRVD_SERVICE=x",
                              "WSRVD_PORT_8080=x",                 // no protocol
                              "WSRVD_PORT_X_TCP=tcp://1.2.3.4:5",  // the port is a number
                              "WSRVD_PORT_8080_TCP_EXTRA=x"}) {
        const auto startup = run_with({"--ipc-socket=/run/a.sock"}, {entry});
        if (std::string_view{entry}.ends_with('=')) {
            EXPECT_TRUE(startup.has_value()) << entry;
        } else {
            EXPECT_FALSE(startup.has_value()) << entry << " must still be an error";
        }
    }
}

TEST_F(EnvironmentTest, ABadValueStopsTheStartupAndNamesTheVariable) {
    const auto startup = run_with({"--ipc-socket=/run/a.sock"}, {"WSRVD_LISTEN_PORT=99999"});
    ASSERT_FALSE(startup.has_value());
    EXPECT_EQ(detail, "WSRVD_LISTEN_PORT");
}

TEST_F(EnvironmentTest, SecretsNeverComeFromTheEnvironment) {
    for (const char* name : {"WSRVD_IPC_TOKEN=s3cret",
                             "WSRVD_PUBLISH_TOKEN=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}) {
        const auto startup = run_with({"--ipc-socket=/run/a.sock"}, {name});
        EXPECT_FALSE(startup.has_value()) << name;
    }
}

TEST_F(EnvironmentTest, AMalformedEntryWithoutAnEqualsSignIsIgnoredLikeTheShellDoes) {
    const auto startup = run_with({"--ipc-socket=/run/a.sock"}, {"WSRVD_NOEQUALS", "=x"});
    EXPECT_TRUE(startup.has_value());
}

TEST_F(EnvironmentTest, HealthcheckIsAModeAndNeedsTheSameConfiguration) {
    const auto startup = run_with({"--healthcheck"}, {"WSRVD_IPC_SOCKET=/run/a.sock"});
    ASSERT_TRUE(startup.has_value());
    EXPECT_TRUE(startup->healthcheck);
    EXPECT_FALSE(startup->help);
    const auto normal = run_with({}, {"WSRVD_IPC_SOCKET=/run/a.sock"});
    ASSERT_TRUE(normal.has_value());
    EXPECT_FALSE(normal->healthcheck);
    EXPECT_FALSE(run_with({"--healthcheck"}, {}).has_value()) << "no socket path: nothing to check";
}

TEST(Usage, NamesTheEnvironmentAndHealthcheckAndSaysTokensAreNeverEnvironment) {  // ADR-0030
    const std::string_view text = wsrv::wsrvd::usage();
    EXPECT_NE(text.find("--healthcheck"), std::string_view::npos);
    EXPECT_NE(text.find("WSRVD_"), std::string_view::npos);
}

TEST(Usage, NamesThePermessageDeflateFlagsAndSaysItIsOffByDefault) {  // ADR-0020 section 6
    const std::string_view text = wsrv::wsrvd::usage();
    for (const char* flag : {"--permessage-deflate",
                             "--deflate-level",
                             "--deflate-min-size",
                             "--deflate-max-ratio",
                             "--deflate-server-max-window-bits",
                             "--deflate-server-no-context-takeover"}) {
        EXPECT_NE(text.find(flag), std::string_view::npos) << flag;
    }
    EXPECT_NE(text.find("off by default"), std::string_view::npos);
}

TEST(Usage, NamesThePerIpLimitFlagsWithTheirDefaults) {  // ADR-0021 section 5
    const std::string_view text = wsrv::wsrvd::usage();
    EXPECT_NE(text.find("--max-connections-per-ip"), std::string_view::npos);
    EXPECT_NE(text.find("--max-new-connections-per-ip-per-second"), std::string_view::npos);
    EXPECT_NE(text.find("0 = no limit"), std::string_view::npos);
}

TEST(Usage, NamesTheBufferedBytesBudgetAndWhatToSetItTo) {  // ADR-0040
    const std::string_view text = wsrv::wsrvd::usage();
    EXPECT_NE(text.find("--max-buffered-bytes"), std::string_view::npos);
    EXPECT_NE(text.find("half the memory limit"), std::string_view::npos);
}

TEST(Usage, NamesTheAdminFlagsAndWarnsThatThePortIsNotAuthenticated) {  // ADR-0023
    const std::string_view text = wsrv::wsrvd::usage();
    EXPECT_NE(text.find("--admin-port"), std::string_view::npos);
    EXPECT_NE(text.find("--admin-host"), std::string_view::npos);
    EXPECT_NE(text.find("no authentication"), std::string_view::npos);
}

TEST_F(EnvironmentTest,
       TheTcpTransportCanBeChosenByTheEnvironmentAlone) {  // docker run -e WSRVD_IPC_PORT=...
    const std::string token = write("token", std::string{kGenerated} + "\n");
    const auto startup = run_with(
        {}, {"WSRVD_IPC_PORT=9300", "WSRVD_IPC_HOST=0.0.0.0", "WSRVD_IPC_TOKEN_FILE=" + token});
    ASSERT_TRUE(startup.has_value());
    EXPECT_TRUE(startup->config.tcp());
    EXPECT_EQ(wsrv::format_endpoint(startup->config.ipc_endpoint()), "0.0.0.0:9300");
}

// `docker run --rm wsrvd --generate-token=...` runs with the variables the image sets; none of them
// matters (and an unknown WSRVD_* one must not stop a run that reads no configuration).
TEST_F(EnvironmentTest, GenerateTokenIgnoresTheEnvironment) {
    const auto startup =
        run_with({"--generate-token=/data/t"},
                 {"WSRVD_LISTEN_HOST=0.0.0.0", "WSRVD_LISTEN_PORT=8080", "WSRVD_NOT_A_KEY=1"});
    ASSERT_TRUE(startup.has_value());
    EXPECT_EQ(startup->generate_token, "/data/t");
}

}  // namespace

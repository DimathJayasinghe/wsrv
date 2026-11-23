// SPDX-License-Identifier: Apache-2.0
#include "wsrvd/startup.hpp"

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

// ADR-0028: the publish endpoint. Its token is a file like the IPC token, never a key or a flag.
TEST_F(StartupTest, ThePublishTokenIsReadFromItsFileAndGivenToTheEmbeddedConfig) {
    const std::string token = write("publish", std::string(40, 'p') + "\n");
    const auto startup = run({"--ipc-socket=/run/x.sock",
                              "--broker=true",
                              "--publish-port=9200",
                              "--publish-token-file=" + token});
    ASSERT_TRUE(startup.has_value());
    EXPECT_TRUE(startup->config.embedded().publish_requested());
    EXPECT_EQ(startup->config.embedded().publish_token(), std::string(40, 'p'));
    EXPECT_TRUE(startup->config.embedded().check_publish().has_value());
}

TEST_F(StartupTest, ThePublishTokenHasNoKeyAndNoFlag) {
    EXPECT_FALSE(
        run({"--ipc-socket=/run/x.sock", "--publish-token=" + std::string(40, 'p')}).has_value());
}

TEST_F(StartupTest, APublishPortAndItsTokenFileComeAsAPair) {
    const std::string token = write("publish", std::string(40, 'p'));
    EXPECT_FALSE(
        run({"--ipc-socket=/run/x.sock", "--broker=true", "--publish-port=9200"}).has_value());
    EXPECT_FALSE(run({"--ipc-socket=/run/x.sock", "--publish-token-file=" + token}).has_value());
}

TEST_F(StartupTest, AShortOrUnsafePublishTokenStopsTheStartup) {
    const std::string shortened = write("short", std::string(31, 'p'));
    const std::string open = write("open", std::string(40, 'p'), 0644);
    for (const std::string& file : {shortened, open}) {
        const auto startup = run({"--ipc-socket=/run/x.sock",
                                  "--broker=true",
                                  "--publish-port=9200",
                                  "--publish-token-file=" + file});
        ASSERT_FALSE(startup.has_value()) << file;
        EXPECT_EQ(startup.error().code, ErrorCode::InvalidConfig);
    }
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

TEST(Usage, NamesTheAdminFlagsAndWarnsThatThePortIsNotAuthenticated) {  // ADR-0023
    const std::string_view text = wsrv::wsrvd::usage();
    EXPECT_NE(text.find("--admin-port"), std::string_view::npos);
    EXPECT_NE(text.find("--admin-host"), std::string_view::npos);
    EXPECT_NE(text.find("no authentication"), std::string_view::npos);
}

}  // namespace

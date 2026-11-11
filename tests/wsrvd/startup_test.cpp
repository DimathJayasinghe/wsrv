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

TEST_F(StartupTest, BadInputIsRefusedBeforeAnythingStarts) {
    EXPECT_FALSE(run({}).has_value());                                          // no socket
    EXPECT_FALSE(run({"--ipc-socket=/run/x.sock", "--bogus=1"}).has_value());   // unknown key
    EXPECT_FALSE(run({"--ipc-socket=/run/x.sock", "stray-word"}).has_value());  // not a flag
    EXPECT_FALSE(run({"--ipc-socket=/run/x.sock", "--ipc-token=s3cret"}).has_value());
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

}  // namespace

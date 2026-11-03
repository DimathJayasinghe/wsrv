// SPDX-License-Identifier: Apache-2.0
#include "wsrvd/token_file.hpp"

#include "support/fake_syscalls.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#include "ipc/message.hpp"

namespace {

namespace fs = std::filesystem;
using wsrv::ErrorCode;
using wsrv::test::Call;
using wsrv::test::FakeSyscalls;
using wsrv::wsrvd::read_token_file;

struct TokenFileTest : public ::testing::Test {
    FakeSyscalls sys;
    std::string dir;
    void SetUp() override {
        std::string pattern = (fs::temp_directory_path() / "wsrv-token-XXXXXX").string();
        ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
        dir = pattern;
    }
    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(dir, ignored);
    }
    std::string write(const std::string& name, const std::string& content, mode_t mode = 0600) const {
        const std::string path = dir + "/" + name;
        {
            std::ofstream file{path, std::ios::binary};
            file << content;
        }
        EXPECT_EQ(::chmod(path.c_str(), mode), 0);
        return path;
    }
};

TEST_F(TokenFileTest, ReadsTheTokenAndDropsOneTrailingNewline) {
    const auto token = read_token_file(sys, write("t", "s3cret\n"));
    ASSERT_TRUE(token.has_value());
    EXPECT_EQ(*token, "s3cret");
    EXPECT_EQ(*read_token_file(sys, write("t2", "s3cret")), "s3cret");
}

TEST_F(TokenFileTest, OnlyOneNewlineIsDroppedAndTheRestIsTheToken) {
    EXPECT_EQ(*read_token_file(sys, write("t", "a\n\n")), "a\n");
    EXPECT_EQ(*read_token_file(sys, write("t2", "a b\tc")), "a b\tc");
}

TEST_F(TokenFileTest, ModeBitsForGroupOrOthersAreRefused) {
    for (const mode_t mode :
         {mode_t{0640}, mode_t{0604}, mode_t{0660}, mode_t{0644}, mode_t{0666}, mode_t{0610}}) {
        const auto token = read_token_file(sys, write("t", "s3cret", mode));
        ASSERT_FALSE(token.has_value()) << std::oct << mode;
        EXPECT_EQ(token.error().code, ErrorCode::InvalidConfig);
    }
}

TEST_F(TokenFileTest, StricterThanOwnerReadWriteIsFine) {
    EXPECT_TRUE(read_token_file(sys, write("t", "s3cret", 0400)).has_value());
}

TEST_F(TokenFileTest, AFileOwnedBySomeoneElseIsRefused) {
    const std::string path = write("t", "s3cret");
    sys.succeed(Call::EffectiveUid, 4242);  // we pretend to be another user
    const auto token = read_token_file(sys, path);
    ASSERT_FALSE(token.has_value());
    EXPECT_EQ(token.error().code, ErrorCode::InvalidConfig);
}

TEST_F(TokenFileTest, ASymlinkIsFollowedAndItsTargetIsChecked) {
    const std::string target = write("real", "s3cret");
    const std::string link = dir + "/link";
    ASSERT_EQ(::symlink(target.c_str(), link.c_str()), 0);
    EXPECT_EQ(*read_token_file(sys, link), "s3cret");
    ASSERT_EQ(::chmod(target.c_str(), 0644), 0);
    EXPECT_FALSE(read_token_file(sys, link).has_value());
}

TEST_F(TokenFileTest, ADirectoryOrAMissingFileIsAConfigurationError) {
    for (const std::string& path : {dir, dir + "/missing"}) {
        const auto token = read_token_file(sys, path);
        ASSERT_FALSE(token.has_value()) << path;
        EXPECT_EQ(token.error().code, ErrorCode::InvalidConfig);
    }
}

TEST_F(TokenFileTest, AnEmptyTokenIsRefusedBecauseItWouldMeanNoProtection) {
    EXPECT_FALSE(read_token_file(sys, write("a", "")).has_value());
    EXPECT_FALSE(read_token_file(sys, write("b", "\n")).has_value());
}

TEST_F(TokenFileTest, TheTokenHasALengthLimitAndANeverBufferedWholeFile) {
    const std::string longest(wsrv::ipc::kMaxTokenSize, 'x');
    EXPECT_TRUE(read_token_file(sys, write("ok", longest + "\n")).has_value());
    EXPECT_FALSE(read_token_file(sys, write("big", longest + "x")).has_value());
    // A huge file is refused from its first bytes, not read to the end.
    const std::string huge(std::size_t{4} << 20U, 'y');
    EXPECT_FALSE(read_token_file(sys, write("huge", huge)).has_value());
}

}  // namespace

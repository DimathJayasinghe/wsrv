// SPDX-License-Identifier: Apache-2.0
#include "wsrvd/token_file.hpp"

#include "net/system_random.hpp"
#include "support/fake_syscalls.hpp"
#include "support/fakes.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#include "ipc/message.hpp"
#include "wsrvd/token_format.hpp"

namespace {

namespace fs = std::filesystem;
using wsrv::ErrorCode;
using wsrv::test::Call;
using wsrv::test::FakeSyscalls;
using wsrv::wsrvd::generate_token_file;
using wsrv::wsrvd::is_generated_token;
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
    std::string
    write(const std::string& name, const std::string& content, mode_t mode = 0600) const {
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

// ---- `wsrvd --generate-token=FILE` (ADR-0038 section 3)
// ------------------------------------------

std::string slurp(const std::string& path) {
    std::ifstream file{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

mode_t mode_of(const std::string& path) {
    struct stat info {};
    EXPECT_EQ(::stat(path.c_str(), &info), 0);
    return info.st_mode & 07777;
}

TEST_F(TokenFileTest, GenerateWritesAGeneratedTokenThatTheReaderAcceptsUnchanged) {
    wsrv::SystemRandom random;
    const std::string path = dir + "/ipc.token";
    ASSERT_TRUE(generate_token_file(sys, random, path).has_value());

    const std::string content = slurp(path);
    ASSERT_EQ(content.size(), wsrv::wsrvd::kGeneratedTokenSize + 1);
    EXPECT_EQ(content.back(), '\n');
    const auto read = read_token_file(sys, path);  // mode and owner pass the daemon's own checks
    ASSERT_TRUE(read.has_value());
    EXPECT_TRUE(is_generated_token(*read));
    EXPECT_EQ(mode_of(path), mode_t{0600});
}

TEST_F(TokenFileTest, TwoGeneratedTokensDiffer) {
    wsrv::SystemRandom random;
    ASSERT_TRUE(generate_token_file(sys, random, dir + "/a").has_value());
    ASSERT_TRUE(generate_token_file(sys, random, dir + "/b").has_value());
    EXPECT_NE(slurp(dir + "/a"), slurp(dir + "/b"));
}

TEST_F(TokenFileTest, GenerateUsesTheRandomSourceAndNothingWeaker) {
    wsrv::test::FakeRandom random{0};  // counts 0, 1, 2, ...: the token is then known
    ASSERT_TRUE(generate_token_file(sys, random, dir + "/known").has_value());
    EXPECT_EQ(slurp(dir + "/known"),
              "wsrv1_000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f_27994e12\n");
}

// An existing token is a deployment's secret: overwriting it silently would lock the application
// out (or, worse, hide that a different token was meant).
TEST_F(TokenFileTest, GenerateRefusesToOverwriteAnExistingFileAndLeavesItAlone) {
    wsrv::SystemRandom random;
    const std::string path = write("old", "precious-existing-token\n");
    const auto result = generate_token_file(sys, random, path);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidConfig);
    EXPECT_NE(std::string{result.error().context}.find("exists"), std::string::npos);
    EXPECT_EQ(slurp(path), "precious-existing-token\n");
}

TEST_F(TokenFileTest, GenerateDoesNotFollowASymlinkToSomewhereElse) {
    wsrv::SystemRandom random;
    const std::string target = write("target", "untouched\n");
    const std::string link = dir + "/link";
    ASSERT_EQ(::symlink(target.c_str(), link.c_str()), 0);
    EXPECT_FALSE(generate_token_file(sys, random, link).has_value());
    EXPECT_EQ(slurp(target), "untouched\n");
}

TEST_F(TokenFileTest, GenerateIsPrivateWhateverTheUmaskIs) {
    wsrv::SystemRandom random;
    const mode_t before =
        ::umask(0);  // the most permissive umask: the file mode must not depend on it
    const auto result = generate_token_file(sys, random, dir + "/open-umask");
    ::umask(before);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(mode_of(dir + "/open-umask"), mode_t{0600});
}

TEST_F(TokenFileTest, GenerateFailsWithoutCreatingAFileWhenThereIsNoRandomSource) {
    wsrv::test::FakeRandom random;
    random.fail_next();
    const auto result = generate_token_file(sys, random, dir + "/none");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::RandomUnavailable);
    EXPECT_FALSE(fs::exists(dir + "/none"));  // never a token made of nothing
}

TEST_F(TokenFileTest, AFailedWriteLeavesNoHalfWrittenTokenBehind) {
    wsrv::SystemRandom random;
    sys.fail(Call::Write, ENOSPC);
    const auto result = generate_token_file(sys, random, dir + "/full-disk");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::Io);
    EXPECT_FALSE(fs::exists(dir + "/full-disk"));  // a file that looks like a token and is not one
}

TEST_F(TokenFileTest, ShortWritesAreCompletedNotTreatedAsSuccess) {
    wsrv::SystemRandom random;
    sys.short_write(5);  // the kernel takes 5 bytes, then 1: the rest must still be written
    sys.short_write(1);
    ASSERT_TRUE(generate_token_file(sys, random, dir + "/short").has_value());
    EXPECT_TRUE(is_generated_token(*read_token_file(sys, dir + "/short")));
}

TEST_F(TokenFileTest, AnInterruptedWriteIsRetried) {
    wsrv::SystemRandom random;
    sys.fail(Call::Write, EINTR, 2);
    ASSERT_TRUE(generate_token_file(sys, random, dir + "/interrupted").has_value());
    EXPECT_TRUE(is_generated_token(*read_token_file(sys, dir + "/interrupted")));
}

TEST_F(TokenFileTest, AWriteThatMakesNoProgressIsAnErrorNotAnEndlessLoop) {
    wsrv::SystemRandom random;
    sys.succeed(Call::Write, 0);
    const auto result = generate_token_file(sys, random, dir + "/stuck");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::Io);
    EXPECT_FALSE(fs::exists(dir + "/stuck"));
}

TEST_F(TokenFileTest, AMissingDirectoryIsAConfigurationError) {
    wsrv::SystemRandom random;
    const auto result = generate_token_file(sys, random, dir + "/no/such/dir/token");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidConfig);
    EXPECT_NE(std::string{result.error().context}.find("directory"), std::string::npos);
}

}  // namespace

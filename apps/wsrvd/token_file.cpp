// SPDX-License-Identifier: Apache-2.0
#include "wsrvd/token_file.hpp"

#include <array>
#include <cstddef>
#include <fstream>
#include <ios>

#include <sys/stat.h>

#include "ipc/message.hpp"

namespace wsrv::wsrvd {

namespace {

constexpr std::uint32_t kGroupOrOtherBits = S_IRWXG | S_IRWXO;

}  // namespace

Result<std::string> read_token_file(ISyscalls& sys, const std::string& path) {
    FileInfo info;
    const SysResult looked = sys.stat_path(path.c_str(), true, info);
    if (looked.err == ENOENT || looked.err == ENOTDIR) {
        return fail(ErrorCode::InvalidConfig, "token file does not exist");
    }
    if (looked.err != 0) {
        return std::unexpected{map_errno(looked.err, "stat token file")};
    }
    if ((info.mode & S_IFMT) != S_IFREG) {
        return fail(ErrorCode::InvalidConfig, "token file is not a regular file");
    }
    if (info.uid != sys.effective_uid() && info.uid != 0) {
        return fail(ErrorCode::InvalidConfig, "token file is owned by another user");
    }
    if ((info.mode & kGroupOrOtherBits) != 0) {
        return fail(ErrorCode::InvalidConfig,
                    "token file must not be accessible by group or others (use mode 0600)");
    }
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return fail(ErrorCode::Io, "cannot open token file");
    }
    // Read at most one byte more than the longest legal token + newline, so a huge file costs
    // nothing and is recognised as too long.
    std::array<char, ipc::kMaxTokenSize + 2> buffer{};
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    std::string token{buffer.data(), static_cast<std::size_t>(file.gcount())};
    if (!token.empty() && token.back() == '\n') {
        token.pop_back();
    }
    if (token.empty()) {
        return fail(ErrorCode::InvalidConfig, "token file is empty");
    }
    if (token.size() > ipc::kMaxTokenSize) {
        return fail(ErrorCode::InvalidConfig, "token is longer than 256 bytes");
    }
    return token;
}

}  // namespace wsrv::wsrvd

// SPDX-License-Identifier: Apache-2.0
#include "wsrvd/token_file.hpp"

#include <array>
#include <cstddef>
#include <fstream>
#include <ios>

#include <fcntl.h>
#include <sys/stat.h>

#include "ipc/message.hpp"
#include "wsrvd/token_format.hpp"

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

namespace {

/// Write all of `data` to `fd`, completing short writes and retrying EINTR.
Result<void> write_all(ISyscalls& sys, int fd, std::string_view data) {
    while (!data.empty()) {
        const SysResult done = sys.write(fd, data.data(), data.size());
        if (done.err == EINTR) {
            continue;
        }
        if (done.err != 0) {
            return std::unexpected{map_errno(done.err, "write token file")};
        }
        if (done.value <= 0) {
            return fail(ErrorCode::Io, "write token file: no progress");
        }
        data.remove_prefix(static_cast<std::size_t>(done.value));
    }
    return {};
}

Error open_error(int err) noexcept {
    switch (err) {
    case EEXIST:
        return Error{ErrorCode::InvalidConfig,
                     "the token file exists already; it is never overwritten (delete it first to "
                     "replace the token)"};
    case ENOENT:
    case ENOTDIR:
        return Error{ErrorCode::InvalidConfig, "the token file's directory does not exist"};
    default:
        return map_errno(err, "create token file");
    }
}

}  // namespace

Result<void> generate_token_file(ISyscalls& sys, IRandom& random, const std::string& path) {
    std::array<std::byte, kGeneratedTokenRandomBytes> bytes{};
    if (const Result<void> filled = random.fill(MutableByteView{bytes}); !filled) {
        return filled;
    }
    std::string text = format_generated_token(ByteView{bytes.data(), bytes.size()});
    text += '\n';
    bytes.fill(std::byte{0});  // the only other copy of the secret

    // 0600 before the umask, which can only remove bits; O_EXCL refuses an existing file and does
    // not follow a symlink; O_NOFOLLOW says it twice for the path's last component.
    const SysResult opened =
        sys.open_file(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, S_IRUSR | S_IWUSR);
    if (opened.err != 0) {
        return std::unexpected{open_error(opened.err)};
    }
    const int fd = static_cast<int>(opened.value);
    const Result<void> written = write_all(sys, fd, text);
    sys.close(fd);
    if (!written) {
        static_cast<void>(sys.unlink_path(path.c_str()));  // no half-written token left behind
        return written;
    }
    return {};
}

}  // namespace wsrv::wsrvd

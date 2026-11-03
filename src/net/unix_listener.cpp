// SPDX-License-Identifier: Apache-2.0
#include "net/unix_listener.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

#include <sys/stat.h>
#include <sys/un.h>

namespace wsrv {

namespace {

constexpr std::uint32_t kSocketMode = 0600;  // owner only (ADR-0017 section 4)
constexpr std::uint32_t kGroupOrOtherWrite = S_IWGRP | S_IWOTH;

[[nodiscard]] bool is_socket(std::uint32_t mode) noexcept {
    return (mode & S_IFMT) == S_IFSOCK;
}

[[nodiscard]] bool is_directory(std::uint32_t mode) noexcept {
    return (mode & S_IFMT) == S_IFDIR;
}

/// The path must be a real filesystem name that fits: a name that does not fit would be cut, and a
/// cut name is a different file. A leading NUL would select the abstract namespace, which has no
/// permissions; a later NUL would silently shorten the name.
[[nodiscard]] Result<void> check_path(const std::string& path) {
    const sockaddr_un probe{};
    if (path.empty()) {
        return fail(ErrorCode::InvalidConfig, "unix socket path is empty");
    }
    if (path.find('\0') != std::string::npos) {
        return fail(ErrorCode::InvalidConfig, "unix socket path contains a NUL byte");
    }
    if (path.size() >= sizeof(probe.sun_path)) {
        return fail(ErrorCode::InvalidConfig, "unix socket path is too long for sockaddr_un");
    }
    return {};
}

/// Everything before the last '/', "/" for a file in the root, "." for a bare file name.
[[nodiscard]] std::string parent_of(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    if (slash == std::string::npos) {
        return ".";
    }
    return slash == 0 ? "/" : path.substr(0, slash);
}

/// Anyone who can write into the directory (its owner included) can replace the socket between our
/// checks and our use (a race known as TOCTOU). A sticky bit only stops deleting other users'
/// files, not creating a look-alike first, so a sticky world-writable directory such as /tmp is
/// refused too.
[[nodiscard]] Result<void> check_directory(ISyscalls& sys, const std::string& directory) {
    FileInfo info;
    const SysResult result = sys.stat_path(directory.c_str(), true, info);
    if (result.err == ENOENT || result.err == ENOTDIR) {
        return fail(ErrorCode::InvalidConfig, "unix socket directory does not exist");
    }
    if (result.err != 0) {
        return std::unexpected{map_errno(result.err, "stat socket directory")};
    }
    if (!is_directory(info.mode)) {
        return fail(ErrorCode::InvalidConfig, "unix socket parent is not a directory");
    }
    // The owner can always rename things inside, so the owner must be us (or root, who can anyway).
    if (info.uid != sys.effective_uid() && info.uid != 0) {
        return fail(ErrorCode::InvalidConfig, "unix socket directory is owned by another user");
    }
    if ((info.mode & kGroupOrOtherWrite) != 0) {
        return fail(ErrorCode::InvalidConfig,
                    "unix socket directory is writable by group or others");
    }
    return {};
}

[[nodiscard]] socklen_t make_address(const std::string& path, sockaddr_un& address) {
    address = sockaddr_un{};
    address.sun_family = AF_UNIX;
    std::ranges::copy(path, static_cast<char*>(address.sun_path));  // check_path made sure it fits
    return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
}

[[nodiscard]] Result<Fd> new_socket(ISyscalls& sys) {
    const Result<long> created =
        retrying([&] { return sys.socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0); },
                 "socket");
    if (!created) {
        return std::unexpected{created.error()};
    }
    return Fd{static_cast<int>(*created)};
}

/// Ask whether anybody listens on `address`. Only ECONNREFUSED proves nobody does (the file is a
/// leftover). A connection, a full backlog (EAGAIN: somebody is there, just busy) and every other
/// answer mean "in use or unknown", and we do not touch the file.
[[nodiscard]] Result<bool> is_stale(ISyscalls& sys, const sockaddr_un& address, socklen_t length) {
    const Result<Fd> probe = new_socket(sys);
    if (!probe) {
        return std::unexpected{probe.error()};
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* generic = reinterpret_cast<const sockaddr*>(&address);
    const SysResult answer = sys.connect(probe->get(), generic, length);
    if (answer.err == ECONNREFUSED || answer.err == ENOENT) {
        return true;
    }
    if (answer.err == 0 || answer.err == EAGAIN) {
        return fail(ErrorCode::AddressInUse, "another process is listening on the unix socket");
    }
    return std::unexpected{map_errno(answer.err, "probe existing unix socket")};
}

/// Make the path free if, and only if, what is there is our own abandoned socket.
[[nodiscard]] Result<void> clear_stale(ISyscalls& sys, const std::string& path) {
    FileInfo existing;
    const SysResult looked = sys.stat_path(path.c_str(), false, existing);  // lstat: no links
    if (looked.err == ENOENT) {
        return {};
    }
    if (looked.err != 0) {
        return std::unexpected{map_errno(looked.err, "stat unix socket path")};
    }
    if (!is_socket(existing.mode)) {
        return fail(ErrorCode::InvalidConfig, "unix socket path exists and is not a socket");
    }
    if (existing.uid != sys.effective_uid()) {
        return fail(ErrorCode::InvalidConfig, "unix socket path is owned by another user");
    }
    sockaddr_un address{};
    const socklen_t length = make_address(path, address);
    const Result<bool> stale = is_stale(sys, address, length);
    if (!stale) {
        return std::unexpected{stale.error()};
    }
    const SysResult removed = sys.unlink_path(path.c_str());
    if (removed.err != 0 && removed.err != ENOENT) {
        return std::unexpected{map_errno(removed.err, "remove stale unix socket")};
    }
    return {};
}

}  // namespace

UnixListener::UnixListener(ISyscalls& sys, Fd fd, std::string path, FileInfo created) noexcept
    : sys_{&sys}, fd_{std::move(fd)}, path_{std::move(path)}, created_{created} {}

UnixListener::UnixListener(UnixListener&& other) noexcept
    : sys_{other.sys_}, fd_{std::move(other.fd_)}, path_{std::move(other.path_)},
      created_{other.created_}, owns_file_{std::exchange(other.owns_file_, false)} {}

UnixListener& UnixListener::operator=(UnixListener&& other) noexcept {
    if (this != &other) {
        remove_if_ours();
        sys_ = other.sys_;
        fd_ = std::move(other.fd_);
        path_ = std::move(other.path_);
        created_ = other.created_;
        owns_file_ = std::exchange(other.owns_file_, false);
    }
    return *this;
}

UnixListener::~UnixListener() {
    remove_if_ours();
}

/// Unlink the path only while it is still the file we created: a different inode means someone
/// replaced it, and removing their file would be our bug.
void UnixListener::remove_if_ours() noexcept {
    if (!owns_file_) {
        return;
    }
    owns_file_ = false;
    FileInfo now;
    if (sys_->stat_path(path_.c_str(), false, now).err == 0 && is_socket(now.mode) &&
        now.same_file_as(created_)) {
        (void)sys_->unlink_path(path_.c_str());
    }
}

Result<UnixListener> UnixListener::open(ISyscalls& sys, const UnixListenerConfig& config) {
    if (const Result<void> ok = check_path(config.path); !ok) {
        return std::unexpected{ok.error()};
    }
    // The directory first: until it is known to be private, nothing in it may be trusted or
    // removed.
    if (const Result<void> ok = check_directory(sys, parent_of(config.path)); !ok) {
        return std::unexpected{ok.error()};
    }
    if (const Result<void> ok = clear_stale(sys, config.path); !ok) {
        return std::unexpected{ok.error()};
    }
    Result<Fd> socket = new_socket(sys);
    if (!socket) {
        return std::unexpected{socket.error()};
    }
    sockaddr_un address{};
    const socklen_t length = make_address(config.path, address);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* generic = reinterpret_cast<const sockaddr*>(&address);
    const Result<long> bound =
        retrying([&] { return sys.bind(socket->get(), generic, length); }, "bind unix socket");
    if (!bound) {
        return std::unexpected{bound.error()};
    }
    // bind() created the file. From here on, every failure must remove it again, so the object that
    // owns it exists before anything else can go wrong; its destructor does the cleanup.
    FileInfo created;
    if (const SysResult looked = sys.stat_path(config.path.c_str(), false, created);
        looked.err != 0) {
        (void)sys.unlink_path(config.path.c_str());  // cannot record its identity: do not leave it
        return std::unexpected{map_errno(looked.err, "stat new unix socket")};
    }
    UnixListener listener{sys, std::move(*socket), config.path, created};
    // chmod BEFORE listen: until listen() nothing can connect, so the socket is never reachable
    // with permissions other than ours (and no process-wide umask is involved).
    if (const Result<long> ok = retrying(
            [&] { return sys.chmod_path(config.path.c_str(), kSocketMode); }, "chmod unix socket");
        !ok) {
        return std::unexpected{ok.error()};
    }
    if (const Result<long> ok =
            retrying([&] { return sys.listen(listener.fd_.get(), config.backlog); }, "listen");
        !ok) {
        return std::unexpected{ok.error()};
    }
    return listener;
}

}  // namespace wsrv

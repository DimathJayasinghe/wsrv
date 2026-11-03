// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/error.hpp"
#include "net/fd.hpp"
#include "net/syscalls.hpp"

#include <string>
#include <string_view>

namespace wsrv {

struct UnixListenerConfig {
    /// A filesystem path. Abstract-namespace names (leading NUL) are refused: they have no file
    /// permissions at all, so any local user could connect (ADR-0017 section 4).
    std::string path;
    int backlog = 16;  ///< the IPC socket serves one consumer; a short queue is plenty
};

/// A bound, listening, non-blocking, close-on-exec Unix-domain socket whose file is safe to leave
/// on disk (ADR-0017 section 4). Everything that makes a socket FILE trustworthy happens in open():
///
///  1. The path must fit sockaddr_un (never truncated: a truncated name is a different file) and
///     contain no NUL.
///  2. The parent directory must not be writable by group or others. Anyone who can write there can
///     swap the socket for their own between our checks and our use. A sticky bit does not help
///     (bare /tmp is refused on purpose).
///  3. If something already sits at the path it is removed only if it is a socket (lstat, which
///  does
///     not follow links, so a symlink is "not a socket"), owned by our effective uid, and nobody
///     answers on it: a probe connect() must fail with ECONNREFUSED. Connected, a full backlog, or
///     any other answer means "in use or unknown": refuse and touch nothing.
///  4. bind(), then chmod 0600, then listen(). Until listen() nothing can connect, so there is no
///     moment when the socket is reachable with the wrong permissions, and no process-wide umask is
///     needed.
///
/// The destructor removes the socket file, but only if it is still the file this listener created
/// (same device and inode): if someone replaced it, it is not ours to delete.
/// Portability: POSIX (Linux, macOS). Windows will need its own transport behind the same use.
/// Thread-affinity: loop-thread-only.
class UnixListener {
public:
    /// @return Error{InvalidConfig} for an unusable path, an unsafe directory or an unsafe existing
    /// file; Error{AddressInUse} if a live socket is there; Error{FdLimit}/Error{Io} from the
    /// kernel. On failure nothing this call created is left behind.
    [[nodiscard]] static Result<UnixListener> open(ISyscalls& sys,
                                                   const UnixListenerConfig& config);

    UnixListener(UnixListener&& other) noexcept;
    UnixListener& operator=(UnixListener&& other) noexcept;
    UnixListener(const UnixListener&) = delete;
    UnixListener& operator=(const UnixListener&) = delete;
    ~UnixListener();

    [[nodiscard]] int fd() const noexcept {
        return fd_.get();
    }
    [[nodiscard]] const std::string& path() const noexcept {
        return path_;
    }

private:
    UnixListener(ISyscalls& sys, Fd fd, std::string path, FileInfo created) noexcept;
    void remove_if_ours() noexcept;

    ISyscalls* sys_;
    Fd fd_;
    std::string path_;
    FileInfo created_;
    bool owns_file_ = true;
};

}  // namespace wsrv

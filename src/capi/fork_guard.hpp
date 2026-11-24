// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <cstdint>

#include <sys/types.h>

namespace wsrv {

/// Tells an object that it is being used in a child created by fork(), without a system call per
/// question. glibc stopped caching getpid() in 2.25, so asking the kernel on every poll and send
/// cost a syscall per message. A private page marked MADV_WIPEONFORK (Linux 4.14) holds a 1 in the
/// process that made it and reads 0 in every child: the kernel clears it at fork. Where the
/// kernel refuses the flag, forked() falls back to comparing getpid().
class ForkGuard {
public:
    /// @param use_wipe_on_fork false skips the page and always compares getpid(): what runs on a
    /// kernel without MADV_WIPEONFORK, selectable so that path can be tested.
    explicit ForkGuard(bool use_wipe_on_fork = true) noexcept;
    ~ForkGuard();
    ForkGuard(const ForkGuard&) = delete;
    ForkGuard& operator=(const ForkGuard&) = delete;
    ForkGuard(ForkGuard&&) = delete;
    ForkGuard& operator=(ForkGuard&&) = delete;

    /// @param owner_pid the pid of the process that made the object.
    [[nodiscard]] bool forked(pid_t owner_pid) const noexcept;

private:
    std::atomic<std::uint8_t>* mark_ = nullptr;  // in a MADV_WIPEONFORK page, or null
};

}  // namespace wsrv

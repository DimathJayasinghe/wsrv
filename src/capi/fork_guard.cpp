// SPDX-License-Identifier: Apache-2.0
#include "capi/fork_guard.hpp"

#include <memory>
#include <new>

#include <sys/mman.h>
#include <unistd.h>

#ifndef MADV_WIPEONFORK
#define MADV_WIPEONFORK 18  // Linux 4.14; older headers lack the name
#endif

namespace wsrv {

ForkGuard::ForkGuard(bool use_wipe_on_fork) noexcept {
    if (!use_wipe_on_fork) {
        return;
    }
    const long page = ::sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        return;
    }
    void* memory =
        ::mmap(nullptr, static_cast<std::size_t>(page), PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);  // NOLINT(cppcoreguidelines-pro-type-vararg)
    if (memory == MAP_FAILED) {                      // NOLINT(performance-no-int-to-ptr)
        return;
    }
    if (::madvise(memory, static_cast<std::size_t>(page), MADV_WIPEONFORK) != 0) {
        ::munmap(memory, static_cast<std::size_t>(page));  // an older kernel: use getpid()
        return;
    }
    mark_ = std::construct_at(static_cast<std::atomic<std::uint8_t>*>(memory), std::uint8_t{1});
}

ForkGuard::~ForkGuard() {
    if (mark_ != nullptr) {
        const long page = ::sysconf(_SC_PAGESIZE);
        ::munmap(static_cast<void*>(mark_), static_cast<std::size_t>(page));
    }
}

bool ForkGuard::forked(pid_t owner_pid) const noexcept {
    if (mark_ != nullptr) {
        return mark_->load(std::memory_order_relaxed) != 1;
    }
    return ::getpid() != owner_pid;
}

}  // namespace wsrv

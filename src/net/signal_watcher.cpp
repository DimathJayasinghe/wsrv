// SPDX-License-Identifier: Apache-2.0
#include "net/signal_watcher.hpp"

#include <cerrno>
#include <new>
#include <span>

#include <sys/signalfd.h>

namespace wsrv {

namespace {

/// A signalfd read returns whole records; there are at most NSIG distinct pending standard signals
/// per read burst, so this bounds the drain loop without ever cutting a real burst short.
constexpr std::size_t kMaxRecordsPerDrain = 128;

[[nodiscard]] Result<sigset_t> make_set(std::span<const int> signals) {
    sigset_t set;
    sigemptyset(&set);
    for (const int signo : signals) {
        if (sigaddset(&set, signo) != 0) {
            return fail(ErrorCode::InvalidArgument, "signal watcher: invalid signal number");
        }
    }
    return set;
}

/// Block `set` for this thread and open a signalfd for it. All or nothing: if the descriptor cannot
/// be opened the previous mask is put back. `previous` receives the mask to restore later.
[[nodiscard]] Result<Fd> block_and_open(ISyscalls& sys, const sigset_t& set, sigset_t& previous) {
    sigemptyset(&previous);
    const Result<long> blocked =
        retrying([&] { return sys.thread_sigmask(SIG_BLOCK, &set, &previous); }, "pthread_sigmask");
    if (!blocked) {
        return std::unexpected{blocked.error()};
    }
    const Result<long> raw =
        retrying([&] { return sys.signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC); }, "signalfd");
    if (!raw) {
        static_cast<void>(sys.thread_sigmask(SIG_SETMASK, &previous, nullptr));
        return std::unexpected{raw.error()};
    }
    return Fd{static_cast<int>(*raw)};
}

}  // namespace

Result<std::unique_ptr<SignalWatcher>> SignalWatcher::create(EventLoop& loop,
                                                             ISyscalls& sys,
                                                             std::span<const int> signals,
                                                             Callback callback) {
    if (signals.empty() || !callback) {
        return fail(ErrorCode::InvalidArgument, "signal watcher: nothing to watch or no callback");
    }
    const Result<sigset_t> set = make_set(signals);
    if (!set) {
        return std::unexpected{set.error()};
    }
    sigset_t previous;
    Result<Fd> fd = block_and_open(sys, *set, previous);
    if (!fd) {
        return std::unexpected{fd.error()};
    }
    std::unique_ptr<SignalWatcher> watcher;
    try {
        watcher = std::make_unique<SignalWatcher>(
            Key{}, loop, sys, std::move(callback), std::move(*fd), previous);
    } catch (const std::bad_alloc&) {
        static_cast<void>(sys.thread_sigmask(SIG_SETMASK, &previous, nullptr));
        return fail(ErrorCode::ResourceExhausted, "signal watcher: out of memory");
    }
    // From here the destructor owns the cleanup (drain the signalfd, restore the mask).
    const Result<WatchHandle> watch =
        loop.watch(watcher->signal_fd_.get(), PollEvents::Read, *watcher);
    if (!watch) {
        return std::unexpected{watch.error()};
    }
    watcher->watch_ = *watch;
    return watcher;
}

SignalWatcher::SignalWatcher(Key /*unused*/,
                             EventLoop& loop,
                             ISyscalls& sys,
                             Callback callback,
                             Fd signal_fd,
                             const sigset_t& previous_mask)
    : loop_{loop}, sys_{sys}, callback_{std::move(callback)}, signal_fd_{std::move(signal_fd)},
      previous_mask_{previous_mask} {}

SignalWatcher::~SignalWatcher() {
    loop_.unwatch(watch_);
    drain(false);
    static_cast<void>(sys_.thread_sigmask(SIG_SETMASK, &previous_mask_, nullptr));
}

// NOLINTNEXTLINE(bugprone-exception-escape): the Callback contract (header) says it must not throw
void SignalWatcher::drain(bool deliver) noexcept {
    for (std::size_t i = 0; i < kMaxRecordsPerDrain; ++i) {
        signalfd_siginfo info{};
        const SysResult got = sys_.read(signal_fd_.get(), &info, sizeof(info));
        if (got.err == EINTR) {
            continue;
        }
        if (got.err != 0 || got.value != static_cast<long>(sizeof(info))) {
            return;  // WouldBlock (drained) or a failure: nothing more to read
        }
        if (deliver) {
            callback_(static_cast<int>(info.ssi_signo));
        }
    }
}

void SignalWatcher::on_event(PollEvents /*events*/) noexcept {
    drain(true);
}

}  // namespace wsrv

// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/clock.hpp"
#include "base/log_rate_limiter.hpp"
#include "base/logger.hpp"
#include "net/syscalls.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace wsrv {

/// Writes one structured line per record to a file descriptor, normally stderr (ADR-0037).
///
/// - Below `min_level` a call costs one comparison: no formatting, no lock, no system call.
/// - A flood of one event is held to a burst, then one line a second, with the count of the
/// held-back
///   ones on the next line (`LogRateLimiter`).
/// - The line (at most kLogMaxLine bytes, so never split by another writer on a pipe or an
///   `O_APPEND` file) goes out in one `write`, after a zero-timeout `poll` has said the descriptor
///   can take it. If it cannot, or the write fails or is short, the line is dropped and counted,
///   and the next line that gets out carries `dropped=N`. A stuck destination therefore costs lines
///   and never stalls the loop that logs.
///
/// Thread-affinity: any thread. The only lock guards the rate limiter's few counters and is held
/// for a handful of instructions; formatting and the system calls happen outside it.
class StreamLogger final : public ILogger {
public:
    /// `fd` stays owned by the caller (it is never closed here); a negative `fd` means "no
    /// destination": nothing is enabled and nothing is written.
    StreamLogger(ISyscalls& sys,
                 const IWallClock& wall,
                 const IClock& clock,
                 int fd,
                 LogLevel min_level) noexcept
        : sys_{sys}, wall_{wall}, clock_{clock}, fd_{fd}, min_level_{min_level} {}

    using ILogger::log;
    void log(const LogRecord& record) override;

    [[nodiscard]] bool enabled(LogLevel level) const noexcept override {
        return fd_ >= 0 && level >= min_level_;
    }

private:
    [[nodiscard]] bool write_line(const std::string& line) noexcept;

    ISyscalls& sys_;
    const IWallClock& wall_;
    const IClock& clock_;
    int fd_;
    LogLevel min_level_;
    std::mutex mutex_;
    LogRateLimiter limiter_;
    std::atomic<std::uint64_t> dropped_{0};  ///< lines lost since the last one that got out
};

}  // namespace wsrv

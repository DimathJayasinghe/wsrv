// SPDX-License-Identifier: Apache-2.0
#include "net/stream_logger.hpp"

#include "base/log_format.hpp"

#include <cerrno>
#include <exception>

#include <poll.h>

namespace wsrv {

bool StreamLogger::write_line(const std::string& line) noexcept {
    pollfd probe{fd_, POLLOUT, 0};
    const SysResult ready = sys_.poll(&probe, 1, 0);
    if (ready.value < 1 || (probe.revents & POLLOUT) == 0) {
        return false;  // a full pipe: write() would block, and this thread is an I/O loop
    }
    for (int attempt = 0; attempt < 2; ++attempt) {  // one retry for EINTR
        const SysResult written = sys_.write(fd_, line.data(), line.size());
        if (written.value >= 0) {
            return static_cast<std::size_t>(written.value) == line.size();
        }
        if (written.err != EINTR) {
            return false;
        }
    }
    return false;
}

void StreamLogger::log(const LogRecord& record) {
    if (!enabled(record.level)) {
        return;
    }
    try {
        std::uint64_t suppressed = 0;
        {
            const std::lock_guard lock{mutex_};
            const LogRateLimiter::Decision decision = limiter_.admit(record.event, clock_.now());
            if (!decision.emit) {
                return;
            }
            suppressed = decision.suppressed;
        }
        const std::uint64_t carried = dropped_.exchange(0, std::memory_order_relaxed);
        const std::string line = format_line(wall_.now(), record, suppressed, carried);
        if (!write_line(line)) {
            dropped_.fetch_add(carried + 1, std::memory_order_relaxed);
        }
    } catch (
        const std::exception&) {  // NOLINT(bugprone-empty-catch): logging never fails its caller
        dropped_.fetch_add(
            1, std::memory_order_relaxed);  // (allocation failed: the line is lost, counted)
    }
}

}  // namespace wsrv

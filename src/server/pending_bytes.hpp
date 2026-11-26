// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>

namespace wsrv {

/// Per loop: the most payload bytes that direct sends (SessionHandle::send_text / send_binary)
/// may have copied into tasks the loop has not run yet (ADR-0032). The task queue bounds the
/// *number* of waiting sends; this bounds their *size*, which the host controls and a count cannot.
inline constexpr std::size_t kDefaultMaxPendingSendBytes = std::size_t{64} << 20;

/// A counter with a ceiling, shared between a LoopLink and the tasks it posted. Lock-free:
/// producers on any thread add, the loop thread (or a destroyed task) subtracts. Thread-affinity:
/// any thread.
class PendingBytes {
public:
    explicit PendingBytes(std::size_t limit) noexcept : limit_{limit} {}

    /// Reserve `bytes` if they fit under the limit. A failed call changes nothing.
    [[nodiscard]] bool try_add(std::size_t bytes) noexcept {
        std::size_t used = used_.load(std::memory_order_relaxed);
        do {
            if (bytes > limit_ - used) {  // used <= limit_ always, so no underflow
                return false;
            }
        } while (!used_.compare_exchange_weak(
            used, used + bytes, std::memory_order_relaxed, std::memory_order_relaxed));
        return true;
    }

    void release(std::size_t bytes) noexcept {
        used_.fetch_sub(bytes, std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t used() const noexcept {
        return used_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::size_t limit() const noexcept {
        return limit_;
    }

private:
    std::atomic<std::size_t> used_{0};
    const std::size_t limit_;
};

/// Holds a reservation and gives it back when the last copy is destroyed: after the task ran, or
/// when it is discarded unrun (queue full, loop destroyed). std::function needs a copyable
/// callable, so the closure holds this through a shared_ptr.
class SendLease {
public:
    SendLease(std::shared_ptr<PendingBytes> bytes, std::size_t size) noexcept
        : bytes_{std::move(bytes)}, size_{size} {}
    SendLease(const SendLease&) = delete;
    SendLease& operator=(const SendLease&) = delete;
    SendLease(SendLease&&) = delete;
    SendLease& operator=(SendLease&&) = delete;
    ~SendLease() {
        bytes_->release(size_);
    }

private:
    std::shared_ptr<PendingBytes> bytes_;
    std::size_t size_;
};

}  // namespace wsrv

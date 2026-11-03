// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <cstddef>
#include <optional>

namespace wsrv {

class PendingGate;

/// One place among the upgrades that may wait for the application at once. Returned by
/// PendingGate::try_enter(); giving it up (destruction) frees the place, exactly once, however the
/// wait ends (decision, timeout, disconnect, shutdown, abort, destructor).
/// Thread-affinity: any thread.
class PendingSlot {
public:
    PendingSlot(const PendingSlot&) = delete;
    PendingSlot& operator=(const PendingSlot&) = delete;
    PendingSlot(PendingSlot&& other) noexcept : gate_{other.gate_} {
        other.gate_ = nullptr;
    }
    PendingSlot& operator=(PendingSlot&&) = delete;
    ~PendingSlot();

private:
    friend class PendingGate;
    explicit PendingSlot(PendingGate& gate) noexcept : gate_{&gate} {}
    PendingGate* gate_;
};

/// The bound on upgrades waiting for the application, shared by every loop of one server (like
/// the connection cap). A lock-free counter: no mutex in connection code (CLAUDE.md).
/// Thread-affinity: any thread. Must outlive every slot it handed out.
class PendingGate {
public:
    explicit PendingGate(std::size_t bound) noexcept : bound_{bound} {}
    PendingGate(const PendingGate&) = delete;
    PendingGate& operator=(const PendingGate&) = delete;
    PendingGate(PendingGate&&) = delete;
    PendingGate& operator=(PendingGate&&) = delete;
    ~PendingGate() = default;

    /// A place, or nothing if `bound` upgrades already wait.
    [[nodiscard]] std::optional<PendingSlot> try_enter() noexcept {
        std::size_t seen = waiting_.load();
        while (seen < bound_) {
            if (waiting_.compare_exchange_weak(seen, seen + 1)) {
                return PendingSlot{*this};
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::size_t waiting() const noexcept {
        return waiting_.load();
    }

private:
    friend class PendingSlot;
    std::size_t bound_;
    std::atomic<std::size_t> waiting_{0};
};

inline PendingSlot::~PendingSlot() {
    if (gate_ != nullptr) {
        gate_->waiting_.fetch_sub(1);
    }
}

}  // namespace wsrv

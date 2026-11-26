// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <cstddef>

namespace wsrv {

/// One connection's share of the server's buffered-bytes counter (ADR-0040).
///
/// The connection says how many bytes it holds right now (`set`); the ledger adds the difference
/// to the shared counter. What it has charged is remembered here, so the refund is exactly that,
/// whatever happened in between, and it runs from the destructor: a connection that ends in any
/// way (a close, an error, an abort, a server torn down with live clients) cannot leave bytes
/// behind. A counter that only ever grows would turn the budget into an outage.
///
/// A connection that holds nothing never touches the shared counter (`set(0)` while charged 0 is
/// a comparison), so idle and echo connections do not make the loops contend on one cache line.
/// The counter is a gauge for a soft limit, not a synchronisation point: relaxed ordering.
///
/// Thread-affinity: one ledger belongs to one connection and its loop thread; the counter it
/// points to is shared by every loop.
class BufferLedger {
public:
    /// `total` may be null (tests, fakes): the ledger then charges nothing.
    explicit BufferLedger(std::atomic<std::size_t>* total = nullptr) noexcept : total_{total} {}

    BufferLedger(const BufferLedger&) = delete;
    BufferLedger& operator=(const BufferLedger&) = delete;
    BufferLedger(BufferLedger&&) = delete;  // a moved-from ledger would refund twice
    BufferLedger& operator=(BufferLedger&&) = delete;
    ~BufferLedger() {
        release();
    }

    /// The connection holds `now` bytes: charge or refund the difference.
    void set(std::size_t now) noexcept {
        if (now == charged_) {
            return;
        }
        if (total_ != nullptr) {
            if (now > charged_) {
                total_->fetch_add(now - charged_, std::memory_order_relaxed);
            } else {
                total_->fetch_sub(charged_ - now, std::memory_order_relaxed);
            }
        }
        charged_ = now;
    }

    /// Give everything back (idempotent).
    void release() noexcept {
        set(0);
    }

    [[nodiscard]] std::size_t charged() const noexcept {
        return charged_;
    }

private:
    std::atomic<std::size_t>* total_;
    std::size_t charged_ = 0;
};

}  // namespace wsrv

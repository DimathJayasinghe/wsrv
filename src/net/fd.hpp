// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace wsrv {

/// Sole owner of a POSIX file descriptor: closed exactly once, when the owner dies or is reset.
/// Move-only, so a descriptor can neither leak (every path ends in a destructor) nor be closed
/// twice (a second owner cannot exist) - the classic ways a server runs out of descriptors or
/// closes someone else's after the number is reused (SECURITY_DESIGN T3, EC-N12).
///
/// close() is called once and never retried on EINTR: on Linux the descriptor is released even
/// when close reports EINTR, so a retry could close a different, newly opened descriptor.
/// Thread-affinity: not synchronised; one owner at a time.
class Fd {
public:
    explicit Fd(int raw = -1) noexcept : raw_{raw} {}
    Fd(Fd&& other) noexcept : raw_{other.release()} {}
    Fd& operator=(Fd&& other) noexcept {
        reset(other.release());
        return *this;
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd();

    [[nodiscard]] int get() const noexcept {
        return raw_;
    }
    [[nodiscard]] bool valid() const noexcept {
        return raw_ >= 0;
    }

    /// Give up ownership without closing; the caller becomes responsible for the descriptor.
    [[nodiscard]] int release() noexcept {
        const int old = raw_;
        raw_ = -1;
        return old;
    }

    /// Close the current descriptor (if any) and own `raw` instead. Resetting to the descriptor
    /// already owned is a no-op.
    void reset(int raw = -1) noexcept;

private:
    int raw_;
};

}  // namespace wsrv

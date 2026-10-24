// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/error.hpp"

#include <string_view>

namespace wsrv {

/// Translate an `errno` value into our error model. The caller must have captured `errno` right
/// after the failing call (CODING_STANDARDS: no errno reads after another call), so this takes the
/// number, never reads the global. `context` must be a string literal.
///
///  EAGAIN / EWOULDBLOCK              -> WouldBlock        (normal: wait for readiness)
///  EPIPE, ECONNRESET, ECONNABORTED,
///  ETIMEDOUT, ENOTCONN, EHOSTUNREACH -> ConnectionClosed  (the peer is gone; never fatal)
///  EMFILE, ENFILE, ENOBUFS, ENOMEM   -> FdLimit           (resource exhaustion: shed load)
///  EADDRINUSE                        -> AddressInUse
///  anything else                     -> Io
///
/// Thread-affinity: pure, any-thread.
[[nodiscard]] Error map_errno(int err, std::string_view context) noexcept;

}  // namespace wsrv

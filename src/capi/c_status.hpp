// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/error.hpp"
#include "wsrv/wsrv.h"

namespace wsrv {

/// The one table from internal error codes to the codes of the C API. Every code is listed on
/// purpose: a new ErrorCode that is not mapped here falls to WSRV_ERR_INTERNAL, and the test
/// walks every code so that is a decision, never an accident.
/// Thread-affinity: pure, any-thread.
[[nodiscard]] wsrv_status to_c_status(ErrorCode code) noexcept;

}  // namespace wsrv

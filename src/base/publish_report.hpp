// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>

namespace wsrv {

/// What one publish did. `accepted + refused == subscribers`. Accepted means "the subscriber's
/// loop took it for delivery", the same promise as SessionHandle::send; refused means its loop's
/// task queue was full, it was out of memory, or the server is gone. Lives in base/ so that the
/// broker (server/) and the daemon (apps/wsrvd, which may not include server/) share one type.
struct PublishReport {
    std::size_t subscribers = 0;
    std::size_t accepted = 0;
    std::size_t refused = 0;
};

}  // namespace wsrv

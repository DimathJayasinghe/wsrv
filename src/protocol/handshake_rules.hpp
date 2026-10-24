// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "protocol/handshake.hpp"

#include <memory>
#include <vector>

namespace wsrv {

/// The built-in rule chain, in the order documented on HandshakeNegotiator. Internal to protocol/.
[[nodiscard]] std::vector<std::unique_ptr<IHandshakeRule>>
make_builtin_rules(const HandshakeConfig& config);

}  // namespace wsrv

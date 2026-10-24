// SPDX-License-Identifier: Apache-2.0
#include "base/version.hpp"

namespace wsrv {

std::string_view version() noexcept {
    return WSRV_VERSION;
}

}  // namespace wsrv

// SPDX-License-Identifier: Apache-2.0
#include "base/version.hpp"

namespace wsrv {

// The value is injected by CMake from project(VERSION ...), so there is a single place to change it.
std::string_view version() noexcept {
    return WSRV_VERSION;
}

}  // namespace wsrv

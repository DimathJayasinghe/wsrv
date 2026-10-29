// SPDX-License-Identifier: Apache-2.0
#include "net/fd.hpp"

#include <unistd.h>

namespace wsrv {

Fd::~Fd() {
    reset();
}

void Fd::reset(int raw) noexcept {
    if (raw == raw_) {
        return;
    }
    if (raw_ >= 0) {
        ::close(
            raw_);  // result deliberately ignored: nothing useful can be done, see class comment
    }
    raw_ = raw;
}

}  // namespace wsrv

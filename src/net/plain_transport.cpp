// SPDX-License-Identifier: Apache-2.0
#include "net/plain_transport.hpp"

namespace wsrv {

Result<std::size_t> PlainTransport::read(MutableByteView out) {
    if (!fd_.valid()) {
        return fail(ErrorCode::ConnectionClosed, "read on a closed transport");
    }
    if (out.empty()) {
        return std::size_t{0};  // never ask the kernel for zero bytes: 0 would look like EOF
    }
    const Result<long> n =
        retrying([&] { return sys_->read(fd_.get(), out.data(), out.size()); }, "read");
    if (!n) {
        return std::unexpected{n.error()};
    }
    return static_cast<std::size_t>(*n);
}

Result<std::size_t> PlainTransport::write(ByteView data) {
    if (!fd_.valid()) {
        return fail(ErrorCode::ConnectionClosed, "write on a closed transport");
    }
    if (data.empty()) {
        return std::size_t{0};
    }
    const Result<long> n = retrying(
        [&] { return sys_->send(fd_.get(), data.data(), data.size(), MSG_NOSIGNAL); }, "write");
    if (!n) {
        return std::unexpected{n.error()};
    }
    return static_cast<std::size_t>(*n);
}

void PlainTransport::shutdown_write() noexcept {
    if (fd_.valid()) {
        (void)sys_->shutdown(fd_.get(), SHUT_WR);  // best effort: the peer may already be gone
    }
}

void PlainTransport::close() noexcept {
    fd_.reset();
}

}  // namespace wsrv

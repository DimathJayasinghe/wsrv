// SPDX-License-Identifier: Apache-2.0
#include "net/io_error.hpp"

#include <cerrno>

namespace wsrv {

namespace {

[[nodiscard]] ErrorCode classify(int err) noexcept {
    switch (err) {
    case EAGAIN:
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
        return ErrorCode::WouldBlock;
    case EPIPE:
    case ECONNRESET:
    case ECONNABORTED:
    case ETIMEDOUT:
    case ENOTCONN:
    case EHOSTUNREACH:
        return ErrorCode::ConnectionClosed;
    case EMFILE:
    case ENFILE:
    case ENOBUFS:
    case ENOMEM:
        return ErrorCode::FdLimit;
    case EADDRINUSE:
        return ErrorCode::AddressInUse;
    default:
        return ErrorCode::Io;
    }
}

}  // namespace

Error map_errno(int err, std::string_view context) noexcept {
    return Error{classify(err), context};
}

}  // namespace wsrv

// SPDX-License-Identifier: Apache-2.0
#include "capi/c_status.hpp"

namespace wsrv {

wsrv_status to_c_status(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::InvalidArgument:
    case ErrorCode::MessageInProgress:
        return WSRV_ERR_INVALID_ARGUMENT;
    case ErrorCode::InvalidConfig:
        return WSRV_ERR_INVALID_CONFIG;
    case ErrorCode::AddressInUse:
        return WSRV_ERR_ADDRESS_IN_USE;
    case ErrorCode::Closing:  // a connection that is gone, or never was: the C API's "no such conn"
        return WSRV_ERR_NO_SUCH_CONN;
    case ErrorCode::Backpressure:
    case ErrorCode::BufferFull:
        return WSRV_ERR_BACKPRESSURE;
    case ErrorCode::InvalidUtf8:
        return WSRV_ERR_INVALID_UTF8;
    case ErrorCode::WrongProcess:
        return WSRV_ERR_WRONG_PROCESS;
    case ErrorCode::ResourceExhausted:
    case ErrorCode::FdLimit:
        return WSRV_ERR_RESOURCE;
    case ErrorCode::Io:
    case ErrorCode::ConnectionClosed:
    case ErrorCode::WouldBlock:
        return WSRV_ERR_IO;
    case ErrorCode::OutOfRange:
    case ErrorCode::Overflow:
    case ErrorCode::InvalidBase64:
    case ErrorCode::RandomUnavailable:
        return WSRV_ERR_INTERNAL;
    }
    return WSRV_ERR_INTERNAL;  // unreachable for valid enumerators; fail safe
}

}  // namespace wsrv

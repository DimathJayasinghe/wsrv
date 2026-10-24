// SPDX-License-Identifier: Apache-2.0
#include "base/error.hpp"

namespace wsrv {

std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::InvalidArgument:
        return "InvalidArgument";
    case ErrorCode::OutOfRange:
        return "OutOfRange";
    case ErrorCode::Overflow:
        return "Overflow";
    case ErrorCode::BufferFull:
        return "BufferFull";
    case ErrorCode::InvalidBase64:
        return "InvalidBase64";
    case ErrorCode::InvalidUtf8:
        return "InvalidUtf8";
    case ErrorCode::RandomUnavailable:
        return "RandomUnavailable";
    case ErrorCode::ResourceExhausted:
        return "ResourceExhausted";
    }
    return "Unknown";
}

}  // namespace wsrv

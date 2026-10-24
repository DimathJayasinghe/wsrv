// SPDX-License-Identifier: Apache-2.0
#include "protocol/utf8.hpp"

namespace wsrv {

namespace {
constexpr std::uint8_t kContinuationLo = 0x80;
constexpr std::uint8_t kContinuationHi = 0xBF;
}  // namespace

void Utf8Validator::reset() noexcept {
    remaining_ = 0;
    seen_ = 0;
    lo_ = kContinuationLo;
    hi_ = kContinuationHi;
    failed_ = false;
}

/// Classify a lead byte (Unicode Table 3-7). Returns false for bytes that can never start a
/// character: stray continuation bytes 80..BF, overlong leads C0/C1, and F5..FF.
bool Utf8Validator::start_sequence(std::uint8_t lead) noexcept {
    lo_ = kContinuationLo;
    hi_ = kContinuationHi;
    if (lead >= 0xC2 && lead <= 0xDF) {
        remaining_ = 1;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        remaining_ = 2;
        if (lead == 0xE0) {
            lo_ = 0xA0;  // 80..9F would be an overlong 3-byte form
        } else if (lead == 0xED) {
            hi_ = 0x9F;  // A0..BF would encode the surrogates D800..DFFF
        }
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        remaining_ = 3;
        if (lead == 0xF0) {
            lo_ = 0x90;  // 80..8F would be an overlong 4-byte form
        } else if (lead == 0xF4) {
            hi_ = 0x8F;  // 90..BF would exceed U+10FFFF
        }
    } else {
        return false;
    }
    seen_ = 1;
    return true;
}

bool Utf8Validator::step(std::uint8_t byte) noexcept {
    if (remaining_ == 0) {
        if (byte < 0x80) {
            return true;  // ASCII
        }
        return start_sequence(byte);
    }
    if (byte < lo_ || byte > hi_) {
        return false;
    }
    --remaining_;
    ++seen_;
    lo_ = kContinuationLo;  // only the second byte may have a special range
    hi_ = kContinuationHi;
    if (remaining_ == 0) {
        seen_ = 0;
    }
    return true;
}

Result<void> Utf8Validator::feed(ByteView chunk) noexcept {
    if (failed_) {
        return fail(ErrorCode::InvalidUtf8, "stream already invalid");
    }
    for (const std::byte b : chunk) {
        if (!step(std::to_integer<std::uint8_t>(b))) {
            failed_ = true;
            return fail(ErrorCode::InvalidUtf8, "invalid UTF-8 byte");
        }
    }
    return {};
}

bool is_valid_utf8(ByteView data) noexcept {
    Utf8Validator validator;
    return validator.feed(data).has_value() && validator.at_boundary();
}

}  // namespace wsrv

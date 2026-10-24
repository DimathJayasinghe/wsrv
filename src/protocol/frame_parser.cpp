// SPDX-License-Identifier: Apache-2.0
#include "protocol/frame_parser.hpp"

#include <algorithm>

namespace wsrv {

namespace {

constexpr unsigned kLength16Marker = 126;
constexpr unsigned kLength64Marker = 127;
constexpr std::uint64_t kMinLength16 = 126;    // shortest value the 16-bit form may carry
constexpr std::uint64_t kMinLength64 = 65536;  // shortest value the 64-bit form may carry
constexpr std::uint64_t kTopBit = 0x8000000000000000ULL;

/// Bytes of extended length that follow the 7-bit length field.
[[nodiscard]] constexpr std::size_t extended_length_bytes(unsigned length_code) noexcept {
    if (length_code == kLength16Marker) {
        return 2;
    }
    return length_code == kLength64Marker ? 8 : 0;
}

}  // namespace

std::unexpected<ProtocolFailure> FrameParser::fail_with(CloseCode code, std::string_view reason) {
    state_ = State::Failed;
    failure_ = ProtocolFailure{code, reason};
    return protocol_failure(code, reason);
}

void FrameParser::reset() noexcept {
    state_ = State::Header;
    have_ = 0;
    need_ = 2;
    header_ = FrameHeader{};
    remaining_ = 0;
    masker_.reset();
}

/// Reserved bits, mask expectation and the control-frame rules: all decidable from the first two
/// bytes.
FrameResult<void> FrameParser::validate_flags() {
    if (header_.rsv2 || header_.rsv3) {
        return fail_with(CloseCode::ProtocolError, "reserved bit set");
    }
    const bool rsv1_context_ok =
        config_.allow_rsv1 && !is_control(header_.opcode) && header_.opcode != Opcode::Continuation;
    if (header_.rsv1 && !rsv1_context_ok) {
        return fail_with(CloseCode::ProtocolError, "RSV1 not allowed here");
    }
    if (header_.masked && !config_.expect_masked) {
        return fail_with(CloseCode::ProtocolError, "server frame is masked");
    }
    if (!header_.masked && config_.expect_masked) {
        return fail_with(CloseCode::ProtocolError, "client frame is not masked");
    }
    if (is_control(header_.opcode) && !header_.fin) {
        return fail_with(CloseCode::ProtocolError, "fragmented control frame");
    }
    if (is_control(header_.opcode) && length_code_ > kMaxControlPayload) {
        return fail_with(CloseCode::ProtocolError, "control frame payload too long");
    }
    return {};
}

/// Everything that can be decided from the first two bytes alone, in the order of RFC 6455
/// section 5.2.
FrameResult<void> FrameParser::parse_first_two_bytes() {
    const auto b0 = std::to_integer<unsigned>(buffer_[0]);
    const auto b1 = std::to_integer<unsigned>(buffer_[1]);
    const std::optional<Opcode> opcode = opcode_from_wire(static_cast<std::uint8_t>(b0 & 0x0FU));
    if (!opcode) {
        return fail_with(CloseCode::ProtocolError, "reserved opcode");
    }
    header_ = FrameHeader{};
    header_.fin = (b0 & 0x80U) != 0;
    header_.rsv1 = (b0 & 0x40U) != 0;
    header_.rsv2 = (b0 & 0x20U) != 0;
    header_.rsv3 = (b0 & 0x10U) != 0;
    header_.opcode = *opcode;
    header_.masked = (b1 & 0x80U) != 0;
    length_code_ = b1 & 0x7FU;

    if (const FrameResult<void> ok = validate_flags(); !ok) {
        return ok;
    }
    need_ = 2 + extended_length_bytes(length_code_) + (header_.masked ? 4 : 0);
    return {};
}

/// Extended length, mask key and the checks that need the full header.
FrameResult<void> FrameParser::parse_rest_of_header() {
    std::size_t pos = 2;
    if (length_code_ < kLength16Marker) {
        header_.payload_length = length_code_;
    } else if (length_code_ == kLength16Marker) {
        header_.payload_length = load_be16(ByteView{buffer_}.subspan(pos));
        pos += 2;
        if (header_.payload_length < kMinLength16) {
            return fail_with(CloseCode::ProtocolError, "non-minimal 16-bit length");
        }
    } else {
        header_.payload_length = load_be64(ByteView{buffer_}.subspan(pos));
        pos += 8;
        if ((header_.payload_length & kTopBit) != 0) {
            return fail_with(CloseCode::ProtocolError, "64-bit length has its top bit set");
        }
        if (header_.payload_length < kMinLength64) {
            return fail_with(CloseCode::ProtocolError, "non-minimal 64-bit length");
        }
    }
    if (header_.masked) {
        std::copy_n(buffer_.begin() + static_cast<std::ptrdiff_t>(pos),
                    header_.mask_key.size(),
                    header_.mask_key.begin());
    }
    if (header_.payload_length > config_.max_frame_payload) {
        return fail_with(CloseCode::MessageTooBig, "frame payload exceeds the limit");
    }
    return {};
}

FrameResult<std::optional<FrameEvent>> FrameParser::read_header(MutableByteView& input) {
    while (have_ < need_) {
        if (input.empty()) {
            return std::optional<FrameEvent>{};  // need more bytes
        }
        const std::size_t take = std::min(need_ - have_, input.size());
        std::copy_n(input.begin(), take, buffer_.begin() + static_cast<std::ptrdiff_t>(have_));
        have_ += take;
        input = input.subspan(take);
        if (have_ == 2 && need_ == 2) {  // exactly once per frame: the first two bytes just arrived
            if (const FrameResult<void> ok = parse_first_two_bytes(); !ok) {
                return std::unexpected{ok.error()};
            }
        }
    }
    if (const FrameResult<void> ok = parse_rest_of_header(); !ok) {
        return std::unexpected{ok.error()};
    }
    remaining_ = header_.payload_length;
    masker_.reset();
    if (header_.masked) {
        masker_.emplace(header_.mask_key);
    }
    state_ = remaining_ == 0 ? State::End : State::Payload;
    return std::optional<FrameEvent>{FrameEvent{FrameEvent::Kind::Header, header_, {}}};
}

FrameResult<std::optional<FrameEvent>> FrameParser::next(MutableByteView& input) {
    switch (state_) {
    case State::Failed:
        return protocol_failure(failure_.code, failure_.reason);
    case State::Header:
        return read_header(input);
    case State::Payload: {
        if (input.empty()) {
            return std::optional<FrameEvent>{};
        }
        const auto take =
            static_cast<std::size_t>(std::min<std::uint64_t>(input.size(), remaining_));
        const MutableByteView chunk = input.first(take);
        if (masker_) {
            masker_->apply(chunk);
        }
        input = input.subspan(take);
        remaining_ -= take;
        if (remaining_ == 0) {
            state_ = State::End;
        }
        return std::optional<FrameEvent>{FrameEvent{FrameEvent::Kind::Payload, header_, chunk}};
    }
    case State::End:
        state_ = State::Header;
        have_ = 0;
        need_ = 2;
        return std::optional<FrameEvent>{FrameEvent{FrameEvent::Kind::End, header_, {}}};
    }
    return protocol_failure(CloseCode::InternalError, "invalid parser state");
}

}  // namespace wsrv

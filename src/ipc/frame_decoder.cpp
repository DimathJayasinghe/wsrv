// SPDX-License-Identifier: Apache-2.0
#include "ipc/frame_decoder.hpp"

#include <algorithm>

namespace wsrv::ipc {

FrameDecoder::FrameDecoder(std::size_t max_frame) noexcept
    : max_frame_{std::min(max_frame, kMaxFrameCeiling)}, gathered_{max_frame_} {}

std::unexpected<Error> FrameDecoder::stop(Error error) noexcept {
    failure_ = error;
    return std::unexpected{error};
}

Result<void> FrameDecoder::judge_header() {
    const std::size_t length = load_be32(ByteView{header_});
    if (length < 2) {
        return fail(ErrorCode::InvalidFrame, "ipc: frame length below 2");
    }
    // The limit is checked on the declared length, before any body byte is read or reserved.
    if (length > max_frame_) {
        return fail(ErrorCode::BufferFull, "ipc: frame above the limit");
    }
    const auto type = frame_type_from(std::to_integer<std::uint8_t>(header_[4]));
    if (!type) {
        return fail(ErrorCode::InvalidFrame, "ipc: unknown frame type");
    }
    if (std::to_integer<std::uint8_t>(header_[5]) != 0) {
        return fail(ErrorCode::InvalidFrame, "ipc: reserved flags set");
    }
    type_ = *type;
    body_size_ = length - 2;
    return {};
}

Result<std::optional<Message>> FrameDecoder::finish(ByteView body) {
    Result<Message> message = parse_message(type_, body);
    if (!message) {
        return stop(message.error());
    }
    return std::optional<Message>{*message};
}

Result<std::optional<Message>> FrameDecoder::next(ByteView& input) {
    if (failure_) {
        return std::unexpected{*failure_};
    }
    if (gathered_delivered_) {  // the previous message pointed into gathered_; its time is over
        gathered_.clear();
        gathered_delivered_ = false;
    }
    if (!in_body_) {
        const std::size_t take = std::min(kFrameHeaderSize - have_, input.size());
        std::copy_n(input.begin(), take, header_.begin() + static_cast<std::ptrdiff_t>(have_));
        input = input.subspan(take);
        have_ += take;
        if (have_ < kFrameHeaderSize) {
            return std::optional<Message>{};
        }
        have_ = 0;
        if (const Result<void> judged = judge_header(); !judged) {
            return stop(judged.error());
        }
        in_body_ = true;
    }
    if (gathered_.size() == 0 && input.size() >= body_size_) {  // whole body at hand: no copy
        const ByteView body = input.first(body_size_);
        input = input.subspan(body_size_);
        in_body_ = false;
        return finish(body);
    }
    const std::size_t take = std::min(body_size_ - gathered_.size(), input.size());
    if (take > 0) {
        if (const Result<void> added = gathered_.append(input.first(take)); !added) {
            return stop(added.error());
        }
        input = input.subspan(take);
    }
    if (gathered_.size() < body_size_) {
        return std::optional<Message>{};
    }
    in_body_ = false;
    gathered_delivered_ = true;
    return finish(gathered_.view());
}

}  // namespace wsrv::ipc

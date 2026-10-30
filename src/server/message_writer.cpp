// SPDX-License-Identifier: Apache-2.0
#include "server/message_writer.hpp"

#include <utility>

namespace wsrv {

MessageWriter::MessageWriter(MessageWriter&& other) noexcept
    : link_{std::move(other.link_)}, finished_{other.finished_} {
    other.link_.reset();  // a moved-from writer owns nothing and closes nothing
}

MessageWriter& MessageWriter::operator=(MessageWriter&& other) noexcept {
    if (this != &other) {
        if (open()) {
            sink()->stream_abandon();  // the message we are replacing can no longer be finished
        }
        link_ = std::move(other.link_);
        finished_ = other.finished_;
        other.link_.reset();
    }
    return *this;
}

MessageWriter::~MessageWriter() {
    if (open()) {
        sink()->stream_abandon();
    }
}

bool MessageWriter::open() const noexcept {
    return !finished_ && sink() != nullptr;
}

Result<void> MessageWriter::write(ByteView bytes) {
    if (finished_) {
        return fail(ErrorCode::InvalidArgument, "writer: message already finished");
    }
    IMessageSink* const target = sink();
    if (target == nullptr) {
        return fail(ErrorCode::Closing, "writer: connection gone");
    }
    return target->stream_write(bytes);
}

Result<void> MessageWriter::finish() {
    if (finished_) {
        return fail(ErrorCode::InvalidArgument, "writer: message already finished");
    }
    IMessageSink* const target = sink();
    if (target == nullptr) {
        return fail(ErrorCode::Closing, "writer: connection gone");
    }
    const Result<void> ended = target->stream_finish();
    finished_ = ended.has_value();
    return ended;
}

}  // namespace wsrv

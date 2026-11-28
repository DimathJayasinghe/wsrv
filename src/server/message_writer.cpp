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
        abandon_if_open();  // the message we are replacing can no longer be finished
        link_ = std::move(other.link_);
        finished_ = other.finished_;
        other.link_.reset();
    }
    return *this;
}

MessageWriter::~MessageWriter() {
    abandon_if_open();
}

void MessageWriter::abandon_if_open() noexcept {
    // The sink is read once and the pointer that was tested is the one used: asking sink() twice
    // (as open() followed by sink()->... did) is a second read that GCC 15 cannot prove is the
    // same non-null pointer (-Wnull-dereference).
    if (finished_) {
        return;
    }
    if (IMessageSink* const target = sink()) {
        target->stream_abandon();
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

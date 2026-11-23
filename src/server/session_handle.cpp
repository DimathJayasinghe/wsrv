// SPDX-License-Identifier: Apache-2.0
#include "server/session_handle.hpp"

#include "protocol/utf8.hpp"

#include <new>
#include <string>
#include <utility>
#include <vector>

namespace wsrv {

LoopLink::LoopLink(EventLoop& loop,
                   ISessionDirectory& directory,
                   std::size_t max_pending_send_bytes)
    : loop_{&loop}, directory_{&directory},
      pending_{std::make_shared<PendingBytes>(max_pending_send_bytes)} {}

Result<void> LoopLink::submit(ConnectionId id, SessionCommand command, std::size_t payload_bytes) {
    return submit_in(Lane::Control, id, std::move(command), payload_bytes);
}

Result<void> LoopLink::submit_bulk(ConnectionId id, SessionCommand command) {
    return submit_in(Lane::Bulk, id, std::move(command), 0);
}

Result<std::shared_ptr<const SendLease>> LoopLink::reserve(std::size_t payload_bytes) {
    if (payload_bytes == 0) {
        return std::shared_ptr<const SendLease>{};
    }
    if (payload_bytes > pending_->limit()) {
        return fail(ErrorCode::InvalidArgument,
                    "session handle: the message is larger than the whole pending-send budget");
    }
    if (!pending_->try_add(payload_bytes)) {
        return fail(ErrorCode::BufferFull,
                    "session handle: the pending-send byte budget is used up");
    }
    try {
        return std::make_shared<const SendLease>(pending_, payload_bytes);
    } catch (const std::bad_alloc&) {
        pending_->release(payload_bytes);
        return fail(ErrorCode::ResourceExhausted, "session handle: out of memory");
    }
}

Result<void>
LoopLink::submit_in(Lane lane, ConnectionId id, SessionCommand command, std::size_t payload_bytes) {
    const std::lock_guard lock{mutex_};
    if (loop_ == nullptr) {
        return fail(ErrorCode::Closing, "session handle: the server is gone");
    }
    ISessionDirectory* const directory = directory_;
    try {
        Result<std::shared_ptr<const SendLease>> lease = reserve(payload_bytes);
        if (!lease) {
            return std::unexpected{lease.error()};
        }
        // The lease lives in the task: the bytes are given back when the task has run, or is
        // destroyed without running (a refused post, a loop that goes away).
        auto task = [directory, id, command = std::move(command), lease = std::move(*lease)] {
            directory->run(id, command);
        };
        return lane == Lane::Bulk ? loop_->post_bulk(std::move(task))
                                  : loop_->post(std::move(task));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "session handle: out of memory");
    }
}

Result<void> LoopLink::submit_decision(ConnectionId id, HandshakeVerdict verdict) {
    const std::lock_guard lock{mutex_};
    if (loop_ == nullptr) {
        return fail(ErrorCode::Closing, "handshake: the server is gone");
    }
    ISessionDirectory* const directory = directory_;
    try {
        return loop_->post(
            [directory, id, verdict = std::move(verdict)] { directory->decide(id, verdict); });
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "handshake: out of memory");
    }
}

void LoopLink::close() noexcept {
    const std::lock_guard lock{mutex_};
    loop_ = nullptr;
    directory_ = nullptr;
}

namespace {

[[nodiscard]] Result<void> submit_to(const std::shared_ptr<LoopLink>& link,
                                     ConnectionId id,
                                     SessionCommand command,
                                     std::size_t payload_bytes = 0) {
    if (link == nullptr) {
        return fail(ErrorCode::Closing, "session handle: not connected");
    }
    return link->submit(id, std::move(command), payload_bytes);
}

}  // namespace

Result<void> SessionHandle::send_text(std::string_view text) const {
    // Checked here, on the caller's thread: a bad string found later, on the loop, could only be
    // dropped, and the caller would never learn why (FR-F7).
    Utf8Validator utf8;
    if (!utf8.feed(as_bytes(text)) || !utf8.at_boundary()) {
        return fail(ErrorCode::InvalidUtf8, "session handle: text is not valid UTF-8");
    }
    try {
        return submit_to(
            link_,
            id_,
            [copy = std::string{text}](Session& session) {
                static_cast<void>(session.send_text(copy).has_value());  // policy applies inside
            },
            text.size());
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "session handle: out of memory");
    }
}

Result<SharedMessage> SharedMessage::text(std::string_view text) {
    Utf8Validator utf8;
    if (!utf8.feed(as_bytes(text)) || !utf8.at_boundary()) {
        return fail(ErrorCode::InvalidUtf8, "shared message: text is not valid UTF-8");
    }
    try {
        const ByteView raw = as_bytes(text);
        return SharedMessage{std::make_shared<const std::vector<std::byte>>(raw.begin(), raw.end()),
                             true};
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "shared message: out of memory");
    }
}

Result<SharedMessage> SharedMessage::binary(ByteView data) {
    try {
        return SharedMessage{
            std::make_shared<const std::vector<std::byte>>(data.begin(), data.end()), false};
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "shared message: out of memory");
    }
}

Result<void> SessionHandle::send(const SharedMessage& message) const {
    if (link_ == nullptr) {
        return fail(ErrorCode::Closing, "session handle: not connected");
    }
    try {
        // The bulk lane: a broadcast is refused before it can fill the loop's queue (ADR-0024 A1).
        return link_->submit_bulk(id_, [message](Session& session) {
            const ByteView bytes = message.data();
            const Result<void> queued =
                message.is_text() ? session.send_text(as_chars(bytes)) : session.send_binary(bytes);
            static_cast<void>(queued.has_value());  // the slow-consumer policy applies inside
        });
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "session handle: out of memory");
    }
}

Result<void> SessionHandle::send_binary(ByteView data) const {
    try {
        return submit_to(
            link_,
            id_,
            [copy = std::vector<std::byte>(data.begin(), data.end())](Session& session) {
                static_cast<void>(session.send_binary(ByteView{copy}).has_value());
            },
            data.size());
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "session handle: out of memory");
    }
}

Result<void> SessionHandle::resume_reads() const {
    return submit_to(link_, id_, [](Session& session) { session.resume_reads(); });
}

Result<void> SessionHandle::close(CloseCode code, std::string_view reason) const {
    // Checked here for the same reason as the text of send_text: on the loop an unsendable code or
    // a reason that is not UTF-8 would only be turned into a 1011 close, and the caller would never
    // learn what it did wrong (RFC 6455 sections 5.5.1 and 7.4).
    if (!is_valid_on_wire(static_cast<std::uint16_t>(code))) {
        return fail(ErrorCode::InvalidArgument, "session handle: close code cannot be sent");
    }
    Utf8Validator utf8;
    if (!utf8.feed(as_bytes(reason)) || !utf8.at_boundary()) {
        return fail(ErrorCode::InvalidUtf8, "session handle: close reason is not valid UTF-8");
    }
    try {
        return submit_to(link_, id_, [code, copy = std::string{reason}](Session& session) {
            session.close(code, copy);
        });
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "session handle: out of memory");
    }
}

}  // namespace wsrv

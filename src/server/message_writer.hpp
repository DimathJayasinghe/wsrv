// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/bytes.hpp"
#include "base/error.hpp"

#include <memory>
#include <string_view>
#include <utility>

namespace wsrv {

/// What a MessageWriter needs from the connection that owns its message (Dependency Inversion:
/// the writer is a public type, the connection is not). Implemented by Connection.
/// Thread-affinity: loop-thread-only.
class IMessageSink {
public:
    IMessageSink() = default;
    IMessageSink(const IMessageSink&) = delete;
    IMessageSink& operator=(const IMessageSink&) = delete;
    IMessageSink(IMessageSink&&) = delete;
    IMessageSink& operator=(IMessageSink&&) = delete;
    virtual ~IMessageSink() = default;

    [[nodiscard]] virtual Result<void> stream_write(ByteView bytes) = 0;
    [[nodiscard]] virtual Result<void> stream_finish() = 0;
    /// The writer was destroyed with its message still open: the peer would see a message that
    /// never ends, so the connection is closed (ADR-0015 section 3). Must not throw.
    virtual void stream_abandon() noexcept = 0;
};

/// The one pointer from writers to their connection. The connection clears `sink` when it
/// finishes or is destroyed, so a writer that outlives it finds nothing and fails with Closing
/// instead of touching freed memory. Both sides live on the loop thread: no atomics needed.
struct StreamLink {
    IMessageSink* sink = nullptr;
};

/// Sends one message piece by piece without holding it in memory (FR-F12, ADR-0015).
/// Obtain one from Session::begin_text() / begin_binary().
///
/// The protocol is cooperative: write() until it returns Backpressure (nothing was queued), return
/// from the callback, write again from IWebSocketHandler::on_writable, and call finish() when
/// done. A writer destroyed before finish() closes the connection with 1011: a message cannot be
/// taken back once its first fragment is on the wire.
///
/// Thread-affinity: loop-thread-only, like the Session it came from. May be kept between
/// callbacks; after its connection ended every call returns Error{Closing}.
class MessageWriter {
public:
    explicit MessageWriter(std::shared_ptr<StreamLink> link) noexcept : link_{std::move(link)} {}
    MessageWriter(const MessageWriter&) = delete;
    MessageWriter& operator=(const MessageWriter&) = delete;
    MessageWriter(MessageWriter&& other) noexcept;
    MessageWriter& operator=(MessageWriter&& other) noexcept;
    ~MessageWriter();

    /// Append bytes to the message. For a text message they are checked as UTF-8 across calls.
    /// @return Error{Backpressure} (nothing queued; wait for on_writable), Error{BufferFull} (the
    /// piece can never fit: send smaller pieces), Error{InvalidUtf8} (nothing queued; the writer
    /// stays usable), Error{Closing} (connection ending or gone), Error{InvalidArgument}
    /// (already finished).
    [[nodiscard]] Result<void> write(ByteView bytes);
    [[nodiscard]] Result<void> write_text(std::string_view text) {
        return write(as_bytes(text));
    }
    /// End the message. @return Error{InvalidUtf8} if a text message would end inside a character
    /// (the writer stays open), Error{Closing}, Error{InvalidArgument} (already finished).
    [[nodiscard]] Result<void> finish();

    /// True until finish() succeeded, the writer was moved from, or the connection ended.
    [[nodiscard]] bool open() const noexcept;

private:
    [[nodiscard]] IMessageSink* sink() const noexcept {
        return link_ != nullptr ? link_->sink : nullptr;
    }

    std::shared_ptr<StreamLink> link_;
    bool finished_ = false;
};

}  // namespace wsrv

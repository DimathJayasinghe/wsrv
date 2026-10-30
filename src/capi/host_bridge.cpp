// SPDX-License-Identifier: Apache-2.0
#include "capi/host_bridge.hpp"

#include "net/endpoint.hpp"

#include <algorithm>
#include <new>
#include <string>
#include <utility>

namespace wsrv {

namespace {

HostEvent make_event(HostEventType type, ConnectionId conn, std::string_view text = {}) {
    HostEvent event;
    event.type = type;
    event.conn = conn;
    const ByteView bytes = as_bytes(text);
    event.data.assign(bytes.begin(), bytes.end());
    return event;
}

constexpr std::string_view kNoSuchConnection = "capi: unknown or finished connection";

}  // namespace

/// One per connection, on its loop thread: copies what the connection does into the queue.
class HostBridge::Handler final : public IWebSocketHandler {
public:
    explicit Handler(HostBridge& bridge) noexcept : bridge_{bridge} {}

    void on_open(Session& session) override {
        // Built and registered before the push: once the host sees the Open it may send at once.
        HostEvent open =
            make_event(HostEventType::Open, session.id(), format_endpoint(session.peer()));
        bridge_.add(session.id(), session.handle());
        if (!push(session, std::move(open))) {
            bridge_.remove(
                session.id());  // never announced, so never closed: the host sees nothing
            return;
        }
        announced_ = true;
    }

    void on_text(Session& session, std::string_view text) override {
        deliver(session, make_event(HostEventType::Text, session.id(), text));
    }

    void on_binary(Session& session, ByteView data) override {
        HostEvent event = make_event(HostEventType::Binary, session.id());
        event.data.assign(data.begin(), data.end());
        deliver(session, std::move(event));
    }

    void on_error(Session& session, const Error& error) override {
        // Best effort: an Error carries no state the host must see, so a full queue just drops it.
        const Result<PushOutcome> pushed = bridge_.events().push(
            make_event(HostEventType::Error, session.id(), to_string(error.code)));
        static_cast<void>(pushed.has_value());
    }

    void on_close(Session& session, const CloseInfo& info) override {
        if (!announced_) {
            return;  // the host never heard of this connection
        }
        bridge_.remove(session.id());  // first: from now on a send by id fails with Closing
        HostEvent close = make_event(HostEventType::Close,
                                     session.id(),
                                     std::string_view{info.reason}.substr(0, kMaxCloseReason));
        close.close_code = static_cast<std::uint16_t>(info.code);
        close.clean = info.clean;
        // A Close always fits because its Open reserved the room; the result can only be an
        // error if that invariant is broken, and there is nobody left to tell.
        static_cast<void>(bridge_.events().push(std::move(close)).has_value());
    }

private:
    void deliver(Session& session, HostEvent event) {
        if (announced_ && !overloaded_) {
            static_cast<void>(push(session, std::move(event)));
        }
    }

    /// @return false after the queue refused the event: the host is too slow, so the connection is
    /// closed with 1013 (ADR-0016 section 4) and nothing more is queued for it but its Close.
    bool push(Session& session, HostEvent event) {
        const Result<PushOutcome> pushed = bridge_.events().push(std::move(event));
        if (pushed) {
            return true;  // pushed->pause_reads is acted on once Session can pause reads
        }
        overloaded_ = true;
        session.close(CloseCode::TryAgainLater, "host is not keeping up");
        return false;
    }

    HostBridge& bridge_;
    bool announced_ = false;   // the host was told about this connection
    bool overloaded_ = false;  // the queue refused an event; the connection is being closed
};

Result<std::unique_ptr<HostBridge>> HostBridge::make(ISyscalls& sys, EventQueueConfig queue) {
    auto events = EventQueue::create(sys, queue);
    if (!events) {
        return std::unexpected{events.error()};
    }
    try {
        return std::make_unique<HostBridge>(Key{}, std::move(*events));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "capi: out of memory");
    }
}

std::unique_ptr<IWebSocketHandler> HostBridge::create(const ConnectionInfo& /*info*/) {
    try {
        return std::make_unique<Handler>(*this);
    } catch (const std::bad_alloc&) {
        return nullptr;  // the factory contract: a null handler closes the connection with 1011
    }
}

void HostBridge::add(ConnectionId id, SessionHandle handle) {
    const std::lock_guard lock{mutex_};
    handles_.insert_or_assign(id, std::move(handle));
}

void HostBridge::remove(ConnectionId id) {
    const std::lock_guard lock{mutex_};
    handles_.erase(id);
}

std::optional<SessionHandle> HostBridge::find(ConnectionId id) {
    const std::lock_guard lock{mutex_};
    const auto it = handles_.find(id);
    if (it == handles_.end()) {
        return std::nullopt;
    }
    return it->second;
}

Result<void> HostBridge::send_text(ConnectionId id, std::string_view text) {
    const std::optional<SessionHandle> handle = find(id);
    return handle ? handle->send_text(text) : fail(ErrorCode::Closing, kNoSuchConnection);
}

Result<void> HostBridge::send_binary(ConnectionId id, ByteView data) {
    const std::optional<SessionHandle> handle = find(id);
    return handle ? handle->send_binary(data) : fail(ErrorCode::Closing, kNoSuchConnection);
}

Result<void> HostBridge::close(ConnectionId id, CloseCode code, std::string_view reason) {
    const std::optional<SessionHandle> handle = find(id);
    return handle ? handle->close(code, reason) : fail(ErrorCode::Closing, kNoSuchConnection);
}

}  // namespace wsrv

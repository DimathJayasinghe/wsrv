// SPDX-License-Identifier: Apache-2.0
#include "capi/host_bridge.hpp"

#include "net/endpoint.hpp"

#include <algorithm>
#include <new>
#include <string>
#include <utility>

#include "capi/handshake_blob.hpp"

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
constexpr int kStatusUnavailable = 503;
constexpr std::string_view kBrokerOff = "capi: the broker is not enabled";
constexpr std::string_view kNoSuchHandshake = "capi: no upgrade is waiting for that id";

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
            if (pushed->pause_reads) {
                pause_until_drained(session);
            }
            return true;
        }
        overloaded_ = true;
        session.close(CloseCode::TryAgainLater, "host is not keeping up");
        return false;
    }

    /// Stop reading until the host has drained the queue. The order matters: the connection is
    /// registered as parked first, and only then is the queue asked whether it is still full. A
    /// host that drained it in between has already looked at an empty parked set and will not
    /// resume us, so we resume ourselves (otherwise this connection would stay paused forever).
    void pause_until_drained(Session& session) {
        session.pause_reads();
        bridge_.park(session.id());
        if (!bridge_.events().pressured() && bridge_.unpark(session.id())) {
            session.resume_reads();
        }
    }

    HostBridge& bridge_;
    bool announced_ = false;   // the host was told about this connection
    bool overloaded_ = false;  // the queue refused an event; the connection is being closed
};

Result<std::unique_ptr<HostBridge>>
HostBridge::make(ISyscalls& sys, EventQueueConfig queue, std::shared_ptr<Broker> broker) {
    auto events = EventQueue::create(sys, queue);
    if (!events) {
        return std::unexpected{events.error()};
    }
    try {
        return std::make_unique<HostBridge>(Key{}, std::move(*events), std::move(broker));
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
    parked_.erase(id);
    if (broker_) {
        broker_->unsubscribe_all(
            id);  // under the bridge mutex: see subscribe() (ADR-0027 section 4)
    }
}

void HostBridge::park(ConnectionId id) {
    const std::lock_guard lock{mutex_};
    parked_.insert(id);
}

/// @return true if `id` was parked (so the caller, not resume_parked(), must resume it).
bool HostBridge::unpark(ConnectionId id) {
    const std::lock_guard lock{mutex_};
    return parked_.erase(id) > 0;
}

/// Resume every parked connection. Their handles are copied out first so that no call into a
/// loop happens under the mutex; a connection that ended meanwhile is simply not found.
///
/// A resume the loop refuses (its task queue is full: BufferFull, or out of memory) is not lost:
/// the connection goes back to the parked set and the next poll() tries again. Forgetting it would
/// leave the connection read-paused until the idle or pong timer ended it. Only Closing means "it
/// is gone, nothing to resume".
void HostBridge::resume_parked() {
    std::vector<SessionHandle> to_resume;
    {
        const std::lock_guard lock{mutex_};
        for (const ConnectionId id : parked_) {
            if (const auto it = handles_.find(id); it != handles_.end()) {
                to_resume.push_back(it->second);
            }
        }
        parked_.clear();
    }
    std::vector<ConnectionId> refused;
    for (const SessionHandle& handle : to_resume) {
        const Result<void> resumed = handle.resume_reads();
        if (!resumed && resumed.error().code != ErrorCode::Closing) {
            refused.push_back(handle.id());
        }
    }
    if (!refused.empty()) {
        const std::lock_guard lock{mutex_};
        for (const ConnectionId id : refused) {
            if (handles_.contains(id)) {  // it may have ended meanwhile (remove() erased it)
                parked_.insert(id);
            }
        }
    }
}

Result<void> HostBridge::request(HandshakeRequest request, PendingHandle handle) {
    HostEvent event;
    try {
        event.type = HostEventType::Handshake;
        event.conn = request.id;
        event.data = encode_handshake(format_endpoint(request.peer), request);
        const std::lock_guard lock{mutex_};
        if (!intake_open_) {
            return fail(ErrorCode::Closing, "capi: no host is listening for handshakes");
        }
        asked_.insert_or_assign(request.id, std::move(handle));  // before the push: see on_open
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "capi: out of memory");
    }
    const ConnectionId id = event.conn;
    if (const Result<PushOutcome> pushed = events_->push(std::move(event)); !pushed) {
        forget_asked(id);  // the host will never hear of it: fail closed (the server refuses)
        return std::unexpected{pushed.error()};
    }
    return {};
}

void HostBridge::set_handshake_intake(bool open) {
    std::unordered_map<ConnectionId, PendingHandle> refused;
    {
        const std::lock_guard lock{mutex_};
        intake_open_ = open;
        if (!open) {
            refused.swap(asked_);  // under the same lock as request(): nothing slips in after
        }
    }
    for (const auto& [id, handle] : refused) {
        // 503: the application that was asked is gone. A failure means the server already ended
        // the wait itself, which is just as final.
        static_cast<void>(handle.reject(kStatusUnavailable).has_value());
    }
}

void HostBridge::withdrawn(ConnectionId id) noexcept {
    forget_asked(id);
}

std::size_t HostBridge::waiting_handshakes() {
    const std::lock_guard lock{mutex_};
    return asked_.size();
}

void HostBridge::forget_asked(ConnectionId id) {
    const std::lock_guard lock{mutex_};
    asked_.erase(id);
}

std::optional<PendingHandle> HostBridge::find_asked(ConnectionId id) {
    const std::lock_guard lock{mutex_};
    const auto it = asked_.find(id);
    if (it == asked_.end()) {
        return std::nullopt;
    }
    return it->second;
}

Result<void> HostBridge::accept_handshake(ConnectionId id,
                                          std::optional<std::string_view> subprotocol) {
    const std::optional<PendingHandle> handle = find_asked(id);
    if (!handle) {
        return fail(ErrorCode::Closing, kNoSuchHandshake);
    }
    Result<void> answered = handle->accept(subprotocol);
    if (answered) {
        forget_asked(id);  // consumed: the next answer for this id is "no such"
    }
    return answered;
}

Result<void> HostBridge::reject_handshake(ConnectionId id, int status) {
    const std::optional<PendingHandle> handle = find_asked(id);
    if (!handle) {
        return fail(ErrorCode::Closing, kNoSuchHandshake);
    }
    Result<void> answered = handle->reject(status);
    if (answered) {
        forget_asked(id);
    }
    return answered;
}

std::optional<HostEvent> HostBridge::poll() {
    std::optional<Popped> popped = events_->try_pop();
    if (!popped) {
        // Nothing to hand out. If the queue is no longer under pressure, nobody may stay parked:
        // a safety net for any path on which the resume signal was not delivered.
        if (!events_->pressured()) {
            resume_parked();
        }
        return std::nullopt;
    }
    if (popped->resume_reads) {
        resume_parked();
    }
    return std::move(popped->event);
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

Result<void> HostBridge::subscribe(ConnectionId id, std::string_view topic) {
    if (!broker_) {
        return fail(ErrorCode::InvalidArgument, kBrokerOff);
    }
    const std::lock_guard lock{mutex_};  // held across the broker call, on purpose (header)
    const auto it = handles_.find(id);
    if (it == handles_.end()) {
        return fail(ErrorCode::Closing, kNoSuchConnection);
    }
    return broker_->subscribe(it->second, topic);
}

Result<void> HostBridge::unsubscribe(ConnectionId id, std::string_view topic) {
    if (!broker_) {
        return fail(ErrorCode::InvalidArgument, kBrokerOff);
    }
    if (!find(id)) {
        return fail(ErrorCode::Closing, kNoSuchConnection);
    }
    broker_->unsubscribe(id, topic);
    return {};
}

Result<PublishReport>
HostBridge::publish_text(std::string_view topic, std::string_view text, ConnectionId except) {
    if (!broker_) {
        return fail(ErrorCode::InvalidArgument, kBrokerOff);
    }
    return broker_->publish_text(topic, text, except);
}

Result<PublishReport>
HostBridge::publish_binary(std::string_view topic, ByteView data, ConnectionId except) {
    if (!broker_) {
        return fail(ErrorCode::InvalidArgument, kBrokerOff);
    }
    return broker_->publish_binary(topic, data, except);
}

std::size_t HostBridge::topic_subscribers(std::string_view topic) const {
    return broker_ ? broker_->subscriber_count(topic) : 0;
}

}  // namespace wsrv

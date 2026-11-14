// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/bytes.hpp"
#include "base/error.hpp"
#include "base/metrics.hpp"
#include "server/session_handle.hpp"

#include <atomic>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace wsrv {

/// Every bound of the broker (ADR-0024 section 2). A subscription or publish over a bound is
/// refused and changes nothing; zero is not "unlimited", a bound of zero refuses everything.
struct BrokerConfig {
    std::size_t max_topics = 1024;
    std::size_t max_subscribers_per_topic = 10'000;
    std::size_t max_topics_per_connection = 64;
    std::size_t max_subscriptions = 100'000;  ///< in total: caps a handler that forgets to leave
    std::size_t max_topic_length = 128;
    std::size_t max_payload_bytes = std::size_t{64} * 1024;
};

/// What one publish did. `accepted + refused == subscribers`. Accepted means "the subscriber's
/// loop took it for delivery", the same promise as SessionHandle::send; refused means its loop's
/// task queue was full, it was out of memory, or the server is gone.
struct PublishReport {
    std::size_t subscribers = 0;
    std::size_t accepted = 0;
    std::size_t refused = 0;
};

/// Topic -> subscribers, for any thread (ADR-0024). A room is a topic.
///
/// One mutex guards the tables and is held for table operations only, never while sending:
/// publish copies the subscriber handles, unlocks, then sends one posted command per subscriber,
/// so a slow or full subscriber costs only its own delivery. Topic names come from peers, so they
/// are validated, the map is ordered (no hash flooding) and every table has a bound.
/// Thread-affinity: any thread.
class Broker {
public:
    /// `metrics` (optional) receives the publish and delivery counters. The broker keeps it alive.
    explicit Broker(BrokerConfig config = {}, std::shared_ptr<IMetrics> metrics = nullptr) noexcept
        : config_{config}, metrics_{std::move(metrics)} {}
    Broker(const Broker&) = delete;
    Broker& operator=(const Broker&) = delete;
    Broker(Broker&&) = delete;
    Broker& operator=(Broker&&) = delete;
    ~Broker() = default;

    /// Send the counters to `metrics` from now on (nullptr: stop). The broker is usually built
    /// before the server whose Server::shared_metrics() it reports to, because the handler factory
    /// the server needs refers to the broker; this closes that circle. Any thread, any time.
    void use_metrics(std::shared_ptr<IMetrics> metrics) noexcept {
        const std::lock_guard lock{mutex_};
        metrics_ = std::move(metrics);
    }

    /// Listen to `topic` through `handle`. Subscribing twice is one subscription.
    /// @return Error{InvalidArgument} for a bad name; Error{BufferFull} when a bound is reached;
    /// Error{ResourceExhausted} on allocation failure. On any error nothing changed.
    [[nodiscard]] Result<void> subscribe(const SessionHandle& handle, std::string_view topic);

    /// Stop listening; harmless for an unknown topic or connection. An emptied topic is erased.
    void unsubscribe(ConnectionId id, std::string_view topic);
    /// Remove every subscription of one connection; idempotent.
    void unsubscribe_all(ConnectionId id);

    /// Send to every subscriber of `topic` except `except` (the publisher in a chat; 0 = nobody).
    /// Validated once: @return Error{InvalidArgument} for a bad name or a payload over the bound,
    /// Error{InvalidUtf8} for text that is not UTF-8. Nothing is sent on error. A topic nobody
    /// listens to gives a report of zero and creates nothing.
    [[nodiscard]] Result<PublishReport>
    publish_text(std::string_view topic, std::string_view text, ConnectionId except = 0);
    [[nodiscard]] Result<PublishReport>
    publish_binary(std::string_view topic, ByteView data, ConnectionId except = 0);

    [[nodiscard]] std::size_t subscriber_count(std::string_view topic) const;
    [[nodiscard]] std::size_t topic_count() const;

private:
    using Subscribers = std::map<ConnectionId, SessionHandle>;

    [[nodiscard]] Result<void> validate_topic(std::string_view topic) const;
    [[nodiscard]] Result<PublishReport>
    publish(std::string_view topic, const SharedMessage& message, ConnectionId except);
    [[nodiscard]] Result<void> add_locked(const SessionHandle& handle, std::string_view topic);
    [[nodiscard]] bool remove_locked(ConnectionId id, std::string_view topic);

    const BrokerConfig config_;
    mutable std::mutex mutex_;
    std::shared_ptr<IMetrics> metrics_;     // guarded by mutex_
    std::atomic<std::size_t> rotation_{0};  // where the next publish starts (fairness, A1)
    std::map<std::string, Subscribers, std::less<>> topics_;
    std::map<ConnectionId, std::set<std::string, std::less<>>> by_connection_;
    std::size_t subscriptions_ = 0;
};

/// What a handler holds so its subscriptions end with its connection (ADR-0024 section 5): the
/// handler lives exactly as long as the connection, so destroying this object calls
/// unsubscribe_all. Use one per handler; the broker must outlive it.
/// Thread-affinity: the handler's loop thread (the Broker itself is any-thread).
class BrokerMembership {
public:
    explicit BrokerMembership(Broker& broker) noexcept : broker_{broker} {}
    BrokerMembership(const BrokerMembership&) = delete;
    BrokerMembership& operator=(const BrokerMembership&) = delete;
    BrokerMembership(BrokerMembership&&) = delete;
    BrokerMembership& operator=(BrokerMembership&&) = delete;
    ~BrokerMembership();

    [[nodiscard]] Result<void> subscribe(const SessionHandle& handle, std::string_view topic);
    void leave(std::string_view topic);

private:
    Broker& broker_;
    ConnectionId id_ = 0;  ///< 0 until the first subscription: nothing to undo
};

}  // namespace wsrv

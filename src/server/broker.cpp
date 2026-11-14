// SPDX-License-Identifier: Apache-2.0
#include "server/broker.hpp"

#include <algorithm>
#include <new>
#include <vector>

namespace wsrv {

namespace {

/// The characters a topic name may contain: letters, digits and . _ : / @ - (ADR-0024 section 2).
[[nodiscard]] bool is_topic_char(char c) noexcept {
    const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    return alnum || c == '.' || c == '_' || c == ':' || c == '/' || c == '@' || c == '-';
}

}  // namespace

Result<void> Broker::validate_topic(std::string_view topic) const {
    if (topic.empty() || topic.size() > config_.max_topic_length ||
        !std::ranges::all_of(topic, is_topic_char)) {
        return fail(ErrorCode::InvalidArgument, "broker: not a valid topic name");
    }
    return {};
}

// ---- registry ----------------------------------------------------------------------------------

Result<void> Broker::subscribe(const SessionHandle& handle, std::string_view topic) {
    if (const Result<void> valid = validate_topic(topic); !valid) {
        return valid;
    }
    const std::lock_guard lock{mutex_};
    return add_locked(handle, topic);
}

/// Check every bound, then insert; if an allocation fails halfway, undo so nothing changed.
Result<void> Broker::add_locked(const SessionHandle& handle, std::string_view topic) {
    const ConnectionId id = handle.id();
    const auto joined = by_connection_.find(id);
    if (joined != by_connection_.end() && joined->second.contains(topic)) {
        return {};  // already subscribed: idempotent
    }
    const std::size_t own = joined == by_connection_.end() ? 0 : joined->second.size();
    const auto existing = topics_.find(topic);
    const std::size_t listeners = existing == topics_.end() ? 0 : existing->second.size();
    const bool new_topic = existing == topics_.end();
    if (own >= config_.max_topics_per_connection ||
        listeners >= config_.max_subscribers_per_topic ||
        (new_topic && topics_.size() >= config_.max_topics) ||
        subscriptions_ >= config_.max_subscriptions) {
        return fail(ErrorCode::BufferFull, "broker: subscription limit reached");
    }
    try {
        topics_[std::string{topic}].emplace(id, handle);
        by_connection_[id].emplace(std::string{topic});
    } catch (const std::bad_alloc&) {
        static_cast<void>(remove_locked(id, topic));  // whatever half was inserted goes again
        return fail(ErrorCode::ResourceExhausted, "broker: out of memory");
    }
    ++subscriptions_;
    return {};
}

/// Remove one subscription from both tables; erase whatever became empty. Idempotent.
/// @return true if the connection was in the topic's table (the caller adjusts the total).
bool Broker::remove_locked(ConnectionId id, std::string_view topic) {
    bool removed_from_topic = false;
    if (const auto found = topics_.find(topic); found != topics_.end()) {
        removed_from_topic = found->second.erase(id) > 0;
        if (found->second.empty()) {
            topics_.erase(found);
        }
    }
    if (const auto joined = by_connection_.find(id); joined != by_connection_.end()) {
        if (const auto name = joined->second.find(topic); name != joined->second.end()) {
            joined->second.erase(name);
        }
        if (joined->second.empty()) {
            by_connection_.erase(joined);
        }
    }
    return removed_from_topic;
}

void Broker::unsubscribe(ConnectionId id, std::string_view topic) {
    const std::lock_guard lock{mutex_};
    if (remove_locked(id, topic) && subscriptions_ > 0) {
        --subscriptions_;
    }
}

void Broker::unsubscribe_all(ConnectionId id) {
    const std::lock_guard lock{mutex_};
    const auto joined = by_connection_.find(id);
    if (joined == by_connection_.end()) {
        return;
    }
    for (const std::string& topic : joined->second) {
        if (const auto found = topics_.find(topic); found != topics_.end()) {
            if (found->second.erase(id) > 0 && subscriptions_ > 0) {
                --subscriptions_;
            }
            if (found->second.empty()) {
                topics_.erase(found);
            }
        }
    }
    by_connection_.erase(joined);
}

std::size_t Broker::subscriber_count(std::string_view topic) const {
    const std::lock_guard lock{mutex_};
    const auto found = topics_.find(topic);
    return found == topics_.end() ? 0 : found->second.size();
}

std::size_t Broker::topic_count() const {
    const std::lock_guard lock{mutex_};
    return topics_.size();
}

// ---- publishing --------------------------------------------------------------------------------

Result<PublishReport>
Broker::publish_text(std::string_view topic, std::string_view text, ConnectionId except) {
    if (const Result<void> valid = validate_topic(topic); !valid) {
        return std::unexpected{valid.error()};
    }
    if (text.size() > config_.max_payload_bytes) {
        return fail(ErrorCode::InvalidArgument, "broker: payload over the bound");
    }
    const Result<SharedMessage> message = SharedMessage::text(text);
    if (!message) {
        return std::unexpected{message.error()};
    }
    return publish(topic, *message, except);
}

Result<PublishReport>
Broker::publish_binary(std::string_view topic, ByteView data, ConnectionId except) {
    if (const Result<void> valid = validate_topic(topic); !valid) {
        return std::unexpected{valid.error()};
    }
    if (data.size() > config_.max_payload_bytes) {
        return fail(ErrorCode::InvalidArgument, "broker: payload over the bound");
    }
    const Result<SharedMessage> message = SharedMessage::binary(data);
    if (!message) {
        return std::unexpected{message.error()};
    }
    return publish(topic, *message, except);
}

/// Copy the recipients under the lock, send outside it. A subscriber whose server is gone
/// (Closing) is removed afterwards: that is the only failure that is permanent.
Result<PublishReport>
Broker::publish(std::string_view topic, const SharedMessage& message, ConnectionId except) {
    std::vector<SessionHandle> recipients;
    try {
        const std::lock_guard lock{mutex_};
        if (const auto found = topics_.find(topic); found != topics_.end()) {
            recipients.reserve(found->second.size());
            for (const auto& [id, handle] : found->second) {
                if (id != except) {
                    recipients.push_back(handle);
                }
            }
        }
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "broker: out of memory");
    }
    PublishReport report;
    report.subscribers = recipients.size();
    for (const SessionHandle& handle : recipients) {
        const Result<void> sent = handle.send(message);
        if (sent) {
            ++report.accepted;
            continue;
        }
        ++report.refused;
        if (sent.error().code == ErrorCode::Closing) {
            unsubscribe(handle.id(), topic);  // permanent: the server is gone (rare, one lock each)
        }
    }
    if (IMetrics* const metrics = metrics_.load(std::memory_order_acquire); metrics != nullptr) {
        metrics->add(Metric::BrokerPublishes);
        metrics->add(Metric::BrokerDeliveriesAccepted, report.accepted);
        metrics->add(Metric::BrokerDeliveriesRefused, report.refused);
    }
    return report;
}

// ---- membership --------------------------------------------------------------------------------

BrokerMembership::~BrokerMembership() {
    if (id_ != 0) {
        broker_.unsubscribe_all(id_);
    }
}

Result<void> BrokerMembership::subscribe(const SessionHandle& handle, std::string_view topic) {
    Result<void> joined = broker_.subscribe(handle, topic);
    if (joined) {
        id_ = handle.id();
    }
    return joined;
}

void BrokerMembership::leave(std::string_view topic) {
    if (id_ != 0) {
        broker_.unsubscribe(id_, topic);
    }
}

}  // namespace wsrv

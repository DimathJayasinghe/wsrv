// SPDX-License-Identifier: Apache-2.0
#include "wsrvd/relay.hpp"

#include "wsrv/wsrv.h"

#include <string_view>

#include "capi/c_status.hpp"

namespace wsrv::wsrvd {

namespace {

[[nodiscard]] std::uint8_t code_of(wsrv_status status) noexcept {
    return static_cast<std::uint8_t>(status);
}

[[nodiscard]] std::uint8_t code_of(const Result<void>& result) noexcept {
    return result ? code_of(WSRV_OK) : code_of(to_c_status(result.error().code));
}

}  // namespace

std::optional<HostEvent> Relay::next_event() {
    for (;;) {
        std::optional<HostEvent> event = backend_.poll();
        if (!event) {
            return std::nullopt;
        }
        if (stale_.contains(event->conn)) {
            // A connection the previous application knew. The new one never saw its Open, so
            // showing it a message or a Close for it would be nonsense. Its Close ends the entry.
            if (event->type == HostEventType::Close) {
                stale_.erase(event->conn);
            }
            continue;
        }
        if (event->type == HostEventType::Open) {
            open_.insert(event->conn);
        } else if (event->type == HostEventType::Close) {
            open_.erase(event->conn);
        }
        return event;
    }
}

void Relay::detach() {
    for (const ConnectionId conn : open_) {
        backend_.drop_subscriptions(conn);  // an HTTP publish must not reach what nobody serves
    }
    stale_.merge(open_);  // moves every id; open_ keeps only duplicates (there are none)
    open_.clear();
}

void Relay::close_stale(std::uint16_t code, std::string_view reason) {
    for (const ConnectionId conn : stale_) {
        static_cast<void>(backend_.close(conn, code, reason).has_value());  // Closing = gone
    }
}

std::optional<std::uint8_t> Relay::refuse(std::uint32_t request,
                                          std::uint64_t conn) const noexcept {
    if (request == 0) {
        return code_of(WSRV_ERR_INVALID_ARGUMENT);  // 0 means "the handshake" (ADR-0017 4a)
    }
    if (!open_.contains(conn)) {
        return code_of(WSRV_ERR_NO_SUCH_CONN);
    }
    return std::nullopt;
}

ipc::Status Relay::execute(const ipc::SendData& command) {
    if (const auto refused = refuse(command.request, command.conn)) {
        return ipc::Status{.request = command.request, .status = *refused};
    }
    const Result<void> result = command.text ? backend_.send_text(command.conn, command.data)
                                             : backend_.send_binary(command.conn, command.data);
    return ipc::Status{.request = command.request, .status = code_of(result)};
}

ipc::Status Relay::execute(const ipc::CloseCommand& command) {
    if (const auto refused = refuse(command.request, command.conn)) {
        return ipc::Status{.request = command.request, .status = *refused};
    }
    const std::string_view reason = as_chars(command.reason);
    const Result<void> result = backend_.close(command.conn, command.code, reason);
    return ipc::Status{.request = command.request, .status = code_of(result)};
}

ipc::Status Relay::execute(const ipc::TopicCommand& command) {
    if (const auto refused = refuse(command.request, command.conn)) {
        return ipc::Status{.request = command.request, .status = *refused};
    }
    const std::string_view topic = as_chars(command.topic);
    const Result<void> result = command.subscribe ? backend_.subscribe(command.conn, topic)
                                                  : backend_.unsubscribe(command.conn, topic);
    if (!result && result.error().code == ErrorCode::BufferFull) {
        return ipc::Status{.request = command.request, .status = code_of(WSRV_ERR_LIMIT)};
    }
    return ipc::Status{.request = command.request, .status = code_of(result)};
}

ipc::Status Relay::execute(const ipc::Publish& command) {
    if (command.request == 0) {
        return ipc::Status{.request = 0, .status = code_of(WSRV_ERR_INVALID_ARGUMENT)};
    }
    // A publish never fails because subscribers are slow (ADR-0024 section 4): only the name, the
    // payload, UTF-8 or "broker off" can refuse it. The report is for the HTTP answer, not here.
    const Result<PublishReport> result = backend_.publish(
        as_chars(command.topic), command.payload, command.text, command.except);
    return ipc::Status{.request = command.request,
                       .status = result ? code_of(WSRV_OK)
                                        : code_of(to_c_status(result.error().code))};
}

ipc::Status Relay::execute(const ipc::HandshakeAccept& command) {
    if (command.request == 0) {
        return ipc::Status{.request = 0, .status = code_of(WSRV_ERR_INVALID_ARGUMENT)};
    }
    std::optional<std::string_view> chosen;
    if (!command.subprotocol.empty()) {
        chosen = as_chars(command.subprotocol);
    }
    const Result<void> result = backend_.accept_handshake(command.conn, chosen);
    return ipc::Status{.request = command.request, .status = code_of(result)};
}

ipc::Status Relay::execute(const ipc::HandshakeReject& command) {
    if (command.request == 0) {
        return ipc::Status{.request = 0, .status = code_of(WSRV_ERR_INVALID_ARGUMENT)};
    }
    const Result<void> result = backend_.reject_handshake(command.conn, command.status);
    return ipc::Status{.request = command.request, .status = code_of(result)};
}

}  // namespace wsrv::wsrvd

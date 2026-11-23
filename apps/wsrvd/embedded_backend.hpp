// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>

#include "capi/embedded_server.hpp"
#include "wsrvd/backend.hpp"

namespace wsrv::wsrvd {

/// The real backend: the embedded WebSocket server of ADR-0016, seen through IBackend. The server
/// runs its own I/O threads; the daemon only ever calls this adapter from its loop thread, so the
/// "one consumer thread" rule of EmbeddedServer::poll holds.
/// Thread-affinity: loop-thread-only.
class EmbeddedBackend final : public IBackend {
public:
    explicit EmbeddedBackend(EmbeddedServer& server) noexcept : server_{server} {}

    [[nodiscard]] std::optional<HostEvent> poll() override {
        return server_.poll(std::chrono::milliseconds{0});  // only look, never wait
    }
    [[nodiscard]] int event_fd() const noexcept override {
        return server_.event_fd();
    }
    [[nodiscard]] Result<void> send_text(ConnectionId conn, ByteView text) override {
        return server_.send_text(conn, as_chars(text));
    }
    [[nodiscard]] Result<void> send_binary(ConnectionId conn, ByteView data) override {
        return server_.send_binary(conn, data);
    }
    [[nodiscard]] Result<void>
    close(ConnectionId conn, std::uint16_t code, std::string_view reason) override {
        // The core decides whether the code may appear on the wire (RFC 6455 section 7.4) and
        // answers InvalidArgument if not; this is only a change of type.
        return server_.close(conn, static_cast<CloseCode>(code), reason);
    }
    [[nodiscard]] Result<void>
    accept_handshake(ConnectionId conn, std::optional<std::string_view> subprotocol) override {
        return server_.accept_handshake(conn, subprotocol);
    }
    [[nodiscard]] Result<void> reject_handshake(ConnectionId conn, int status) override {
        return server_.reject_handshake(conn, status);
    }
    [[nodiscard]] Result<void> subscribe(ConnectionId conn, std::string_view topic) override {
        return server_.subscribe(conn, topic);
    }
    [[nodiscard]] Result<void> unsubscribe(ConnectionId conn, std::string_view topic) override {
        return server_.unsubscribe(conn, topic);
    }
    [[nodiscard]] Result<PublishReport>
    publish(std::string_view topic, ByteView payload, bool text, ConnectionId except) override {
        return text ? server_.publish_text(topic, as_chars(payload), except)
                    : server_.publish_binary(topic, payload, except);
    }
    void drop_subscriptions(ConnectionId conn) noexcept override {
        server_.drop_subscriptions(conn);
    }
    void set_handshake_intake(bool open) noexcept override {
        server_.set_handshake_intake(open);
    }

private:
    EmbeddedServer& server_;
};

}  // namespace wsrv::wsrvd

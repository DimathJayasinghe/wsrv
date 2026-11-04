// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/error.hpp"
#include "server/config.hpp"
#include "server/server_builder.hpp"

#include <cstdint>
#include <string>
#include <string_view>

#include "capi/event_queue.hpp"

namespace wsrv {

/// The settings a host can give an embedded server, as text key/value pairs (ADR-0016 section 6).
/// Text keys are the one configuration shape every binding can express without sharing any C++
/// type, and a key is never removed or changes meaning, so adding settings is always compatible.
///
/// set() checks that a value is well formed (a clean number, a known boolean) and the key is known.
/// Whether the combination is usable is decided where it is used: ServerBuilder::build() and
/// EventQueue::create() validate once, so the rules live in one place each.
///
/// Keys: listen_host, listen_port, threads, allow_origin (repeatable), allow_missing_origin,
/// max_message_size, max_connections, shutdown_grace_ms, event_queue_events, event_queue_bytes,
/// and for the handshake hook (ADR-0018): handshake_hook (bool, off), handshake_forward_header
/// (repeatable header name), handshake_decision_timeout_ms (> 0), max_pending_handshakes (> 0).
/// and for TLS (ADR-0019): tls_cert (certificate chain file, PEM), tls_key (private key file, PEM).
/// max_message_size also sizes the outbound queue at twice that, so a message that can be received
/// can be sent back (as in the echo example).
/// Thread-affinity: not thread-safe; configure on one thread before starting.
class EmbeddedConfig {
public:
    /// @return Error{InvalidConfig} for an unknown key or a malformed value; nothing changes then.
    [[nodiscard]] Result<void> set(std::string_view key, std::string_view value);

    [[nodiscard]] const std::string& host() const noexcept {
        return host_;
    }
    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_;
    }
    [[nodiscard]] const ServerConfig& server() const noexcept {
        return server_;
    }
    [[nodiscard]] EventQueueConfig queue() const noexcept {
        return queue_;
    }

    /// True when the host wants to be asked before each upgrade (handshake_hook).
    [[nodiscard]] bool handshake_hook() const noexcept {
        return handshake_hook_;
    }

    /// The TLS files (empty = not set). TLS is asked for as soon as either key is set, so that a
    /// half setup is refused at start() instead of quietly serving plain text.
    [[nodiscard]] const std::string& tls_cert() const noexcept {
        return tls_cert_;
    }
    [[nodiscard]] const std::string& tls_key() const noexcept {
        return tls_key_;
    }
    [[nodiscard]] bool tls_requested() const noexcept {
        return !tls_cert_.empty() || !tls_key_.empty();
    }

    /// Copy the listen address and the server settings into `builder`.
    void apply_to(ServerBuilder& builder) const;

private:
    [[nodiscard]] Result<void> set_listen_host(std::string_view value);
    [[nodiscard]] Result<void> set_listen_port(std::string_view value);
    [[nodiscard]] Result<void> set_threads(std::string_view value);
    [[nodiscard]] Result<void> add_allow_origin(std::string_view value);
    [[nodiscard]] Result<void> set_allow_missing_origin(std::string_view value);
    [[nodiscard]] Result<void> set_max_message_size(std::string_view value);
    [[nodiscard]] Result<void> set_max_connections(std::string_view value);
    [[nodiscard]] Result<void> set_shutdown_grace_ms(std::string_view value);
    [[nodiscard]] Result<void> set_event_queue_events(std::string_view value);
    [[nodiscard]] Result<void> set_event_queue_bytes(std::string_view value);
    [[nodiscard]] Result<void> set_handshake_hook(std::string_view value);
    [[nodiscard]] Result<void> add_handshake_forward_header(std::string_view value);
    [[nodiscard]] Result<void> set_handshake_decision_timeout_ms(std::string_view value);
    [[nodiscard]] Result<void> set_max_pending_handshakes(std::string_view value);
    [[nodiscard]] Result<void> set_tls_cert(std::string_view value);
    [[nodiscard]] Result<void> set_tls_key(std::string_view value);

    std::string host_ = "127.0.0.1";  // loopback unless the operator opts into exposure
    std::uint16_t port_ = 0;
    ServerConfig server_;
    EventQueueConfig queue_;
    bool handshake_hook_ = false;
    std::string tls_cert_;
    std::string tls_key_;
};

}  // namespace wsrv

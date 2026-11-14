// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/error.hpp"
#include "server/config.hpp"
#include "server/server_builder.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "capi/event_queue.hpp"

namespace wsrv {

/// The most tls_sni settings one config takes. The same number as tls/server_context.hpp's
/// kMaxSniEntries (a test in embedded_tls_test.cpp keeps them equal): this header cannot include
/// tls/, and the config must be bounded before the context ever sees it.
inline constexpr std::size_t kMaxTlsSniSettings = 64;

/// One tls_sni setting: a host name (or "*." wildcard) and the files of its certificate.
struct TlsSniSetting {
    std::string name;
    std::string certificate;
    std::string key;
};

/// The settings a host can give an embedded server, as text key/value pairs (ADR-0016 section 6).
/// Text keys are the one configuration shape every binding can express without sharing any C++
/// type, and a key is never removed or changes meaning, so adding settings is always compatible.
///
/// set() checks that a value is well formed (a clean number, a known boolean) and the key is known.
/// Whether the combination is usable is decided where it is used: ServerBuilder::build() and
/// EventQueue::create() validate once, so the rules live in one place each.
///
/// Keys: listen_host, listen_port, threads, allow_origin (repeatable), allow_missing_origin,
/// max_message_size, max_connections, max_connections_per_ip (default 100) and
/// max_new_connections_per_ip_per_second (default 50), both 0 = no limit (ADR-0021),
/// shutdown_grace_ms, event_queue_events, event_queue_bytes, and for the handshake hook (ADR-0018):
/// handshake_hook (bool, off), handshake_forward_header (repeatable header name),
/// handshake_decision_timeout_ms (> 0), max_pending_handshakes (> 0). and for TLS (ADR-0019):
/// tls_cert (certificate chain file, PEM), tls_key (private key file, PEM), tls_sni (repeatable
/// "name=cert_file:key_file", at most 64: a certificate for one host name or one
/// "*.example.com" wildcard; the paths must not contain a colon).
/// and for permessage-deflate (ADR-0020 section 6, off by default): permessage_deflate (bool),
/// deflate_server_max_window_bits (9..15, default 12), deflate_server_no_context_takeover (bool,
/// default true), deflate_level (1..9, default 6), deflate_min_size (bytes, default 128),
/// deflate_max_ratio (1..1000, default 100). The tuning keys only matter once permessage_deflate is
/// true, in any order; set() checks the number is well formed and fits its field, the ranges are
/// checked where the settings are used (ServerBuilder::build). A build without zlib refuses to
/// start with permessage_deflate=true instead of quietly serving uncompressed.
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
    [[nodiscard]] const std::vector<TlsSniSetting>& tls_sni() const noexcept {
        return tls_sni_;
    }
    [[nodiscard]] bool tls_requested() const noexcept {
        return !tls_cert_.empty() || !tls_key_.empty() || !tls_sni_.empty();
    }

    /// permessage-deflate (ADR-0020): asked for with permessage_deflate=true; the options are the
    /// tuning keys' values (defaults until set) and only apply when it is on.
    [[nodiscard]] bool permessage_deflate() const noexcept {
        return deflate_enabled_;
    }
    [[nodiscard]] const DeflateOptions& deflate_options() const noexcept {
        return deflate_;
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
    [[nodiscard]] Result<void> set_max_connections_per_ip(std::string_view value);
    [[nodiscard]] Result<void> set_max_new_connections_per_ip_per_second(std::string_view value);
    [[nodiscard]] Result<void> set_shutdown_grace_ms(std::string_view value);
    [[nodiscard]] Result<void> set_event_queue_events(std::string_view value);
    [[nodiscard]] Result<void> set_event_queue_bytes(std::string_view value);
    [[nodiscard]] Result<void> set_handshake_hook(std::string_view value);
    [[nodiscard]] Result<void> add_handshake_forward_header(std::string_view value);
    [[nodiscard]] Result<void> set_handshake_decision_timeout_ms(std::string_view value);
    [[nodiscard]] Result<void> set_max_pending_handshakes(std::string_view value);
    [[nodiscard]] Result<void> set_tls_cert(std::string_view value);
    [[nodiscard]] Result<void> set_tls_key(std::string_view value);
    [[nodiscard]] Result<void> add_tls_sni(std::string_view value);
    [[nodiscard]] Result<void> set_permessage_deflate(std::string_view value);
    [[nodiscard]] Result<void> set_deflate_server_max_window_bits(std::string_view value);
    [[nodiscard]] Result<void> set_deflate_server_no_context_takeover(std::string_view value);
    [[nodiscard]] Result<void> set_deflate_level(std::string_view value);
    [[nodiscard]] Result<void> set_deflate_min_size(std::string_view value);
    [[nodiscard]] Result<void> set_deflate_max_ratio(std::string_view value);

    std::string host_ = "127.0.0.1";  // loopback unless the operator opts into exposure
    std::uint16_t port_ = 0;
    ServerConfig server_;
    EventQueueConfig queue_;
    bool handshake_hook_ = false;
    std::string tls_cert_;
    std::string tls_key_;
    std::vector<TlsSniSetting> tls_sni_;
    bool deflate_enabled_ = false;
    DeflateOptions deflate_;  ///< tuning; used only when deflate_enabled_
};

}  // namespace wsrv

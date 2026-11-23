// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/error.hpp"
#include "server/broker.hpp"
#include "server/config.hpp"
#include "server/server_builder.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
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

namespace detail {
/// Sums that stop at the largest size_t instead of wrapping: a huge max_connections must give a
/// huge (and then refused by create()) queue, never a small one.
[[nodiscard]] constexpr std::size_t saturating_sum(std::size_t a, std::size_t b) noexcept {
    return a > std::numeric_limits<std::size_t>::max() - b ? std::numeric_limits<std::size_t>::max()
                                                           : a + b;
}
[[nodiscard]] constexpr std::size_t saturating_product(std::size_t a, std::size_t b) noexcept {
    return b != 0 && a > std::numeric_limits<std::size_t>::max() / b
               ? std::numeric_limits<std::size_t>::max()
               : a * b;
}
}  // namespace detail

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
/// trusted_proxy (repeatable address or CIDR, at most 64) and client_ip_header (default
/// X-Forwarded-For): the client address behind a reverse proxy (ADR-0029),
/// shutdown_grace_ms, event_queue_events, event_queue_bytes, and for the handshake hook (ADR-0018):
/// handshake_hook (bool, off), handshake_forward_header (repeatable header name),
/// handshake_decision_timeout_ms (> 0), max_pending_handshakes (> 0). max_pending_send_bytes
/// (default 64 MiB, 1 .. 1 GiB, per loop): payload bytes of direct sends waiting for the loop
/// (ADR-0032). and for TLS (ADR-0019): tls_cert (certificate chain file, PEM), tls_key (private key
/// file, PEM), tls_sni (repeatable "name=cert_file:key_file", at most 64: a certificate for one
/// host name or one
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
/// and three limits a trusted client may need raised (ADR-0016 A1), each 1..ceiling, never 0:
/// max_frame_payload (bytes, default 1 MiB, ceiling 64 MiB), max_fragments (default 1024, ceiling
/// 16777216) and max_data_frames_per_second (default 10000, ceiling 10000000). A refused value
/// changes nothing.
/// and for the admin listener (ADR-0023 section 3, off by default): admin_port (0..65535; setting
/// it switches the listener on, 0 lets the kernel choose) and admin_host (an IP literal, default
/// 127.0.0.1). It serves /metrics, /healthz and /readyz over plain HTTP with no authentication:
/// keep it on loopback or behind a network policy.
/// and for the pub/sub broker (ADR-0027 section 1, off by default): broker (bool) and its bounds
/// broker_max_topics (default 1024, ceiling 1000000), broker_max_subscribers_per_topic (10000,
/// 1000000), broker_max_topics_per_connection (64, 1024), broker_max_subscriptions (100000,
/// 10000000) and broker_max_payload_bytes (65536, 1048576). Never 0: a bound is not switched off.
/// The publish endpoint (ADR-0028, off by default): publish_host (127.0.0.1) and publish_port; it
/// needs broker=true and a token given through set_publish_token(), and its largest body is
/// broker_max_payload_bytes.
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
    /// The admin listener (ADR-0023): on only once a port was given.
    [[nodiscard]] bool admin_requested() const noexcept {
        return admin_port_.has_value();
    }
    [[nodiscard]] const std::string& admin_host() const noexcept {
        return admin_host_;
    }
    [[nodiscard]] std::uint16_t admin_port() const noexcept {
        return admin_port_.value_or(0);
    }
    [[nodiscard]] const ServerConfig& server() const noexcept {
        return server_;
    }
    /// The event queue's limits. A live connection holds one slot (and kReservedCloseBytes) of the
    /// queue for its Close event for as long as it lives, so a queue smaller than max_connections
    /// would quietly cap the server below that number. A limit the operator did not set therefore
    /// follows max_connections (never below the 4096 events / 8 MiB default, plus that much room
    /// for events in flight); a limit the operator did set is returned exactly as set.
    [[nodiscard]] EventQueueConfig queue() const noexcept {
        EventQueueConfig out = queue_;
        const std::size_t connections = server_.max_connections;
        if (!queue_events_set_) {
            out.max_events = detail::saturating_sum(connections, EventQueueConfig{}.max_events);
        }
        if (!queue_bytes_set_) {
            out.max_bytes = detail::saturating_sum(
                detail::saturating_product(connections, EventQueue::kReservedCloseBytes),
                EventQueueConfig{}.max_bytes);
        }
        return out;
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

    /// The pub/sub broker (ADR-0027): on only with broker=true; the bounds apply when it is.
    [[nodiscard]] bool broker_enabled() const noexcept {
        return broker_enabled_;
    }
    [[nodiscard]] const BrokerConfig& broker_config() const noexcept {
        return broker_;
    }

    /// The publish endpoint (ADR-0028): on only once publish_port was given, and then it needs the
    /// broker and a token. The token is deliberately NOT a key: it cannot come from a config file,
    /// a command line or the C ABI's string settings, only from set_publish_token().
    [[nodiscard]] bool publish_requested() const noexcept {
        return publish_port_.has_value();
    }
    [[nodiscard]] const std::string& publish_host() const noexcept {
        return publish_host_;
    }
    [[nodiscard]] std::uint16_t publish_port() const noexcept {
        return publish_port_.value_or(0);
    }
    [[nodiscard]] const std::string& publish_token() const noexcept {
        return publish_token_;
    }
    /// @return Error{InvalidConfig} when the token is shorter than kMinPublishTokenBytes.
    [[nodiscard]] Result<void> set_publish_token(std::string token);
    /// Whether the publish settings make sense together: with publish_port the broker must be on
    /// and a token must have been given. @return Error{InvalidConfig} otherwise.
    [[nodiscard]] Result<void> check_publish() const;

    /// Copy the listen address and the server settings into `builder`.
    void apply_to(ServerBuilder& builder) const;

private:
    [[nodiscard]] std::optional<Result<void>> set_broker_key(std::string_view key,
                                                             std::string_view value);
    [[nodiscard]] Result<void> set_listen_host(std::string_view value);
    [[nodiscard]] Result<void> set_listen_port(std::string_view value);
    [[nodiscard]] Result<void> set_threads(std::string_view value);
    [[nodiscard]] Result<void> add_allow_origin(std::string_view value);
    [[nodiscard]] Result<void> set_allow_missing_origin(std::string_view value);
    [[nodiscard]] Result<void> set_max_message_size(std::string_view value);
    [[nodiscard]] Result<void> set_max_frame_payload(std::string_view value);
    [[nodiscard]] Result<void> set_max_fragments(std::string_view value);
    [[nodiscard]] Result<void> set_max_data_frames_per_second(std::string_view value);
    [[nodiscard]] Result<void> set_max_connections(std::string_view value);
    [[nodiscard]] Result<void> set_admin_host(std::string_view value);
    [[nodiscard]] Result<void> set_admin_port(std::string_view value);
    [[nodiscard]] Result<void> set_max_connections_per_ip(std::string_view value);
    [[nodiscard]] Result<void> set_max_new_connections_per_ip_per_second(std::string_view value);
    [[nodiscard]] Result<void> set_shutdown_grace_ms(std::string_view value);
    [[nodiscard]] Result<void> set_event_queue_events(std::string_view value);
    [[nodiscard]] Result<void> set_event_queue_bytes(std::string_view value);
    [[nodiscard]] Result<void> set_handshake_hook(std::string_view value);
    [[nodiscard]] Result<void> add_handshake_forward_header(std::string_view value);
    [[nodiscard]] Result<void> add_trusted_proxy(std::string_view value);
    [[nodiscard]] Result<void> set_client_ip_header(std::string_view value);
    [[nodiscard]] Result<void> set_handshake_decision_timeout_ms(std::string_view value);
    [[nodiscard]] Result<void> set_max_pending_handshakes(std::string_view value);
    [[nodiscard]] Result<void> set_max_pending_send_bytes(std::string_view value);
    [[nodiscard]] Result<void> set_tls_cert(std::string_view value);
    [[nodiscard]] Result<void> set_tls_key(std::string_view value);
    [[nodiscard]] Result<void> add_tls_sni(std::string_view value);
    [[nodiscard]] Result<void> set_publish_host(std::string_view value);
    [[nodiscard]] Result<void> set_publish_port(std::string_view value);
    [[nodiscard]] Result<void> set_broker(std::string_view value);
    [[nodiscard]] Result<void> set_broker_max_topics(std::string_view value);
    [[nodiscard]] Result<void> set_broker_max_subscribers_per_topic(std::string_view value);
    [[nodiscard]] Result<void> set_broker_max_topics_per_connection(std::string_view value);
    [[nodiscard]] Result<void> set_broker_max_subscriptions(std::string_view value);
    [[nodiscard]] Result<void> set_broker_max_payload_bytes(std::string_view value);
    [[nodiscard]] Result<void> set_permessage_deflate(std::string_view value);
    [[nodiscard]] Result<void> set_deflate_server_max_window_bits(std::string_view value);
    [[nodiscard]] Result<void> set_deflate_server_no_context_takeover(std::string_view value);
    [[nodiscard]] Result<void> set_deflate_level(std::string_view value);
    [[nodiscard]] Result<void> set_deflate_min_size(std::string_view value);
    [[nodiscard]] Result<void> set_deflate_max_ratio(std::string_view value);

    // Ordered by size, largest first, so the padding analyzer is satisfied (ServerConfig and the
    // strings first, the small integers and flags last).
    ServerConfig server_;
    BrokerConfig broker_;
    EventQueueConfig queue_;
    std::vector<TlsSniSetting> tls_sni_;
    DeflateOptions deflate_;                  ///< tuning; used only when deflate_enabled_
    std::string admin_host_ = "127.0.0.1";    // loopback: the admin page has no authentication
    std::string publish_host_ = "127.0.0.1";  // loopback unless the operator opts into exposure
    std::string publish_token_;               // a secret: never logged, never a config key
    std::string host_ = "127.0.0.1";          // loopback unless the operator opts into exposure
    std::string tls_cert_;
    std::string tls_key_;
    std::optional<std::uint16_t> admin_port_;    // set = the admin listener is on
    std::optional<std::uint16_t> publish_port_;  // set = the publish listener is on
    std::uint16_t port_ = 0;
    bool queue_events_set_ = false;  ///< event_queue_events was given: queue() must not derive it
    bool queue_bytes_set_ = false;   ///< event_queue_bytes was given: queue() must not derive it
    bool handshake_hook_ = false;
    bool broker_enabled_ = false;
    bool deflate_enabled_ = false;
};

}  // namespace wsrv

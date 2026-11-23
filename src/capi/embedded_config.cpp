// SPDX-License-Identifier: Apache-2.0
#include "capi/embedded_config.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <limits>
#include <new>
#include <optional>
#include <system_error>

namespace wsrv {

namespace {

/// A whole-string unsigned number: no sign, no spaces, no suffix, no overflow.
template <class Number>
std::optional<Number> parse_number(std::string_view text) {
    Number value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || text.empty()) {
        return std::nullopt;
    }
    return value;
}

Result<std::size_t> parse_size(std::string_view value, std::string_view context) {
    if (const auto number = parse_number<std::size_t>(value)) {
        return *number;
    }
    return fail(ErrorCode::InvalidConfig, context);
}

/// Ceilings the C layer adds where Session has none (ADR-0016 A1). The frame ceiling is Session's.
constexpr std::uint64_t kMaxFragmentsCeiling = std::uint64_t{1} << 24;
constexpr std::uint64_t kMaxDataFramesPerSecondCeiling = 10'000'000;

/// A whole-string number in [1, ceiling]. There is no "0 = off" and no value above the ceiling: a
/// limit key never switches a guard off by typo (SECURITY_DESIGN section 3, ADR-0016 A1).
Result<std::uint64_t>
parse_limit(std::string_view value, std::uint64_t ceiling, std::string_view context) {
    const auto number = parse_number<std::uint64_t>(value);
    if (!number || *number == 0 || *number > ceiling) {
        return fail(ErrorCode::InvalidConfig, context);
    }
    return *number;
}

/// A boolean is exactly true, false, 1 or 0 (so "yes" or "True" cannot silently mean something).
std::optional<bool> parse_bool(std::string_view value) {
    if (value == "true" || value == "1") {
        return true;
    }
    if (value == "false" || value == "0") {
        return false;
    }
    return std::nullopt;
}

/// A header name is an RFC 9110 section 5.6.2 token: no spaces, colons or control characters.
bool is_header_name(std::string_view name) {
    constexpr std::string_view kPunctuation = "!#$%&'*+-.^_`|~";
    if (name.empty()) {
        return false;
    }
    return std::ranges::all_of(name, [&](char c) {
        const bool alnum =
            (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        return alnum || kPunctuation.find(c) != std::string_view::npos;
    });
}

struct KeyEntry {
    std::string_view name;
    Result<void> (EmbeddedConfig::*setter)(std::string_view);
};

}  // namespace

Result<void> EmbeddedConfig::set(std::string_view key, std::string_view value) {
    static constexpr std::array kKeys{
        KeyEntry{"listen_host", &EmbeddedConfig::set_listen_host},
        KeyEntry{"listen_port", &EmbeddedConfig::set_listen_port},
        KeyEntry{"threads", &EmbeddedConfig::set_threads},
        KeyEntry{"allow_origin", &EmbeddedConfig::add_allow_origin},
        KeyEntry{"allow_missing_origin", &EmbeddedConfig::set_allow_missing_origin},
        KeyEntry{"max_message_size", &EmbeddedConfig::set_max_message_size},
        KeyEntry{"max_frame_payload", &EmbeddedConfig::set_max_frame_payload},
        KeyEntry{"max_fragments", &EmbeddedConfig::set_max_fragments},
        KeyEntry{"max_data_frames_per_second", &EmbeddedConfig::set_max_data_frames_per_second},
        KeyEntry{"max_connections", &EmbeddedConfig::set_max_connections},
        KeyEntry{"admin_host", &EmbeddedConfig::set_admin_host},
        KeyEntry{"admin_port", &EmbeddedConfig::set_admin_port},
        KeyEntry{"max_connections_per_ip", &EmbeddedConfig::set_max_connections_per_ip},
        KeyEntry{"max_new_connections_per_ip_per_second",
                 &EmbeddedConfig::set_max_new_connections_per_ip_per_second},
        KeyEntry{"shutdown_grace_ms", &EmbeddedConfig::set_shutdown_grace_ms},
        KeyEntry{"event_queue_events", &EmbeddedConfig::set_event_queue_events},
        KeyEntry{"event_queue_bytes", &EmbeddedConfig::set_event_queue_bytes},
        KeyEntry{"handshake_hook", &EmbeddedConfig::set_handshake_hook},
        KeyEntry{"handshake_forward_header", &EmbeddedConfig::add_handshake_forward_header},
        KeyEntry{"handshake_decision_timeout_ms",
                 &EmbeddedConfig::set_handshake_decision_timeout_ms},
        KeyEntry{"max_pending_handshakes", &EmbeddedConfig::set_max_pending_handshakes},
        KeyEntry{"tls_cert", &EmbeddedConfig::set_tls_cert},
        KeyEntry{"tls_key", &EmbeddedConfig::set_tls_key},
        KeyEntry{"tls_sni", &EmbeddedConfig::add_tls_sni},
        KeyEntry{"permessage_deflate", &EmbeddedConfig::set_permessage_deflate},
        KeyEntry{"deflate_server_max_window_bits",
                 &EmbeddedConfig::set_deflate_server_max_window_bits},
        KeyEntry{"deflate_server_no_context_takeover",
                 &EmbeddedConfig::set_deflate_server_no_context_takeover},
        KeyEntry{"deflate_level", &EmbeddedConfig::set_deflate_level},
        KeyEntry{"deflate_min_size", &EmbeddedConfig::set_deflate_min_size},
        KeyEntry{"deflate_max_ratio", &EmbeddedConfig::set_deflate_max_ratio},
    };
    for (const KeyEntry& entry : kKeys) {
        if (entry.name == key) {
            return (this->*entry.setter)(value);
        }
    }
    if (const std::optional<Result<void>> broker = set_broker_key(key, value)) {
        return *broker;
    }
    return fail(ErrorCode::InvalidConfig, "config: unknown key");
}

/// The pub/sub keys (ADR-0027), in their own table so `set` stays within the size the linter
/// allows.
/// @return nothing when `key` is not a broker key.
std::optional<Result<void>> EmbeddedConfig::set_broker_key(std::string_view key,
                                                           std::string_view value) {
    static constexpr std::array kBrokerKeys{
        KeyEntry{"broker", &EmbeddedConfig::set_broker},
        KeyEntry{"broker_max_topics", &EmbeddedConfig::set_broker_max_topics},
        KeyEntry{"broker_max_subscribers_per_topic",
                 &EmbeddedConfig::set_broker_max_subscribers_per_topic},
        KeyEntry{"broker_max_topics_per_connection",
                 &EmbeddedConfig::set_broker_max_topics_per_connection},
        KeyEntry{"broker_max_subscriptions", &EmbeddedConfig::set_broker_max_subscriptions},
        KeyEntry{"broker_max_payload_bytes", &EmbeddedConfig::set_broker_max_payload_bytes},
    };
    for (const KeyEntry& entry : kBrokerKeys) {
        if (entry.name == key) {
            return (this->*entry.setter)(value);
        }
    }
    return std::nullopt;
}

void EmbeddedConfig::apply_to(ServerBuilder& builder) const {
    builder.listen(host_, port_);
    builder.config() = server_;
    if (admin_port_) {
        builder.admin(admin_host_, *admin_port_);  // build() checks the address and the bounds
    }
    if (deflate_enabled_) {
        builder.permessage_deflate(deflate_);  // build() checks the ranges and the zlib build
    }
}

Result<void> EmbeddedConfig::set_listen_host(std::string_view value) {
    if (value.empty()) {
        return fail(ErrorCode::InvalidConfig, "config: listen_host is empty");
    }
    host_ = std::string{value};  // an address literal; ServerBuilder::build() checks it
    return {};
}

Result<void> EmbeddedConfig::set_listen_port(std::string_view value) {
    const auto port = parse_number<std::uint16_t>(value);
    if (!port) {
        return fail(ErrorCode::InvalidConfig, "config: listen_port needs a number 0..65535");
    }
    port_ = *port;
    return {};
}

Result<void> EmbeddedConfig::set_threads(std::string_view value) {
    const Result<std::size_t> number = parse_size(value, "config: threads needs a number");
    if (!number) {
        return std::unexpected{number.error()};
    }
    server_.threads = *number;
    return {};
}

Result<void> EmbeddedConfig::add_allow_origin(std::string_view value) {
    if (value.empty()) {
        return fail(ErrorCode::InvalidConfig, "config: allow_origin is empty");
    }
    try {
        server_.handshake.allowed_origins.emplace_back(value);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "config: out of memory");
    }
    return {};
}

Result<void> EmbeddedConfig::set_allow_missing_origin(std::string_view value) {
    const std::optional<bool> flag = parse_bool(value);
    if (!flag) {
        return fail(ErrorCode::InvalidConfig, "config: a boolean is true, false, 1 or 0");
    }
    server_.handshake.allow_missing_origin = *flag;
    return {};
}

Result<void> EmbeddedConfig::set_max_message_size(std::string_view value) {
    const Result<std::size_t> number = parse_size(value, "config: max_message_size needs a number");
    if (!number) {
        return std::unexpected{number.error()};
    }
    if (*number > std::numeric_limits<std::size_t>::max() / 2) {
        return fail(ErrorCode::InvalidConfig, "config: max_message_size is too large");
    }
    server_.session.assembler.max_message_size = *number;
    server_.session.outbound.max_queued_bytes = 2 * *number;
    return {};
}

// ---- the three limits a conformance run raises (ADR-0016 A1) ----

Result<void> EmbeddedConfig::set_max_frame_payload(std::string_view value) {
    const Result<std::uint64_t> bytes = parse_limit(
        value, kMaxConfiguredFrame, "config: max_frame_payload must be 1 to 67108864 (64 MiB)");
    if (!bytes) {
        return std::unexpected{bytes.error()};
    }
    server_.session.parser.max_frame_payload = *bytes;
    return {};
}

Result<void> EmbeddedConfig::set_max_fragments(std::string_view value) {
    const Result<std::uint64_t> count =
        parse_limit(value, kMaxFragmentsCeiling, "config: max_fragments must be 1 to 16777216");
    if (!count) {
        return std::unexpected{count.error()};
    }
    server_.session.assembler.max_fragments = static_cast<std::size_t>(*count);
    return {};
}

Result<void> EmbeddedConfig::set_max_data_frames_per_second(std::string_view value) {
    const Result<std::uint64_t> rate =
        parse_limit(value,
                    kMaxDataFramesPerSecondCeiling,
                    "config: max_data_frames_per_second must be 1 to 10000000");
    if (!rate) {
        return std::unexpected{rate.error()};
    }
    server_.session.max_data_frames_per_second = static_cast<std::uint32_t>(*rate);
    return {};
}

Result<void> EmbeddedConfig::set_max_connections(std::string_view value) {
    const Result<std::size_t> number = parse_size(value, "config: max_connections needs a number");
    if (!number) {
        return std::unexpected{number.error()};
    }
    server_.max_connections = *number;
    return {};
}

// ---- per-IP limits (ADR-0021 section 5): well-formedness here, ranges in ServerBuilder::build
// ----

Result<void> EmbeddedConfig::set_admin_host(std::string_view value) {
    if (value.empty()) {
        return fail(ErrorCode::InvalidConfig, "config: admin_host is empty");
    }
    admin_host_ = std::string{value};  // an address literal; ServerBuilder::build() checks it
    return {};
}

Result<void> EmbeddedConfig::set_admin_port(std::string_view value) {
    const auto port = parse_number<std::uint16_t>(value);
    if (!port) {
        return fail(ErrorCode::InvalidConfig, "config: admin_port needs a number 0..65535");
    }
    admin_port_ = port;
    return {};
}

Result<void> EmbeddedConfig::set_max_connections_per_ip(std::string_view value) {
    const Result<std::size_t> number =
        parse_size(value, "config: max_connections_per_ip needs a number");
    if (!number) {
        return std::unexpected{number.error()};
    }
    server_.max_connections_per_ip = *number;
    return {};
}

Result<void> EmbeddedConfig::set_max_new_connections_per_ip_per_second(std::string_view value) {
    const auto number = parse_number<std::uint32_t>(value);
    if (!number) {
        return fail(ErrorCode::InvalidConfig,
                    "config: max_new_connections_per_ip_per_second needs a number");
    }
    server_.max_new_connections_per_ip_per_second = *number;
    return {};
}

// ADR-0027 section 1: the ceilings below are the documented upper limits of the broker bounds.
constexpr std::uint64_t kBrokerMaxTopicsCeiling = 1'000'000;
constexpr std::uint64_t kBrokerMaxSubscribersPerTopicCeiling = 1'000'000;
constexpr std::uint64_t kBrokerMaxTopicsPerConnectionCeiling = 1024;
constexpr std::uint64_t kBrokerMaxSubscriptionsCeiling = 10'000'000;
constexpr std::uint64_t kBrokerMaxPayloadCeiling = std::uint64_t{1} << 20;

Result<void> EmbeddedConfig::set_broker(std::string_view value) {
    const auto flag = parse_bool(value);
    if (!flag) {
        return fail(ErrorCode::InvalidConfig, "config: broker needs true, false, 1 or 0");
    }
    broker_enabled_ = *flag;
    return {};
}

Result<void> EmbeddedConfig::set_broker_max_topics(std::string_view value) {
    const Result<std::uint64_t> number = parse_limit(
        value, kBrokerMaxTopicsCeiling, "config: broker_max_topics needs a number 1..1000000");
    if (!number) {
        return std::unexpected{number.error()};
    }
    broker_.max_topics = static_cast<std::size_t>(*number);
    return {};
}

Result<void> EmbeddedConfig::set_broker_max_subscribers_per_topic(std::string_view value) {
    const Result<std::uint64_t> number =
        parse_limit(value,
                    kBrokerMaxSubscribersPerTopicCeiling,
                    "config: broker_max_subscribers_per_topic needs a number 1..1000000");
    if (!number) {
        return std::unexpected{number.error()};
    }
    broker_.max_subscribers_per_topic = static_cast<std::size_t>(*number);
    return {};
}

Result<void> EmbeddedConfig::set_broker_max_topics_per_connection(std::string_view value) {
    const Result<std::uint64_t> number =
        parse_limit(value,
                    kBrokerMaxTopicsPerConnectionCeiling,
                    "config: broker_max_topics_per_connection needs a number 1..1024");
    if (!number) {
        return std::unexpected{number.error()};
    }
    broker_.max_topics_per_connection = static_cast<std::size_t>(*number);
    return {};
}

Result<void> EmbeddedConfig::set_broker_max_subscriptions(std::string_view value) {
    const Result<std::uint64_t> number =
        parse_limit(value,
                    kBrokerMaxSubscriptionsCeiling,
                    "config: broker_max_subscriptions needs a number 1..10000000");
    if (!number) {
        return std::unexpected{number.error()};
    }
    broker_.max_subscriptions = static_cast<std::size_t>(*number);
    return {};
}

Result<void> EmbeddedConfig::set_broker_max_payload_bytes(std::string_view value) {
    const Result<std::uint64_t> number =
        parse_limit(value,
                    kBrokerMaxPayloadCeiling,
                    "config: broker_max_payload_bytes needs a number 1..1048576");
    if (!number) {
        return std::unexpected{number.error()};
    }
    broker_.max_payload_bytes = static_cast<std::size_t>(*number);
    return {};
}

Result<void> EmbeddedConfig::set_shutdown_grace_ms(std::string_view value) {
    const auto number = parse_number<std::int64_t>(value);
    if (!number) {
        return fail(ErrorCode::InvalidConfig, "config: shutdown_grace_ms needs a number");
    }
    server_.shutdown_grace = std::chrono::milliseconds{*number};
    return {};
}

Result<void> EmbeddedConfig::set_event_queue_events(std::string_view value) {
    const Result<std::size_t> number =
        parse_size(value, "config: event_queue_events needs a number");
    if (!number) {
        return std::unexpected{number.error()};
    }
    queue_.max_events = *number;
    return {};
}

Result<void> EmbeddedConfig::set_event_queue_bytes(std::string_view value) {
    const Result<std::size_t> number =
        parse_size(value, "config: event_queue_bytes needs a number");
    if (!number) {
        return std::unexpected{number.error()};
    }
    queue_.max_bytes = *number;
    return {};
}

Result<void> EmbeddedConfig::set_handshake_hook(std::string_view value) {
    const std::optional<bool> flag = parse_bool(value);
    if (!flag) {
        return fail(ErrorCode::InvalidConfig, "config: a boolean is true, false, 1 or 0");
    }
    handshake_hook_ = *flag;
    return {};
}

Result<void> EmbeddedConfig::add_handshake_forward_header(std::string_view value) {
    if (!is_header_name(value)) {
        return fail(ErrorCode::InvalidConfig,
                    "config: handshake_forward_header needs a header name");
    }
    try {
        server_.handshake_forward_headers.emplace_back(value);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "config: out of memory");
    }
    return {};
}

Result<void> EmbeddedConfig::set_handshake_decision_timeout_ms(std::string_view value) {
    const auto number = parse_number<std::int64_t>(value);
    if (!number || *number <= 0) {
        return fail(ErrorCode::InvalidConfig,
                    "config: handshake_decision_timeout_ms needs a number above 0");
    }
    server_.handshake_decision_timeout = std::chrono::milliseconds{*number};
    return {};
}

Result<void> EmbeddedConfig::set_max_pending_handshakes(std::string_view value) {
    const Result<std::size_t> number =
        parse_size(value, "config: max_pending_handshakes needs a number");
    if (!number) {
        return std::unexpected{number.error()};
    }
    if (*number == 0) {
        return fail(ErrorCode::InvalidConfig, "config: max_pending_handshakes must be above 0");
    }
    server_.max_pending_handshakes = *number;
    return {};
}

Result<void> EmbeddedConfig::set_tls_cert(std::string_view value) {
    if (value.empty()) {
        return fail(ErrorCode::InvalidConfig, "config: tls_cert needs a file path");
    }
    tls_cert_ = std::string{value};  // the file is read at start(), where the reason is reported
    return {};
}

Result<void> EmbeddedConfig::set_tls_key(std::string_view value) {
    if (value.empty()) {
        return fail(ErrorCode::InvalidConfig, "config: tls_key needs a file path");
    }
    tls_key_ = std::string{value};
    return {};
}

Result<void> EmbeddedConfig::add_tls_sni(std::string_view value) {
    constexpr std::string_view kShape = "config: tls_sni needs name=cert_file:key_file";
    // name=cert:key. The first '=' ends the name; the one ':' splits the two paths, so a path with
    // a colon in it is refused rather than guessed at.
    const std::size_t equals = value.find('=');
    if (equals == std::string_view::npos) {
        return fail(ErrorCode::InvalidConfig, kShape);
    }
    const std::string_view name = value.substr(0, equals);
    const std::string_view paths = value.substr(equals + 1);
    const std::size_t colon = paths.find(':');
    if (colon == std::string_view::npos || paths.find(':', colon + 1) != std::string_view::npos) {
        return fail(ErrorCode::InvalidConfig, kShape);
    }
    const std::string_view certificate = paths.substr(0, colon);
    const std::string_view key = paths.substr(colon + 1);
    if (name.empty() || certificate.empty() || key.empty()) {
        return fail(ErrorCode::InvalidConfig, kShape);
    }
    if (tls_sni_.size() >= kMaxTlsSniSettings) {
        return fail(ErrorCode::InvalidConfig, "config: too many tls_sni settings");
    }
    try {
        tls_sni_.push_back({std::string{name}, std::string{certificate}, std::string{key}});
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "config: out of memory");
    }
    return {};
}

// ---- permessage-deflate (ADR-0020 section 6) ----------------------------------------------------

Result<void> EmbeddedConfig::set_permessage_deflate(std::string_view value) {
    const std::optional<bool> flag = parse_bool(value);
    if (!flag) {
        return fail(ErrorCode::InvalidConfig, "config: a boolean is true, false, 1 or 0");
    }
    deflate_enabled_ = *flag;
    return {};
}

// A window or level is read as an unsigned byte: "-1", "256" and "x" are refused here, so the
// value that reaches the int field has not wrapped. Whether it is in range is build()'s verdict.
Result<void> EmbeddedConfig::set_deflate_server_max_window_bits(std::string_view value) {
    const auto bits = parse_number<std::uint8_t>(value);
    if (!bits) {
        return fail(ErrorCode::InvalidConfig,
                    "config: deflate_server_max_window_bits needs a number");
    }
    deflate_.policy.server_max_window_bits = *bits;
    return {};
}

Result<void> EmbeddedConfig::set_deflate_server_no_context_takeover(std::string_view value) {
    const std::optional<bool> flag = parse_bool(value);
    if (!flag) {
        return fail(ErrorCode::InvalidConfig, "config: a boolean is true, false, 1 or 0");
    }
    deflate_.policy.server_no_context_takeover = *flag;
    return {};
}

Result<void> EmbeddedConfig::set_deflate_level(std::string_view value) {
    const auto level = parse_number<std::uint8_t>(value);
    if (!level) {
        return fail(ErrorCode::InvalidConfig, "config: deflate_level needs a number");
    }
    deflate_.level = *level;
    return {};
}

Result<void> EmbeddedConfig::set_deflate_min_size(std::string_view value) {
    const Result<std::size_t> number = parse_size(value, "config: deflate_min_size needs a number");
    if (!number) {
        return std::unexpected{number.error()};
    }
    deflate_.min_compress_size = *number;
    return {};
}

Result<void> EmbeddedConfig::set_deflate_max_ratio(std::string_view value) {
    const auto ratio = parse_number<std::uint32_t>(value);
    if (!ratio) {
        return fail(ErrorCode::InvalidConfig, "config: deflate_max_ratio needs a number");
    }
    deflate_.max_inflate_ratio = *ratio;
    return {};
}

}  // namespace wsrv

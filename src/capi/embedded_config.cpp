// SPDX-License-Identifier: Apache-2.0
#include "capi/embedded_config.hpp"

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
        KeyEntry{"max_connections", &EmbeddedConfig::set_max_connections},
        KeyEntry{"shutdown_grace_ms", &EmbeddedConfig::set_shutdown_grace_ms},
        KeyEntry{"event_queue_events", &EmbeddedConfig::set_event_queue_events},
        KeyEntry{"event_queue_bytes", &EmbeddedConfig::set_event_queue_bytes},
    };
    for (const KeyEntry& entry : kKeys) {
        if (entry.name == key) {
            return (this->*entry.setter)(value);
        }
    }
    return fail(ErrorCode::InvalidConfig, "config: unknown key");
}

void EmbeddedConfig::apply_to(ServerBuilder& builder) const {
    builder.listen(host_, port_);
    builder.config() = server_;
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
    if (value == "true" || value == "1") {
        server_.handshake.allow_missing_origin = true;
    } else if (value == "false" || value == "0") {
        server_.handshake.allow_missing_origin = false;
    } else {
        return fail(ErrorCode::InvalidConfig, "config: a boolean is true, false, 1 or 0");
    }
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

Result<void> EmbeddedConfig::set_max_connections(std::string_view value) {
    const Result<std::size_t> number = parse_size(value, "config: max_connections needs a number");
    if (!number) {
        return std::unexpected{number.error()};
    }
    server_.max_connections = *number;
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

}  // namespace wsrv

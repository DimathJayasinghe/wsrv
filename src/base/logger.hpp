// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string_view>

namespace wsrv {

enum class LogLevel : std::uint8_t { Trace, Debug, Info, Warn, Error };

/// One thing that happened (ADR-0037). Every view is only valid for the duration of the call.
struct LogRecord {
    LogLevel level = LogLevel::Info;
    /// A stable snake_case name an operator can match in an alert ("slow_consumer"); at most 32
    /// characters are written. Field and event names are stable within 1.x, the message is not.
    std::string_view event{};  // NOLINT(readability-redundant-member-init):
                               // -Wmissing-field-initializers wants it
    /// Human text. May contain peer-influenced bytes: the sink escapes and caps it.
    std::string_view message{};  // NOLINT(readability-redundant-member-init):
                                 // -Wmissing-field-initializers wants it
    /// The connection id (NFR-O3); 0 when the event is not about one connection.
    std::uint64_t conn = 0;
    /// The client address, empty unless the operator enabled `log_peer` (an address is personal
    /// data, NFR-O1).
    std::string_view
        peer{};  // NOLINT(readability-redundant-member-init): -Wmissing-field-initializers wants it
};

/// Destination for log lines, injected so the library never owns a global logger.
/// Contract: `message` may contain peer-influenced text. Implementations that write to a terminal
/// or file MUST escape control characters and cap the length (log injection, SECURITY_DESIGN T14).
/// Thread-affinity: implementations used from several loops must be thread-safe (documented per
/// class).
class ILogger {
public:
    ILogger() = default;
    ILogger(const ILogger&) = delete;
    ILogger& operator=(const ILogger&) = delete;
    ILogger(ILogger&&) = delete;
    ILogger& operator=(ILogger&&) = delete;
    virtual ~ILogger() = default;

    virtual void log(const LogRecord& record) = 0;

    /// False when a record at `level` would be dropped, so a caller can skip building its text.
    [[nodiscard]] virtual bool enabled(LogLevel /*level*/) const noexcept {
        return true;
    }

    /// The common case: an event with no connection and no peer.
    void log(LogLevel level, std::string_view event, std::string_view message) {
        log(LogRecord{level, event, message, 0, {}});
    }
};

/// Discards everything (Null Object pattern: callers never test for "no logger").
class NullLogger final : public ILogger {
public:
    using ILogger::log;
    void log(const LogRecord& /*record*/) override {}
    [[nodiscard]] bool enabled(LogLevel /*level*/) const noexcept override {
        return false;
    }
};

}  // namespace wsrv

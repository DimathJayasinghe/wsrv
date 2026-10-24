// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string_view>

namespace wsrv {

enum class LogLevel : std::uint8_t { Trace, Debug, Info, Warn, Error };

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

    virtual void log(LogLevel level, std::string_view message) = 0;
};

/// Discards everything (Null Object pattern: callers never test for "no logger").
class NullLogger final : public ILogger {
public:
    void log(LogLevel /*level*/, std::string_view /*message*/) override {}
};

}  // namespace wsrv

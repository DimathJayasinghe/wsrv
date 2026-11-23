// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/error.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "capi/embedded_config.hpp"
#include "ipc/message.hpp"

namespace wsrv::wsrvd {

/// Everything the daemon is told, as text key/value pairs (ADR-0017). The IPC keys are handled
/// here; every other key goes to EmbeddedConfig::set, so the WebSocket settings (listen_host,
/// max_message_size, ...) have exactly one parser and one set of rules.
///
///   ipc_socket           path of the Unix socket (required)
///   ipc_token_file       file holding the shared token; must be readable by its owner only
///   publish_token_file   file holding the publish token (ADR-0028), same rules, at least 32 bytes;
///                        required with publish_port, and there is no publish_token key
///   ipc_max_frame        largest IPC frame after HELLO; default max_message_size + 64 KiB
///   ipc_hello_timeout_ms how long a new connection may take to say HELLO (default 5000)
///   ipc_stall_ms         how long the application may leave our output unread before we close
///                        the link (default 10000, 100 to 600000)
///
/// There is deliberately NO `ipc_token` key: a secret on the command line is visible to every
/// local user in `ps` and /proc/<pid>/cmdline, and one in a world-readable config file is no
/// better.
///
/// Usage: set()/apply_* for every input, then finish() once. Accessors are valid after finish().
/// Thread-affinity: not thread-safe; configure on one thread before starting.
class DaemonConfig {
public:
    static constexpr std::size_t kMinMaxFrame = 4096;
    static constexpr std::size_t kDefaultFrameHeadroom = std::size_t{64} << 10U;
    static constexpr std::chrono::milliseconds kDefaultHelloTimeout{5000};
    static constexpr std::chrono::milliseconds kMaxHelloTimeout{60000};
    static constexpr std::chrono::milliseconds kDefaultStallTimeout{10000};
    static constexpr std::chrono::milliseconds kMinStallTimeout{100};
    static constexpr std::chrono::milliseconds kMaxStallTimeout{600000};
    /// An EVENT frame besides its payload: the 6-byte frame header and the 12 body fields.
    static constexpr std::size_t kEventOverhead = ipc::kFrameHeaderSize + 12;

    /// @return Error{InvalidConfig} for an unknown key, a malformed value or the forbidden
    /// `ipc_token`; nothing changes then.
    [[nodiscard]] Result<void> set(std::string_view key, std::string_view value);

    /// One command-line flag, `--key=value` (dashes in the key mean underscores: `--ipc-socket=`).
    [[nodiscard]] Result<void> apply_argument(std::string_view argument);

    /// The text of a config file: one `key = value` per line, blank lines and `#` comments ignored.
    [[nodiscard]] Result<void> apply_file_text(std::string_view text);

    /// Check the whole configuration and fill in what was left to defaults.
    /// @return Error{InvalidConfig} if ipc_socket is missing or the frame limit cannot work.
    [[nodiscard]] Result<void> finish();

    [[nodiscard]] const EmbeddedConfig& embedded() const noexcept {
        return embedded_;
    }
    [[nodiscard]] const std::string& socket_path() const noexcept {
        return socket_path_;
    }
    /// Empty when no token file is configured.
    [[nodiscard]] const std::string& token_file() const noexcept {
        return token_file_;
    }
    /// The file holding the publish token (ADR-0028); empty when none is configured. The token
    /// itself is read by prepare() and given to the embedded config with set_publish_token().
    [[nodiscard]] const std::string& publish_token_file() const noexcept {
        return publish_token_file_;
    }
    /// Hand the publish token (already read from its file) to the embedded server's config.
    [[nodiscard]] Result<void> set_publish_token(std::string token) {
        return embedded_.set_publish_token(std::move(token));
    }
    [[nodiscard]] std::uint32_t max_frame() const noexcept {
        return max_frame_;
    }
    [[nodiscard]] std::chrono::milliseconds hello_timeout() const noexcept {
        return hello_timeout_;
    }
    /// How long the application may leave the daemon's output unread before the link is closed.
    [[nodiscard]] std::chrono::milliseconds stall_timeout() const noexcept {
        return stall_timeout_;
    }

private:
    [[nodiscard]] std::optional<Result<void>> set_publish_key(std::string_view key,
                                                             std::string_view value);
    EmbeddedConfig embedded_;
    std::string socket_path_;
    std::string token_file_;
    std::string publish_token_file_;
    std::uint64_t explicit_max_frame_ = 0;  // 0 = not set
    std::uint32_t max_frame_ = 0;
    std::chrono::milliseconds hello_timeout_ = kDefaultHelloTimeout;
    std::chrono::milliseconds stall_timeout_ = kDefaultStallTimeout;
};

}  // namespace wsrv::wsrvd

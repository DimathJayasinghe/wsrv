// SPDX-License-Identifier: Apache-2.0
#include "wsrvd/config.hpp"

#include <algorithm>
#include <charconv>
#include <optional>
#include <system_error>

#include "ipc/message.hpp"

namespace wsrv::wsrvd {

namespace {

/// A whole-string unsigned number: no sign, no spaces, no suffix, no overflow.
std::optional<std::uint64_t> parse_number(std::string_view text) {
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::string_view trim(std::string_view text) {
    constexpr std::string_view kSpace = " \t\r";
    const std::size_t first = text.find_first_not_of(kSpace);
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(kSpace) - first + 1);
}

/// `--ipc-socket` and `ipc_socket` name the same key.
std::string normalised(std::string_view key) {
    std::string out{key};
    std::ranges::replace(out, '-', '_');
    return out;
}

/// The accepted range of one millisecond setting, and what to say when a value is outside it.
struct MillisRule {
    std::chrono::milliseconds min;
    std::chrono::milliseconds max;
    std::string_view message;  // a string literal (Error::context)
};

/// A whole-string number of milliseconds within the rule; `out` changes only on success.
Result<void>
parse_millis(std::string_view value, const MillisRule& rule, std::chrono::milliseconds& out) {
    const auto number = parse_number(value);
    if (!number || *number < static_cast<std::uint64_t>(rule.min.count()) ||
        *number > static_cast<std::uint64_t>(rule.max.count())) {
        return fail(ErrorCode::InvalidConfig, rule.message);
    }
    out = std::chrono::milliseconds{static_cast<std::int64_t>(*number)};
    return {};
}

}  // namespace

/// The token file keys (publish: ADR-0028, admin: ADR-0041), apart from `set` so it stays within
/// the size the linter allows. @return nothing when `key` is not one of them.
std::optional<Result<void>> DaemonConfig::set_token_file_key(std::string_view key,
                                                             std::string_view value) {
    if (key == "admin_token") {
        return fail(ErrorCode::InvalidConfig,
                    "config: there is no admin_token; put the token in a file and use "
                    "admin_token_file");
    }
    if (key == "admin_token_file") {
        if (value.empty()) {
            return fail(ErrorCode::InvalidConfig, "config: admin_token_file needs a path");
        }
        admin_token_file_ = std::string{value};
        return Result<void>{};
    }
    if (key == "publish_token") {
        return fail(ErrorCode::InvalidConfig,
                    "config: there is no publish_token; put the token in a file and use "
                    "publish_token_file");
    }
    if (key == "publish_token_file") {
        if (value.empty()) {
            return fail(ErrorCode::InvalidConfig, "config: publish_token_file needs a path");
        }
        publish_token_file_ = std::string{value};
        return Result<void>{};
    }
    return std::nullopt;
}

/// The TCP transport keys (ADR-0038), apart from `set` so it stays within the size the linter
/// allows. @return nothing when `key` is not one of them.
std::optional<Result<void>> DaemonConfig::set_transport_key(std::string_view key,
                                                            std::string_view value) {
    if (key == "ipc_port") {
        const auto number = parse_number(value);
        if (!number || *number > 65535) {
            return fail(ErrorCode::InvalidConfig, "config: ipc_port must be a number from 0 to 65535");
        }
        ipc_port_ = static_cast<std::uint16_t>(*number);
        return Result<void>{};  // handled
    }
    if (key == "ipc_host") {
        if (value.empty()) {
            return fail(ErrorCode::InvalidConfig, "config: ipc_host needs an address");
        }
        ipc_host_ = std::string{value};  // an address literal; finish() parses it
        ipc_host_set_ = true;
        return Result<void>{};  // handled
    }
    return std::nullopt;
}

Result<void> DaemonConfig::set(std::string_view key, std::string_view value) {
    if (key == "ipc_socket") {
        if (value.empty()) {
            return fail(ErrorCode::InvalidConfig, "config: ipc_socket needs a path");
        }
        socket_path_ = std::string{value};
        return {};
    }
    if (key == "ipc_token") {
        return fail(
            ErrorCode::InvalidConfig,
            "config: there is no ipc_token; put the token in a file and use ipc_token_file");
    }
    if (key == "ipc_token_file") {
        if (value.empty()) {
            return fail(ErrorCode::InvalidConfig, "config: ipc_token_file needs a path");
        }
        token_file_ = std::string{value};
        return {};
    }
    if (const std::optional<Result<void>> files = set_token_file_key(key, value)) {
        return *files;
    }
    if (const std::optional<Result<void>> transport = set_transport_key(key, value)) {
        return *transport;
    }
    if (key == "ipc_max_frame") {
        const auto number = parse_number(value);
        if (!number || *number < kMinMaxFrame || *number > ipc::kMaxFrameCeiling) {
            return fail(ErrorCode::InvalidConfig,
                        "config: ipc_max_frame must be a number from 4096 to 67108864");
        }
        explicit_max_frame_ = *number;
        return {};
    }
    if (key == "ipc_hello_timeout_ms") {
        return parse_millis(value,
                            {std::chrono::milliseconds{1},
                             kMaxHelloTimeout,
                             "config: ipc_hello_timeout_ms must be a number from 1 to 60000"},
                            hello_timeout_);
    }
    if (key == "ipc_stall_ms") {
        return parse_millis(value,
                            {kMinStallTimeout,
                             kMaxStallTimeout,
                             "config: ipc_stall_ms must be a number from 100 to 600000"},
                            stall_timeout_);
    }
    return embedded_.set(key, value);
}

Result<void> DaemonConfig::apply_argument(std::string_view argument) {
    if (!argument.starts_with("--")) {
        return fail(ErrorCode::InvalidConfig, "argument: flags look like --key=value");
    }
    const std::string_view body = argument.substr(2);
    const std::size_t equals = body.find('=');
    if (equals == std::string_view::npos || equals == 0) {
        return fail(ErrorCode::InvalidConfig, "argument: flags look like --key=value");
    }
    return set(normalised(body.substr(0, equals)), body.substr(equals + 1));
}

Result<void> DaemonConfig::apply_file_text(std::string_view text) {
    while (!text.empty()) {
        const std::size_t newline = text.find('\n');
        const std::string_view line = trim(text.substr(0, newline));
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::size_t equals = line.find('=');
        const std::string_view key =
            equals == std::string_view::npos ? line : trim(line.substr(0, equals));
        if (equals == std::string_view::npos || key.empty()) {
            return fail(ErrorCode::InvalidConfig, "config file: lines look like key = value");
        }
        if (const Result<void> done = set(normalised(key), trim(line.substr(equals + 1))); !done) {
            return done;
        }
    }
    return {};
}

Result<void> DaemonConfig::check_transport() {
    if (ipc_port_ && !socket_path_.empty()) {
        return fail(ErrorCode::InvalidConfig,
                    "config: set only one of ipc_socket (a Unix socket) and ipc_port (TCP)");
    }
    if (!ipc_port_ && socket_path_.empty()) {
        if (ipc_host_set_) {
            return fail(ErrorCode::InvalidConfig, "config: ipc_host needs ipc_port");
        }
        if (default_socket_.empty()) {
            return fail(ErrorCode::InvalidConfig, "config: ipc_socket or ipc_port is required");
        }
        socket_path_ = default_socket_;
    }
    if (ipc_host_set_ && !ipc_port_) {
        return fail(ErrorCode::InvalidConfig, "config: ipc_host needs ipc_port");
    }
    if (ipc_port_) {
        // No peer uid over TCP: the token is the only authentication, so it is not optional.
        if (token_file_.empty()) {
            return fail(ErrorCode::InvalidConfig,
                        "config: ipc_port needs ipc_token_file (over TCP the token is the only "
                        "authentication)");
        }
        const Result<Endpoint> bind = parse_endpoint(ipc_host_, *ipc_port_);
        if (!bind) {
            return fail(ErrorCode::InvalidConfig,
                        "config: ipc_host must be an IPv4 or IPv6 address literal, not a name");
        }
        ipc_endpoint_ = *bind;
    }
    return {};
}

Result<void> DaemonConfig::finish() {
    if (const Result<void> transport = check_transport(); !transport) {
        return transport;
    }
    // The endpoint and its secret come as a pair: a port without a token file would serve nobody,
    // and a token file without a port is most likely a mistyped key.
    if (embedded_.publish_requested() == publish_token_file_.empty()) {
        return fail(ErrorCode::InvalidConfig,
                    "config: publish_port and publish_token_file go together");
    }
    // The token protects the admin listener, so without a port it is most likely a mistyped key.
    if (!admin_token_file_.empty() && !embedded_.admin_requested()) {
        return fail(ErrorCode::InvalidConfig, "config: admin_token_file needs admin_port");
    }
    const std::size_t message = embedded_.server().session.assembler.max_message_size;
    if (explicit_max_frame_ != 0) {
        // The biggest message the WebSocket side accepts must fit one EVENT frame: otherwise the
        // daemon would receive messages it can never forward. Refuse at startup, not at runtime.
        // (message <= ceiling is not assumed: compare without adding.)
        if (explicit_max_frame_ < kEventOverhead ||
            message > explicit_max_frame_ - kEventOverhead) {
            return fail(ErrorCode::InvalidConfig,
                        "config: ipc_max_frame must be at least max_message_size + 18 bytes");
        }
        const std::size_t lowered = clamp_to_outbound(explicit_max_frame_);
        max_frame_lowered_ = lowered != explicit_max_frame_;
        max_frame_ = static_cast<std::uint32_t>(lowered);  // checked against the ceiling
        return {};
    }
    // Default: the biggest message plus headroom for the frame's own fields, so any message the
    // WebSocket side accepts fits in one IPC frame. Checked: a size near the ceiling must not wrap.
    if (message > ipc::kMaxFrameCeiling - kDefaultFrameHeadroom) {
        return fail(ErrorCode::InvalidConfig,
                    "config: max_message_size is too large to forward in one IPC frame");
    }
    max_frame_ = static_cast<std::uint32_t>(clamp_to_outbound(message + kDefaultFrameHeadroom));
    return {};
}

// ADR-0043. A SEND_TEXT or SEND_BINARY frame can carry at most what one connection's outbound queue
// can ever hold; a frame limit above that lets a worker send what the daemon would then drop (and
// answer OK to). The limit is lowered to that size plus the frame's own fields, but never below
// what the biggest inbound message needs (the check above).
std::size_t DaemonConfig::clamp_to_outbound(std::size_t frame) const noexcept {
    const std::size_t fits = embedded_.max_outbound_message() + kSendOverhead;
    const std::size_t inbound = embedded_.server().session.assembler.max_message_size;
    const std::size_t floor = inbound + kEventOverhead;
    return std::max(std::min(frame, fits), floor);
}

}  // namespace wsrv::wsrvd

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

}  // namespace

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
        const auto number = parse_number(value);
        if (!number || *number < 1 ||
            *number > static_cast<std::uint64_t>(kMaxHelloTimeout.count())) {
            return fail(ErrorCode::InvalidConfig,
                        "config: ipc_hello_timeout_ms must be a number from 1 to 60000");
        }
        hello_timeout_ = std::chrono::milliseconds{static_cast<std::int64_t>(*number)};
        return {};
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

Result<void> DaemonConfig::finish() {
    if (socket_path_.empty()) {
        return fail(ErrorCode::InvalidConfig, "config: ipc_socket is required");
    }
    if (explicit_max_frame_ != 0) {
        max_frame_ =
            static_cast<std::uint32_t>(explicit_max_frame_);  // checked against the ceiling
        return {};
    }
    // Default: the biggest message plus headroom for the frame's own fields, so any message the
    // WebSocket side accepts fits in one IPC frame. Checked: a size near the ceiling must not wrap.
    const std::size_t message = embedded_.server().session.assembler.max_message_size;
    if (message > ipc::kMaxFrameCeiling - kDefaultFrameHeadroom) {
        return fail(ErrorCode::InvalidConfig,
                    "config: max_message_size is too large to forward in one IPC frame");
    }
    max_frame_ = static_cast<std::uint32_t>(message + kDefaultFrameHeadroom);
    return {};
}

}  // namespace wsrv::wsrvd

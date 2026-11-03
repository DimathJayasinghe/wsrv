// SPDX-License-Identifier: Apache-2.0
#include "wsrvd/startup.hpp"

#include <fstream>
#include <ios>
#include <vector>

#include "wsrvd/token_file.hpp"

namespace wsrv::wsrvd {

namespace {

constexpr std::string_view kConfigFlag = "--config=";

/// Read a whole config file, refusing one larger than kMaxConfigFileSize (reads one byte past the
/// limit to tell "exactly at the limit" from "over it", never the whole file).
Result<std::string> read_config_file(const std::string& path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return fail(ErrorCode::InvalidConfig, "cannot open the --config file");
    }
    std::string text(kMaxConfigFileSize + 1, '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(file.gcount()));
    if (text.size() > kMaxConfigFileSize) {
        return fail(ErrorCode::InvalidConfig, "the --config file is larger than 64 KiB");
    }
    return text;
}

/// One argument: `--config=FILE` reads a file, anything else is a `--key=value` flag.
Result<void> apply(DaemonConfig& config, std::string_view arg) {
    if (!arg.starts_with(kConfigFlag)) {
        return config.apply_argument(arg);
    }
    const Result<std::string> text = read_config_file(std::string{arg.substr(kConfigFlag.size())});
    if (!text) {
        return std::unexpected{text.error()};
    }
    return config.apply_file_text(*text);
}

}  // namespace

Result<Startup> prepare(std::span<const std::string_view> args, ISyscalls& sys) {
    Startup startup;
    for (const std::string_view arg : args) {
        if (arg == "--help" || arg == "-h") {
            startup.help = true;
            return startup;
        }
        if (const Result<void> done = apply(startup.config, arg); !done) {
            return std::unexpected{done.error()};
        }
    }
    if (const Result<void> done = startup.config.finish(); !done) {
        return std::unexpected{done.error()};
    }
    if (!startup.config.token_file().empty()) {
        Result<std::string> token = read_token_file(sys, startup.config.token_file());
        if (!token) {
            return std::unexpected{token.error()};
        }
        startup.token = std::move(*token);
    }
    return startup;
}

std::string_view usage() noexcept {
    return "usage: wsrvd --ipc-socket=PATH [options]\n"
           "\n"
           "  --ipc-socket=PATH          Unix socket the application connects to (required)\n"
           "  --ipc-token-file=FILE      shared token; FILE must be mode 0600 and yours\n"
           "  --ipc-max-frame=BYTES      largest IPC frame (default: max_message_size + 64 KiB)\n"
           "  --ipc-hello-timeout-ms=MS  time allowed for HELLO (default 5000)\n"
           "  --config=FILE              read 'key = value' lines (later arguments override)\n"
           "  --listen-host=ADDR  --listen-port=PORT  --threads=N  --allow-origin=ORIGIN\n"
           "  --max-message-size=BYTES  --max-connections=N  --shutdown-grace-ms=MS\n"
           "\n"
           "The token is never accepted on the command line: it would show in `ps`.\n";
}

}  // namespace wsrv::wsrvd

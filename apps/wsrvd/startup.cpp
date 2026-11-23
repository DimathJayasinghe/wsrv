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
    if (!startup.config.publish_token_file().empty()) {
        // The same checks as the IPC token (a regular file, ours, mode 0600), plus the length floor
        // of set_publish_token(). The secret goes straight into the embedded config, never through
        // the key table.
        Result<std::string> token = read_token_file(sys, startup.config.publish_token_file());
        if (!token) {
            return std::unexpected{token.error()};
        }
        if (const Result<void> done = startup.config.set_publish_token(std::move(*token)); !done) {
            return std::unexpected{done.error()};
        }
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
           "  --ipc-stall-ms=MS          drop an application that reads nothing for MS (default "
           "10000)\n"
           "  --config=FILE              read 'key = value' lines (later arguments override)\n"
           "  --listen-host=ADDR  --listen-port=PORT  --threads=N  --allow-origin=ORIGIN\n"
           "  --max-message-size=BYTES  --max-connections=N  --shutdown-grace-ms=MS\n"
           "  --admin-port=PORT  --admin-host=ADDR (127.0.0.1)  serve /metrics, /healthz and\n"
           "                             /readyz over plain HTTP on this address; off unless\n"
           "                             a port is given; there is no authentication, so keep\n"
           "                             it on loopback or behind a network policy\n"
           "  --publish-port=PORT  --publish-host=ADDR (127.0.0.1)  --publish-token-file=FILE\n"
           "                             accept `POST /publish?topic=T` from other processes and\n"
           "                             send the body to the subscribers of T (needs\n"
           "                             --broker=true); FILE holds a token of at least 32 bytes,\n"
           "                             mode 0600; plain HTTP: keep it on loopback or a private\n"
           "                             network, or put TLS in front\n"
           "  --max-connections-per-ip=N (100)  --max-new-connections-per-ip-per-second=N (50)\n"
           "                             per client address (IPv6: per /64); over a limit the\n"
           "                             client gets 429; 0 = no limit (behind a proxy every user\n"
           "                             is one address: raise or switch off)\n"
           "  --permessage-deflate=true  compress messages (RFC 7692; off by default, costs memory\n"
           "                             per connection: see docs/OPERATIONS.md); tuning, only used\n"
           "                             with the switch on:\n"
           "  --deflate-server-max-window-bits=9..15 (12)  --deflate-level=1..9 (6)\n"
           "  --deflate-server-no-context-takeover=BOOL (true)  --deflate-min-size=BYTES (128)\n"
           "  --deflate-max-ratio=1..1000 (100)  inflated size limit, times the compressed size\n"
           "\n"
           "The token is never accepted on the command line: it would show in `ps`.\n";
}

}  // namespace wsrv::wsrvd

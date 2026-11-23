// SPDX-License-Identifier: Apache-2.0
#include "wsrvd/startup.hpp"

#include <algorithm>
#include <fstream>
#include <ios>
#include <vector>

#include "wsrvd/token_file.hpp"

namespace wsrv::wsrvd {

namespace {

constexpr std::string_view kConfigFlag = "--config=";
constexpr std::string_view kEnvironmentPrefix = "WSRVD_";
constexpr std::string_view kHealthcheckFlag = "--healthcheck";

/// The keys that may be given several times (ADR-0030 section 1): one variable carries a comma list.
[[nodiscard]] bool repeatable(std::string_view key) noexcept {
    return key == "allow_origin" || key == "handshake_forward_header" || key == "trusted_proxy" ||
           key == "tls_sni";
}

[[nodiscard]] std::string lower_ascii(std::string_view text) {
    std::string out{text};
    std::ranges::transform(out, out.begin(), [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return out;
}

[[nodiscard]] std::string_view trim_blanks(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

/// One variable: the key is the name after the prefix in lower case; a repeatable key takes a comma
/// list, one `set` per non-empty item.
Result<void> apply_variable(DaemonConfig& config, std::string_view name, std::string_view value) {
    const std::string key = lower_ascii(name.substr(kEnvironmentPrefix.size()));
    if (!repeatable(key)) {
        return config.set(key, value);
    }
    while (!value.empty()) {
        const std::size_t comma = value.find(',');
        const std::string_view item = trim_blanks(value.substr(0, comma));
        value = comma == std::string_view::npos ? std::string_view{} : value.substr(comma + 1);
        if (item.empty()) {
            continue;
        }
        if (const Result<void> done = config.set(key, item); !done) {
            return done;
        }
    }
    return {};
}

/// The environment layer (ADR-0030 section 1). Entries without the prefix, without an `=`, or
/// with an empty value are skipped; a prefixed one that is not a key is an error naming it.
Result<void> apply_environment(DaemonConfig& config,
                               std::span<const std::string_view> environment,
                               std::string* detail) {
    for (const std::string_view entry : environment) {
        const std::size_t equals = entry.find('=');
        if (!entry.starts_with(kEnvironmentPrefix) || equals == std::string_view::npos ||
            equals == kEnvironmentPrefix.size() || equals + 1 == entry.size()) {
            continue;
        }
        const std::string_view name = entry.substr(0, equals);
        if (const Result<void> done = apply_variable(config, name, entry.substr(equals + 1)); !done) {
            if (detail != nullptr) {
                *detail = std::string{name};
            }
            return done;
        }
    }
    return {};
}

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

[[nodiscard]] bool is_help(std::string_view arg) noexcept {
    return arg == "--help" || arg == "-h";
}

/// The arguments in order (`--config=FILE` and `--key=value`); `--healthcheck` is a mode.
Result<void> apply_arguments(Startup& startup, std::span<const std::string_view> args) {
    for (const std::string_view arg : args) {
        if (arg == kHealthcheckFlag) {
            startup.healthcheck = true;
        } else if (const Result<void> done = apply(startup.config, arg); !done) {
            return done;
        }
    }
    return {};
}

/// The IPC token and the publish token, each read from its file with the permission checks of
/// read_token_file. The publish token goes straight into the embedded config, never through the key
/// table, and has the length floor of set_publish_token().
Result<void> read_secrets(Startup& startup, ISyscalls& sys) {
    if (!startup.config.token_file().empty()) {
        Result<std::string> token = read_token_file(sys, startup.config.token_file());
        if (!token) {
            return std::unexpected{token.error()};
        }
        startup.token = std::move(*token);
    }
    if (!startup.config.publish_token_file().empty()) {
        Result<std::string> token = read_token_file(sys, startup.config.publish_token_file());
        if (!token) {
            return std::unexpected{token.error()};
        }
        return startup.config.set_publish_token(std::move(*token));
    }
    return {};
}

}  // namespace

Result<Startup> prepare(std::span<const std::string_view> args, ISyscalls& sys) {
    return prepare(args, {}, sys);
}

Result<Startup> prepare(std::span<const std::string_view> args,
                        std::span<const std::string_view> environment,
                        ISyscalls& sys,
                        std::string* detail) {
    Startup startup;
    if (std::ranges::any_of(args, is_help)) {
        startup.help = true;  // help wins over a bad variable or flag: it is how one finds out
        return startup;
    }
    if (const Result<void> done = apply_environment(startup.config, environment, detail); !done) {
        return std::unexpected{done.error()};
    }
    if (const Result<void> done = apply_arguments(startup, args); !done) {
        return std::unexpected{done.error()};
    }
    if (const Result<void> done = startup.config.finish(); !done) {
        return std::unexpected{done.error()};
    }
    if (startup.healthcheck) {
        return startup;  // it only looks at the sockets: no secret needs reading
    }
    if (const Result<void> done = read_secrets(startup, sys); !done) {
        return std::unexpected{done.error()};
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
           "  --healthcheck              probe the running daemon (GET /healthz on the admin port,\n"
           "                             else connect to the websocket port and the IPC socket);\n"
           "                             exit 0 = healthy, 1 = not; reads the same settings\n"
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
           "Every setting is also an environment variable: WSRVD_ and the key in upper case\n"
           "(WSRVD_LISTEN_PORT, WSRVD_ALLOW_ORIGIN=a,b for repeatable keys). The environment is\n"
           "the base layer; --config and flags override it. An unknown WSRVD_ variable is an\n"
           "error; an empty one counts as unset.\n"
           "\n"
           "The token is never accepted on the command line or in the environment: it would show\n"
           "in `ps` and /proc. Use ipc_token_file / publish_token_file.\n";
}

}  // namespace wsrv::wsrvd

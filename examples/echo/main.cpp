// SPDX-License-Identifier: Apache-2.0
// The smallest useful wsrv server: echoes every message back. It is built only on the public API
// (ServerBuilder + IWebSocketHandler), so it doubles as the Autobahn conformance target and as the
// backend of the browser demo (examples/demo).
//
//   echo [--host IP] [--port N] [--allow-origin URL]... [--allow-missing-origin] [--autobahn]
#include "server/server_builder.hpp"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

struct Options {
    std::string host = "127.0.0.1";  // loopback unless the operator opts into exposure
    std::uint16_t port = 9001;
    std::vector<std::string> allowed_origins;
    bool allow_missing_origin = false;  // non-browser clients (Autobahn, curl-like tools)
    bool autobahn = false;
};

constexpr std::string_view kUsage =
    "usage: echo [--host IP] [--port N] [--allow-origin URL]... [--allow-missing-origin] "
    "[--autobahn]\n"
    "  --allow-origin URL       accept browsers on this Origin (repeatable), e.g. "
    "http://localhost:8000\n"
    "  --allow-missing-origin   accept clients that send no Origin header\n"
    "  --autobahn               raise size and rate limits for the conformance suite (test use "
    "only)\n";

std::optional<std::uint16_t> parse_port(std::string_view text) {
    std::uint16_t port = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), port);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return port;
}

/// @return nullopt after printing the problem; the caller exits with an error status.
std::optional<Options> parse_args(const std::vector<std::string_view>& args) {
    Options options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        const bool has_value = i + 1 < args.size();
        if (arg == "--host" && has_value) {
            options.host = std::string{args[++i]};
        } else if (arg == "--port" && has_value) {
            const std::optional<std::uint16_t> port = parse_port(args[++i]);
            if (!port) {
                std::fputs("echo: --port needs a number from 0 to 65535\n", stderr);
                return std::nullopt;
            }
            options.port = *port;
        } else if (arg == "--allow-origin" && has_value) {
            options.allowed_origins.emplace_back(args[++i]);
        } else if (arg == "--allow-missing-origin") {
            options.allow_missing_origin = true;
        } else if (arg == "--autobahn") {
            options.autobahn = true;
        } else {
            std::fprintf(stderr,
                         "echo: unknown or incomplete option '%.*s'\n%.*s",
                         static_cast<int>(arg.size()),
                         arg.data(),
                         static_cast<int>(kUsage.size()),
                         kUsage.data());
            return std::nullopt;
        }
    }
    return options;
}

/// Sends every message back unchanged. Text arrives already validated as UTF-8 (FR-F7).
class EchoHandler final : public wsrv::IWebSocketHandler {
public:
    void on_text(wsrv::Session& session, std::string_view text) override {
        // A refused send (the connection is closing, or its queue is full) needs no extra
        // handling here: the server already closes a client that cannot keep up.
        static_cast<void>(session.send_text(text).has_value());
    }
    void on_binary(wsrv::Session& session, wsrv::ByteView data) override {
        static_cast<void>(session.send_binary(data).has_value());
    }
};

class EchoFactory final : public wsrv::IHandlerFactory {
public:
    std::unique_ptr<wsrv::IWebSocketHandler> create(const wsrv::ConnectionInfo& /*info*/) override {
        return std::make_unique<EchoHandler>();
    }
};

/// The conformance suite sends 16 MiB messages, tens of thousands of fragments and bursts of
/// frames that the production defaults refuse on purpose (FR-F6, FR-F10, EC-F31). Raise only
/// those limits, and only below the hard ceilings; every protocol check stays on.
/// The numbers and the cases they come from: tests/autobahn/EXCLUDED.md.
void raise_limits_for_autobahn(wsrv::ServerConfig& config) {
    wsrv::SessionConfig& session = config.session;
    session.parser.max_frame_payload = std::uint64_t{20} << 20;
    session.assembler.max_message_size = std::size_t{20} << 20;
    session.assembler.max_fragments = std::size_t{1} << 20;
    session.outbound.max_queued_bytes = std::size_t{64} << 20;
    session.max_data_frames_per_second = 2'000'000;
}

wsrv::ServerBuilder make_builder(const Options& options, EchoFactory& factory) {
    wsrv::ServerBuilder builder;
    builder.listen(options.host, options.port).handler_factory(factory);
    wsrv::ServerConfig& config = builder.config();
    config.handshake.allowed_origins = options.allowed_origins;
    config.handshake.allow_missing_origin = options.allow_missing_origin;
    // An echo must be able to send back every message it accepts (FR-F6): the outbound queue has
    // to hold a maximum-size message plus per-message overhead, so it is twice the message limit.
    config.session.outbound.max_queued_bytes = 2 * config.session.assembler.max_message_size;
    if (options.autobahn) {
        raise_limits_for_autobahn(config);
    }
    return builder;
}

int serve(const Options& options) {
    EchoFactory factory;
    try {
        const std::unique_ptr<wsrv::Server> server = make_builder(options, factory).build();
        if (const auto signals = server->install_signal_handlers(); !signals) {
            std::fputs("echo: cannot install signal handlers\n", stderr);
            return EXIT_FAILURE;
        }
        std::printf("echo: listening on %s:%u (Ctrl-C to stop)\n",
                    options.host.c_str(),
                    static_cast<unsigned>(server->local_endpoint().port));
        std::fflush(stdout);
        return server->run() ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const wsrv::ConfigError& error) {
        std::fprintf(stderr, "echo: %s\n", error.what());
        return EXIT_FAILURE;
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    const std::optional<Options> options = parse_args(args);
    return options ? serve(*options) : EXIT_FAILURE;
}

// SPDX-License-Identifier: Apache-2.0
// The smallest useful wsrv server: echoes every message back. It is built only on the public API
// (ServerBuilder + IWebSocketHandler), so it doubles as the Autobahn conformance target and as the
// backend of the browser demo (examples/demo).
//
//   echo [--host IP] [--port N] [--threads N] [--allow-origin URL]... [--allow-missing-origin]
//        [--tls-cert FILE --tls-key FILE] [--deflate] [--autobahn]
#include "base/logger.hpp"
#include "server/server_builder.hpp"

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(WSRV_HAS_TLS)
#include "tls/server_context.hpp"
#include "tls/tls_transport_wrapper.hpp"
#endif

namespace {

struct Options {
    std::string host = "127.0.0.1";  // loopback unless the operator opts into exposure
    std::uint16_t port = 9001;
    std::size_t threads = 1;
    std::vector<std::string> allowed_origins;
    bool allow_missing_origin = false;  // non-browser clients (Autobahn, curl-like tools)
    bool autobahn = false;
    bool deflate = false;  // offer permessage-deflate (RFC 7692, ADR-0020)
    std::string tls_cert;  // both set = serve wss:// (ADR-0019)
    std::string tls_key;
};

constexpr std::string_view kUsage =
    "usage: echo [--host IP] [--port N] [--threads N] [--allow-origin URL]... "
    "[--allow-missing-origin] [--tls-cert FILE --tls-key FILE] [--deflate] [--autobahn]\n"
    "  --threads N              event loops, one thread each (default 1)\n"
    "  --allow-origin URL       accept browsers on this Origin (repeatable), e.g. "
    "http://localhost:8000\n"
    "  --allow-missing-origin   accept clients that send no Origin header\n"
    "  --tls-cert FILE          serve wss://: PEM certificate chain (needs --tls-key)\n"
    "  --tls-key FILE           PEM private key, not readable by others\n"
    "  --deflate                offer permessage-deflate (RFC 7692); off by default\n"
    "  --autobahn               raise size, rate and per-IP limits for the conformance suite (test "
    "use only)\n";

template <class Number>
std::optional<Number> parse_number(std::string_view text) {
    Number value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

void report_unknown(std::string_view arg) {
    std::cerr << "echo: unknown or incomplete option '" << arg << "'\n" << kUsage;
}

/// An option that takes a value. @return false after printing the problem.
bool apply_valued_option(Options& options, std::string_view name, std::string_view value) {
    if (name == "--host") {
        options.host = std::string{value};
    } else if (name == "--tls-cert") {
        options.tls_cert = std::string{value};
    } else if (name == "--tls-key") {
        options.tls_key = std::string{value};
    } else if (name == "--allow-origin") {
        options.allowed_origins.emplace_back(value);
    } else if (name == "--port") {
        const std::optional<std::uint16_t> port = parse_number<std::uint16_t>(value);
        if (!port) {
            std::cerr << "echo: --port needs a number from 0 to 65535\n";
            return false;
        }
        options.port = *port;
    } else if (name == "--threads") {
        const std::optional<std::size_t> threads = parse_number<std::size_t>(value);
        if (!threads) {
            std::cerr << "echo: --threads needs a positive number\n";
            return false;
        }
        options.threads = *threads;
    } else {
        report_unknown(name);
        return false;
    }
    return true;
}

/// @return nullopt after printing the problem; the caller exits with an error status.
std::optional<Options> parse_args(const std::vector<std::string_view>& args) {
    Options options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--allow-missing-origin") {
            options.allow_missing_origin = true;
        } else if (args[i] == "--autobahn") {
            options.autobahn = true;
        } else if (args[i] == "--deflate") {
            options.deflate = true;
        } else if (i + 1 >= args.size()) {
            report_unknown(args[i]);
            return std::nullopt;
        } else if (!apply_valued_option(options, args[i], args[i + 1])) {
            return std::nullopt;
        } else {
            ++i;  // the value
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
    // The suite is one client address opening hundreds of connections in quick succession: the
    // per-IP limits (ADR-0021) would answer it with 429s. This server is for that test only.
    config.max_connections_per_ip = 0;
    config.max_new_connections_per_ip_per_second = 0;
}

wsrv::ServerBuilder make_builder(const Options& options, EchoFactory& factory) {
    wsrv::ServerBuilder builder;
    builder.listen(options.host, options.port).handler_factory(factory);
    wsrv::ServerConfig& config = builder.config();
    config.threads = options.threads;
    config.handshake.allowed_origins = options.allowed_origins;
    config.handshake.allow_missing_origin = options.allow_missing_origin;
    // An echo must be able to send back every message it accepts (FR-F6): the outbound queue has
    // to hold a maximum-size message plus per-message overhead, so it is twice the message limit.
    config.session.outbound.max_queued_bytes = 2 * config.session.assembler.max_message_size;
    if (options.autobahn) {
        raise_limits_for_autobahn(config);
    }
    if (options.deflate) {
        builder.permessage_deflate(wsrv::DeflateOptions{});  // library defaults, ratio included
    }
    return builder;
}

/// Prints what the TLS context reports (which file, and why): a server that cannot load its
/// certificate must say so on the terminal.
class StderrLogger final : public wsrv::ILogger {
public:
    using wsrv::ILogger::log;
    void log(const wsrv::LogRecord& record) override {
        std::cerr << "echo: " << record.message << '\n';
    }
};

int serve(const Options& options) {
    EchoFactory factory;
    wsrv::ServerBuilder builder = make_builder(options, factory);
#if defined(WSRV_HAS_TLS)
    StderrLogger logger;
    std::unique_ptr<wsrv::TlsServerContext> tls;
    std::unique_ptr<wsrv::TlsTransportWrapper> wrapper;
    if (!options.tls_cert.empty() || !options.tls_key.empty()) {
        auto loaded = wsrv::TlsServerContext::load({options.tls_cert, options.tls_key, {}}, logger);
        if (!loaded) {
            std::cerr << "echo: cannot load the TLS certificate and key\n";
            return EXIT_FAILURE;  // never fall back to plain ws:// (fail closed)
        }
        tls = std::move(*loaded);
        wrapper = std::make_unique<wsrv::TlsTransportWrapper>(*tls);
        builder.transport_wrapper(*wrapper);
    }
#else
    if (!options.tls_cert.empty() || !options.tls_key.empty()) {
        std::cerr << "echo: this build has no TLS (WSRV_ENABLE_TLS=OFF)\n";
        return EXIT_FAILURE;
    }
#endif
    try {
        const std::unique_ptr<wsrv::Server> server = builder.build();
        if (const auto signals = server->install_signal_handlers(); !signals) {
            std::cerr << "echo: cannot install signal handlers\n";
            return EXIT_FAILURE;
        }
        std::cout << "echo: listening (" << (options.tls_cert.empty() ? "ws" : "wss") << ") on "
                  << options.host << ':' << server->local_endpoint().port << " (Ctrl-C to stop)\n"
                  << std::flush;  // flushed: scripts wait for this line
        return server->run() ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const wsrv::ConfigError& error) {
        std::cerr << "echo: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::vector<std::string_view> args(argv + 1, argv + argc);
        const std::optional<Options> options = parse_args(args);
        return options ? serve(*options) : EXIT_FAILURE;
    } catch (const std::exception& error) {  // for example out of memory while starting up
        std::cerr << "echo: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

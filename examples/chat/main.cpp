// SPDX-License-Identifier: Apache-2.0
// Chat rooms on the pub/sub broker: the composition root. Handlers and the broker live in
// chat.hpp; this file only reads options, builds the server and runs it.
//
//   chat [--host IP] [--port N] [--threads N] [--allow-origin URL]... [--allow-missing-origin]
//        [--admin-port N]
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

#include "chat.hpp"

namespace {

struct Options {
    std::string host = "127.0.0.1";  // loopback unless the operator opts into exposure
    std::uint16_t port = 9002;
    std::size_t threads = 2;
    std::vector<std::string> allowed_origins;
    bool allow_missing_origin = false;
    std::optional<std::uint16_t> admin_port;  // /metrics on 127.0.0.1 when set
};

constexpr std::string_view kUsage =
    "usage: chat [--host IP] [--port N] [--threads N] [--allow-origin URL]... "
    "[--allow-missing-origin] [--admin-port N]\n"
    "  --allow-origin URL       accept browsers on this Origin (repeatable)\n"
    "  --allow-missing-origin   accept clients that send no Origin header\n"
    "  --admin-port N           serve /metrics, /healthz, /readyz on 127.0.0.1:N (no auth)\n";

template <class Number>
std::optional<Number> parse_number(std::string_view text) {
    Number value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

bool apply_valued_option(Options& options, std::string_view name, std::string_view value) {
    if (name == "--host") {
        options.host = std::string{value};
    } else if (name == "--allow-origin") {
        options.allowed_origins.emplace_back(value);
    } else if (name == "--port") {
        const auto port = parse_number<std::uint16_t>(value);
        if (!port) {
            std::cerr << "chat: --port needs a number from 0 to 65535\n";
            return false;
        }
        options.port = *port;
    } else if (name == "--admin-port") {
        const auto port = parse_number<std::uint16_t>(value);
        if (!port) {
            std::cerr << "chat: --admin-port needs a number from 0 to 65535\n";
            return false;
        }
        options.admin_port = port;
    } else if (name == "--threads") {
        const auto threads = parse_number<std::size_t>(value);
        if (!threads) {
            std::cerr << "chat: --threads needs a positive number\n";
            return false;
        }
        options.threads = *threads;
    } else {
        std::cerr << "chat: unknown option '" << name << "'\n" << kUsage;
        return false;
    }
    return true;
}

std::optional<Options> parse_args(const std::vector<std::string_view>& args) {
    Options options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--allow-missing-origin") {
            options.allow_missing_origin = true;
        } else if (i + 1 >= args.size()) {
            std::cerr << "chat: unknown or incomplete option '" << args[i] << "'\n" << kUsage;
            return std::nullopt;
        } else if (!apply_valued_option(options, args[i], args[i + 1])) {
            return std::nullopt;
        } else {
            ++i;  // the value
        }
    }
    return options;
}

int serve(const Options& options) {
    wsrv::Broker broker;
    chat::ChatFactory factory{broker};
    wsrv::ServerBuilder builder;
    builder.listen(options.host, options.port).handler_factory(factory);
    builder.config().threads = options.threads;
    builder.config().handshake.allowed_origins = options.allowed_origins;
    builder.config().handshake.allow_missing_origin = options.allow_missing_origin;
    if (options.admin_port) {
        builder.admin("127.0.0.1", *options.admin_port);
    }
    try {
        const std::unique_ptr<wsrv::Server> server = builder.build();
        broker.use_metrics(&server->shared_metrics());  // wsrv_broker_* on the metrics page
        if (const auto signals = server->install_signal_handlers(); !signals) {
            std::cerr << "chat: cannot install signal handlers\n";
            return EXIT_FAILURE;
        }
        std::cout << "chat: listening (ws) on " << options.host << ':'
                  << server->local_endpoint().port << " (Ctrl-C to stop)\n"
                  << std::flush;  // flushed: scripts wait for this line
        const bool ok = server->run().has_value();
        broker.use_metrics(nullptr);  // the server is about to be destroyed
        return ok ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const wsrv::ConfigError& error) {
        std::cerr << "chat: " << error.what() << '\n';
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
        std::cerr << "chat: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

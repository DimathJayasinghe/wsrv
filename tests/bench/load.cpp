// SPDX-License-Identifier: Apache-2.0
// wsrv_load: an in-process echo server plus blocking clients.
//
//   wsrv_load --threads 4 --clients 16 --connections 8 --seconds 10     scaling benchmark
//   wsrv_load --threads 4 --clients 16 --connections 8 --seconds 3600 --churn --sample 30   soak
//
// Throughput mode prints round trips per second and latency percentiles. Soak mode also closes and
// reopens connections all the time and samples descriptors and resident memory; it fails (exit 1)
// if either keeps growing. Client and server share the machine and the process, so the numbers
// are for comparing loop counts on one machine, not absolute capacity.
#include "server/server_builder.hpp"
#include "support/running_server.hpp"
#include "support/tcp_client.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using wsrv::Opcode;
using wsrv::test::TcpClient;

struct Options {
    std::size_t threads = 1;      // server loops
    std::size_t clients = 8;      // client threads
    std::size_t connections = 8;  // per client thread
    std::size_t seconds = 10;
    std::size_t size = 64;    // payload bytes
    std::size_t sample = 30;  // soak: seconds between samples
    bool churn = false;
};

class EchoHandler final : public wsrv::IWebSocketHandler {
public:
    void on_text(wsrv::Session& session, std::string_view text) override {
        static_cast<void>(session.send_text(text).has_value());
    }
};

class EchoFactory final : public wsrv::IHandlerFactory {
public:
    std::unique_ptr<wsrv::IWebSocketHandler> create(const wsrv::ConnectionInfo&) override {
        return std::make_unique<EchoHandler>();
    }
};

std::optional<std::size_t> number(std::string_view text) {
    std::size_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<Options> parse(const std::vector<std::string_view>& args) {
    Options options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--churn") {
            options.churn = true;
            continue;
        }
        std::size_t* field = args[i] == "--threads"       ? &options.threads
                             : args[i] == "--clients"     ? &options.clients
                             : args[i] == "--connections" ? &options.connections
                             : args[i] == "--seconds"     ? &options.seconds
                             : args[i] == "--size"        ? &options.size
                             : args[i] == "--sample"      ? &options.sample
                                                          : nullptr;
        const std::optional<std::size_t> value =
            (field != nullptr && i + 1 < args.size()) ? number(args[i + 1]) : std::nullopt;
        if (!value || *value == 0) {
            std::cerr << "wsrv_load: bad or unknown option '" << args[i] << "'\n";
            return std::nullopt;
        }
        *field = *value;
        ++i;
    }
    return options;
}

/// One client thread: K connections used in lockstep rounds (send on all, then read on all).
/// With churn, one connection is replaced every round.
struct ClientResult {
    std::uint64_t messages = 0;
    std::uint64_t failures = 0;
    std::vector<std::uint32_t> latency_us;
};

std::unique_ptr<TcpClient> open_connection(std::uint16_t port) {
    auto client = std::make_unique<TcpClient>(port);
    return client->valid() && client->upgrade() ? std::move(client) : nullptr;
}

void run_client(std::uint16_t port,
                const Options& options,
                Clock::time_point deadline,
                ClientResult& result) {
    std::vector<std::unique_ptr<TcpClient>> conns;
    for (std::size_t i = 0; i < options.connections; ++i) {
        conns.push_back(open_connection(port));
        if (!conns.back()) {
            ++result.failures;
            return;
        }
    }
    const std::string payload(options.size, 'p');
    const std::size_t latency_cap = options.churn ? 50000 : 1000000;  // samples per thread
    std::vector<Clock::time_point> sent(conns.size());
    std::size_t round = 0;
    while (Clock::now() < deadline) {
        for (std::size_t i = 0; i < conns.size(); ++i) {
            sent[i] = Clock::now();
            if (!conns[i]->send_frame(Opcode::Text, payload)) {
                ++result.failures;
                return;
            }
        }
        for (std::size_t i = 0; i < conns.size(); ++i) {
            const auto frame = conns[i]->read_frame();
            if (!frame || frame->payload != payload) {
                ++result.failures;
                return;
            }
            ++result.messages;
            if (result.latency_us.size() >= latency_cap) {
                continue;  // enough samples; in a soak, growth here would hide a real leak
            }
            result.latency_us.push_back(static_cast<std::uint32_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - sent[i])
                    .count()));
        }
        if (options.churn) {
            conns[round % conns.size()] = open_connection(port);  // the old one closes here
            if (!conns[round % conns.size()]) {
                ++result.failures;
                return;
            }
            ++round;
        }
    }
}

std::size_t open_descriptors() {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        static_cast<void>(entry);
        ++count;
    }
    return count;
}

/// Resident set size in KiB (second field of statm is in pages).
std::size_t resident_kib() {
    std::ifstream statm("/proc/self/statm");
    std::size_t size = 0;
    std::size_t resident = 0;
    statm >> size >> resident;
    return resident * 4;  // 4 KiB pages on the supported platforms
}

std::uint32_t percentile(std::vector<std::uint32_t>& values, double fraction) {
    if (values.empty()) {
        return 0;
    }
    const auto index = static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1));
    std::nth_element(
        values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
    return values[index];
}

/// Soak check: after a warm-up sample, descriptors must stay within the churn slack and memory
/// within 25 % + 16 MiB of the warm-up value. @return false (and say why) otherwise.
struct SoakWatch {
    std::size_t fd_baseline = 0;
    std::size_t rss_baseline = 0;
    std::size_t fd_peak = 0;
    std::size_t rss_peak = 0;
    bool warmed = false;

    void sample(std::size_t fds, std::size_t rss) {
        if (!warmed) {
            fd_baseline = fds;
            rss_baseline = rss;
            warmed = true;
        }
        fd_peak = std::max(fd_peak, fds);
        rss_peak = std::max(rss_peak, rss);
    }
    [[nodiscard]] bool healthy(std::size_t slack_fds) const {
        return fd_peak <= fd_baseline + slack_fds &&
               rss_peak <= rss_baseline + rss_baseline / 4 + 16384;
    }
};

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    const std::optional<Options> parsed = parse(args);
    if (!parsed) {
        return EXIT_FAILURE;
    }
    const Options options = *parsed;

    EchoFactory factory;
    wsrv::ServerBuilder builder;
    builder.listen("127.0.0.1", 0).handler_factory(factory);
    builder.config().threads = options.threads;
    builder.config().max_connections = options.clients * options.connections * 2 + 64;
    builder.config().handshake.allowed_origins = {"https://app.example.com"};
    wsrv::test::RunningServer running{builder};

    const auto deadline = Clock::now() + std::chrono::seconds{options.seconds};
    std::vector<ClientResult> results(options.clients);
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < options.clients; ++i) {
        threads.emplace_back([&, i] { run_client(running.port(), options, deadline, results[i]); });
    }

    SoakWatch watch;
    const std::size_t slack = options.clients * 4 + 32;  // churn closes and opens in flight
    if (options.churn) {
        while (Clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::seconds{options.sample});
            const std::size_t fds = open_descriptors();
            const std::size_t rss = resident_kib();
            watch.sample(fds, rss);
            std::cout << "sample: fds=" << fds << " rss_kib=" << rss << std::endl;
        }
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    std::uint64_t messages = 0;
    std::uint64_t failures = 0;
    std::vector<std::uint32_t> latency;
    for (ClientResult& result : results) {
        messages += result.messages;
        failures += result.failures;
        latency.insert(latency.end(), result.latency_us.begin(), result.latency_us.end());
    }
    std::cout << std::fixed << std::setprecision(0) << "loops=" << options.threads
              << " clients=" << options.clients
              << " connections=" << options.clients * options.connections
              << " size=" << options.size << " seconds=" << options.seconds << " msgs_per_s="
              << static_cast<double>(messages) / static_cast<double>(options.seconds)
              << " p50_us=" << percentile(latency, 0.50) << " p99_us=" << percentile(latency, 0.99)
              << " failures=" << failures << '\n';
    if (options.churn && !watch.healthy(slack)) {
        std::cout << "SOAK FAILED: fds peak " << watch.fd_peak << " (baseline " << watch.fd_baseline
                  << "), rss peak " << watch.rss_peak << " KiB (baseline " << watch.rss_baseline
                  << ")\n";
        return EXIT_FAILURE;
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

// SPDX-License-Identifier: Apache-2.0
// wsrv_fanout: what one publish costs as the number of subscribers grows (ADR-0024).
//
//   wsrv_fanout --threads 4 --subscribers 1000 --rounds 2000 --size 128
//
// An in-process server with a Broker; N subscriber connections, read by a few blocking client
// threads. The main thread publishes one message, waits until every subscriber has read it, and
// repeats (lockstep), so each round time is "publish until the last subscriber has the message".
// Prints rounds per second, deliveries per second, round-time percentiles and the broker's own
// accepted / refused counts. Client and server share the machine: compare runs, do not size from
// them.
#include "server/broker.hpp"
#include "server/server_builder.hpp"
#include "support/running_server.hpp"
#include "support/tcp_client.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
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
using wsrv::test::TcpClient;

struct Options {
    std::size_t threads = 1;  // server loops
    std::size_t subscribers = 100;
    std::size_t rounds = 1000;
    std::size_t size = 128;   // payload bytes
    std::size_t readers = 8;  // client threads reading subscriber connections
};

/// Subscribes every connection to "bench" and nothing else.
class SubscriberHandler final : public wsrv::IWebSocketHandler {
public:
    explicit SubscriberHandler(wsrv::Broker& broker) : membership_{broker} {}
    void on_open(wsrv::Session& session) override {
        static_cast<void>(membership_.subscribe(session.handle(), "bench").has_value());
    }

private:
    wsrv::BrokerMembership membership_;
};

class SubscriberFactory final : public wsrv::IHandlerFactory {
public:
    explicit SubscriberFactory(wsrv::Broker& broker) : broker_{broker} {}
    std::unique_ptr<wsrv::IWebSocketHandler> create(const wsrv::ConnectionInfo&) override {
        return std::make_unique<SubscriberHandler>(broker_);
    }

private:
    wsrv::Broker& broker_;
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
    for (std::size_t i = 0; i < args.size(); i += 2) {
        std::size_t* field = args[i] == "--threads"       ? &options.threads
                             : args[i] == "--subscribers" ? &options.subscribers
                             : args[i] == "--rounds"      ? &options.rounds
                             : args[i] == "--size"        ? &options.size
                             : args[i] == "--readers"     ? &options.readers
                                                          : nullptr;
        const std::optional<std::size_t> value =
            (field != nullptr && i + 1 < args.size()) ? number(args[i + 1]) : std::nullopt;
        if (!value || *value == 0) {
            std::cerr << "wsrv_fanout: bad or unknown option '" << args[i] << "'\n";
            return std::nullopt;
        }
        *field = *value;
    }
    return options;
}

std::uint64_t percentile(std::vector<std::uint64_t>& values, double fraction) {
    if (values.empty()) {
        return 0;
    }
    const auto index = static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1));
    std::nth_element(
        values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
    return values[index];
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    const std::optional<Options> parsed = parse(args);
    if (!parsed) {
        return EXIT_FAILURE;
    }
    const Options options = *parsed;

    wsrv::BrokerConfig broker_config;
    broker_config.max_subscribers_per_topic = options.subscribers + 1;
    broker_config.max_subscriptions = options.subscribers + 1;
    wsrv::Broker broker{broker_config};
    SubscriberFactory factory{broker};
    wsrv::ServerBuilder builder;
    builder.listen("127.0.0.1", 0).handler_factory(factory);
    builder.config().threads = options.threads;
    builder.config().max_connections = options.subscribers + 64;
    builder.config().max_connections_per_ip = 0;
    builder.config().max_new_connections_per_ip_per_second = 0;
    builder.config().handshake.allowed_origins = {"https://app.example.com"};
    wsrv::test::RunningServer running{builder};
    broker.use_metrics(running.server().shared_metrics());

    std::vector<std::unique_ptr<TcpClient>> clients;
    for (std::size_t i = 0; i < options.subscribers; ++i) {
        clients.push_back(std::make_unique<TcpClient>(running.port()));
        if (!clients.back()->valid() || !clients.back()->upgrade()) {
            std::cerr << "wsrv_fanout: connect " << i << " failed\n";
            return EXIT_FAILURE;
        }
    }
    while (broker.subscriber_count("bench") < options.subscribers) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }

    // Each reader thread owns a slice of the connections and reads one frame from each per round.
    std::atomic<std::uint64_t> round_published{0};
    std::atomic<std::uint64_t> received{0};
    std::atomic<std::uint64_t> failures{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> readers;
    const std::size_t reader_count = std::min(options.readers, options.subscribers);
    for (std::size_t r = 0; r < reader_count; ++r) {
        readers.emplace_back([&, r] {
            std::uint64_t seen = 0;
            while (!stop.load()) {
                while (round_published.load() <= seen && !stop.load()) {
                    std::this_thread::yield();
                }
                if (stop.load()) {
                    return;
                }
                ++seen;
                for (std::size_t i = r; i < clients.size(); i += reader_count) {
                    const auto frame = clients[i]->read_frame();
                    if (!frame) {
                        failures.fetch_add(1);
                    }
                    received.fetch_add(1);
                }
            }
        });
    }

    const std::string payload(options.size, 'f');
    std::vector<std::uint64_t> round_us;
    round_us.reserve(options.rounds);
    std::uint64_t accepted = 0;
    std::uint64_t refused = 0;
    const auto started = Clock::now();
    for (std::size_t round = 0; round < options.rounds; ++round) {
        const auto t0 = Clock::now();
        const auto report = broker.publish_text("bench", payload);
        if (!report) {
            failures.fetch_add(1);
            break;
        }
        accepted += report->accepted;
        refused += report->refused;
        round_published.fetch_add(1);
        const std::uint64_t want = (round + 1) * options.subscribers;
        while (received.load() < want && failures.load() == 0) {
            std::this_thread::yield();
        }
        round_us.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count()));
        if (failures.load() != 0) {
            break;
        }
    }
    const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
    stop.store(true);
    for (std::thread& reader : readers) {
        reader.join();
    }

    const double rounds_done = static_cast<double>(round_us.size());
    std::cout << std::fixed << std::setprecision(0) << "loops=" << options.threads
              << " subscribers=" << options.subscribers << " size=" << options.size
              << " rounds=" << round_us.size() << " rounds_per_s=" << rounds_done / seconds
              << " deliveries_per_s="
              << rounds_done * static_cast<double>(options.subscribers) / seconds
              << " round_p50_us=" << percentile(round_us, 0.50)
              << " round_p99_us=" << percentile(round_us, 0.99) << " accepted=" << accepted
              << " refused=" << refused << " failures=" << failures.load() << '\n';
    return failures.load() == 0 && refused == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

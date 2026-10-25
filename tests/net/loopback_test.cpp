// SPDX-License-Identifier: Apache-2.0
#include "net/acceptor.hpp"
#include "net/plain_transport.hpp"
#include "net/tcp_listener.hpp"
#include "support/fake_syscalls.hpp"
#include "support/fakes.hpp"
#include "support/net_helpers.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

/// An edge-triggered echo connection: the discipline every M6 connection will follow. It reads
/// until WouldBlock, queues what it read, writes until WouldBlock, and asks for Write readiness
/// only while data is queued (EC-N02).
class EchoConnection final : public wsrv::IEventHandler {
public:
    EchoConnection(wsrv::EventLoop& loop, wsrv::Fd fd, wsrv::ISyscalls& sys)
        : loop_{loop}, transport_{std::move(fd), sys} {}

    void start() {
        const auto watch = loop_.watch(transport_.fd(), wsrv::PollEvents::Read | wsrv::PollEvents::Write, *this);
        ASSERT_TRUE(watch.has_value());
        watch_ = *watch;
    }

    void on_event(wsrv::PollEvents events) noexcept override {
        if (wsrv::has(events, wsrv::PollEvents::Read) || wsrv::has(events, wsrv::PollEvents::Hangup)) {
            read_all();
        }
        flush();
        if (finished_ && pending_.empty()) {
            transport_.shutdown_write();
        }
    }

    [[nodiscard]] std::size_t echoed() const { return echoed_; }

private:
    void read_all() {
        std::array<std::byte, 16384> buf{};
        for (;;) {
            const auto n = transport_.read(buf);
            if (!n.has_value()) {
                return;  // WouldBlock (or an error): wait for the next edge
            }
            if (*n == 0) {
                finished_ = true;
                return;
            }
            pending_.insert(pending_.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n));
        }
    }
    void flush() {
        while (!pending_.empty()) {
            std::vector<std::byte> chunk(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(pending_.size(), 65536)));
            const auto n = transport_.write(chunk);
            if (!n.has_value()) {
                return;  // WouldBlock: the Write edge will call us again
            }
            pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(*n));
            echoed_ += *n;
        }
    }

    wsrv::EventLoop& loop_;
    wsrv::PlainTransport transport_;
    wsrv::WatchHandle watch_{};
    std::deque<std::byte> pending_;
    std::size_t echoed_ = 0;
    bool finished_ = false;
};

class LoopbackTest : public ::testing::TestWithParam<std::size_t> {};

TEST_P(LoopbackTest, EverythingTheClientSendsComesBackIntact) {
    wsrv::test::FakeSyscalls sys;
    wsrv::test::FakeClock clock;
    auto loop = wsrv::EventLoop::create(sys, clock);
    ASSERT_TRUE(loop.has_value());
    wsrv::ListenerConfig config;
    config.endpoint = *wsrv::parse_endpoint("127.0.0.1", 0);
    auto listener = wsrv::TcpListener::open(sys, config);
    ASSERT_TRUE(listener.has_value());

    std::vector<std::unique_ptr<EchoConnection>> connections;
    wsrv::Acceptor acceptor{**loop, *listener, sys,
                            [&](wsrv::Fd fd, wsrv::Endpoint) {
                                connections.push_back(
                                    std::make_unique<EchoConnection>(**loop, std::move(fd), sys));
                                connections.back()->start();
                            },
                            {}};
    ASSERT_TRUE(acceptor.start().has_value());

    const std::size_t size = GetParam();
    std::string payload(size, '\0');
    std::mt19937 rng{static_cast<std::mt19937::result_type>(size)};
    std::generate(payload.begin(), payload.end(), [&] { return static_cast<char>(rng()); });

    std::string received;
    std::atomic<bool> client_done{false};
    std::thread client{[&] {
        wsrv::Fd fd = wsrv::test::connect_to(listener->local_endpoint());
        ASSERT_TRUE(fd.valid());
        std::thread writer{[&] {  // write and read concurrently: a full pipe in one direction must not deadlock
            std::size_t sent = 0;
            while (sent < payload.size()) {
                const ssize_t n = ::send(fd.get(), payload.data() + sent, payload.size() - sent, MSG_NOSIGNAL);
                ASSERT_GT(n, 0);
                sent += static_cast<std::size_t>(n);
            }
            ::shutdown(fd.get(), SHUT_WR);
        }};
        std::array<char, 65536> buf{};
        for (;;) {
            const ssize_t n = ::read(fd.get(), buf.data(), buf.size());
            if (n <= 0) {
                break;
            }
            received.append(buf.data(), static_cast<std::size_t>(n));
        }
        writer.join();
        client_done = true;
    }};

    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (!client_done && std::chrono::steady_clock::now() < deadline) {
        ASSERT_TRUE((*loop)->run_once(5ms).has_value());
    }
    client.join();
    EXPECT_TRUE(client_done);
    EXPECT_EQ(received.size(), payload.size());
    EXPECT_TRUE(received == payload);
}

INSTANTIATE_TEST_SUITE_P(Sizes, LoopbackTest, ::testing::Values(std::size_t{1}, std::size_t{65536}, std::size_t{4U << 20}));

}  // namespace

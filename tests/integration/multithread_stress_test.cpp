// SPDX-License-Identifier: Apache-2.0
// Four loops under pressure (ADR-0014): clients connect, echo and disconnect in a loop while
// other threads push through SessionHandles, some of them dead; then the server shuts down in the
// middle of it. The assertions are modest on purpose (no hang, no crash, every echo returns); the
// value is running this under ThreadSanitizer and AddressSanitizer: a race or a use-after-free
// in the hand-off, the handle link or the shutdown shows up here.
#include "server/server_builder.hpp"
#include "server/session_handle.hpp"
#include "support/running_server.hpp"
#include "support/tcp_client.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using wsrv::Opcode;
using wsrv::Session;
using wsrv::test::RunningServer;
using wsrv::test::TcpClient;

/// Echoes; keeps every handle it was ever given (dead ones included).
class StressFactory final : public wsrv::IHandlerFactory {
public:
    std::unique_ptr<wsrv::IWebSocketHandler> create(const wsrv::ConnectionInfo&) override {
        return std::make_unique<Handler>(*this);
    }
    [[nodiscard]] std::vector<wsrv::SessionHandle> snapshot() {
        const std::lock_guard lock{mutex_};
        return handles_;
    }

private:
    class Handler final : public wsrv::IWebSocketHandler {
    public:
        explicit Handler(StressFactory& owner) : owner_{owner} {}
        void on_open(Session& session) override {
            const std::lock_guard lock{owner_.mutex_};
            owner_.handles_.push_back(session.handle());
        }
        void on_text(Session& session, std::string_view text) override {
            static_cast<void>(session.send_text(text).has_value());
        }

    private:
        StressFactory& owner_;
    };

    std::mutex mutex_;
    std::vector<wsrv::SessionHandle> handles_;
};

/// Connect, echo a few messages, disconnect, repeat until told to stop. Pushed messages from the
/// handle threads arrive in between and are skipped. A frame that is neither Text nor Close
/// counts in `broken`.
void churn_client(std::uint16_t port, const std::atomic<bool>& stop, std::atomic<int>& broken) {
    while (!stop.load()) {
        TcpClient client{port};
        if (!client.valid() || !client.upgrade()) {
            return;  // refused or shut down: the server is going away
        }
        for (int i = 0; i < 5 && !stop.load(); ++i) {
            const std::string text = "echo-" + std::to_string(i);
            if (!client.send_frame(Opcode::Text, text)) {
                return;
            }
            for (;;) {
                const auto frame = client.read_frame();
                if (!frame) {
                    return;  // EOF: the server is shutting down (or the read timed out)
                }
                if (frame->opcode == Opcode::Close) {
                    return;
                }
                if (frame->opcode == Opcode::Text && frame->payload == text) {
                    break;
                }
                if (frame->opcode != Opcode::Text) {
                    broken.fetch_add(1);  // the server only ever sends Text and Close here
                    return;
                }
            }
        }
    }
}

void push_through_handles(StressFactory& factory, const std::atomic<bool>& stop) {
    std::size_t round = 0;
    while (!stop.load()) {
        const std::vector<wsrv::SessionHandle> handles = factory.snapshot();
        for (const wsrv::SessionHandle& handle : handles) {
            static_cast<void>(handle.send_text("push").has_value());  // dead handles: dropped
            if ((++round % 50) == 0) {
                static_cast<void>(
                    handle.close(wsrv::CloseCode::Normal, "pushed close").has_value());
            }
        }
        std::this_thread::sleep_for(1ms);
    }
}

TEST(MultiThreadStress, ChurnPushesAndAShutdownInTheMiddleOfTraffic) {
    StressFactory factory;
    wsrv::ServerBuilder builder;
    builder.listen("127.0.0.1", 0).handler_factory(factory);
    builder.config().threads = 4;
    builder.config().max_connections = 256;
    builder.config().max_connections_per_ip = 0;  // one address churns on purpose (ADR-0021)
    builder.config().max_new_connections_per_ip_per_second = 0;
    builder.config().shutdown_grace = 1s;
    builder.config().handshake.allowed_origins = {"https://app.example.com"};
    RunningServer running{builder};

    std::atomic<bool> stop{false};
    std::atomic<int> broken{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&] { churn_client(running.port(), stop, broken); });
    }
    for (int i = 0; i < 2; ++i) {
        threads.emplace_back([&] { push_through_handles(factory, stop); });
    }

    std::this_thread::sleep_for(1500ms);  // steady state: connections come and go on every loop
    ASSERT_TRUE(running.server().request_shutdown().has_value());  // ...and then the plug is pulled
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (!running.finished() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_TRUE(running.finished());
    // A client that connected during the shutdown waits in the kernel backlog (the listener stays
    // open until the Server object is destroyed) and gives up at its 5 s read timeout: that is why
    // a run can take 6.5 s instead of 1.5 s. Not a server fault.
    stop.store(true);
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(broken.load(), 0);
}

}  // namespace

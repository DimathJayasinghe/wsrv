// SPDX-License-Identifier: Apache-2.0
// The server with several loops (ADR-0014): connections are spread over the loops, the connection
// cap and ids are global, SessionHandle reaches a connection on another loop, and shutdown, forced
// shutdown and stop() reach every loop. Run under TSan: this is where cross-thread bugs live.
#include "server/server_builder.hpp"
#include "server/session_handle.hpp"
#include "support/running_server.hpp"
#include "support/tcp_client.hpp"
#include "support/wire.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using wsrv::Opcode;
using wsrv::Session;
using wsrv::test::RunningServer;
using wsrv::test::TcpClient;

template <class Condition>
bool eventually(Condition&& condition, std::chrono::milliseconds limit = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return condition();
}

/// Echoes, and records which thread and which id each connection got, and its handle.
class RecordingFactory final : public wsrv::IHandlerFactory {
public:
    std::unique_ptr<wsrv::IWebSocketHandler> create(const wsrv::ConnectionInfo& info) override {
        return std::make_unique<Handler>(*this, info.id);
    }

    [[nodiscard]] std::size_t distinct_threads() {
        const std::lock_guard lock{mutex_};
        return threads_.size();
    }
    [[nodiscard]] std::size_t distinct_ids() {
        const std::lock_guard lock{mutex_};
        return ids_.size();
    }
    [[nodiscard]] std::vector<wsrv::SessionHandle> handles() {
        const std::lock_guard lock{mutex_};
        return handles_;
    }

private:
    class Handler final : public wsrv::IWebSocketHandler {
    public:
        Handler(RecordingFactory& owner, wsrv::ConnectionId id) : owner_{owner}, id_{id} {}
        void on_open(Session& session) override {
            const std::lock_guard lock{owner_.mutex_};
            owner_.threads_.insert(std::this_thread::get_id());
            owner_.ids_.insert(id_);
            owner_.handles_.push_back(session.handle());
        }
        void on_text(Session& session, std::string_view text) override {
            static_cast<void>(session.send_text(text).has_value());
        }

    private:
        RecordingFactory& owner_;
        wsrv::ConnectionId id_;
    };

    std::mutex mutex_;
    std::set<std::thread::id> threads_;
    std::set<wsrv::ConnectionId> ids_;
    std::vector<wsrv::SessionHandle> handles_;
};

/// Counts what the host would see: one Open and, owed for every Open, exactly one Close.
class CountingFactory final : public wsrv::IHandlerFactory {
public:
    std::unique_ptr<wsrv::IWebSocketHandler> create(const wsrv::ConnectionInfo& /*info*/) override {
        return std::make_unique<Handler>(*this);
    }
    [[nodiscard]] std::size_t opened() const {
        return opened_.load();
    }
    [[nodiscard]] std::size_t closed() const {
        return closed_.load();
    }

private:
    class Handler final : public wsrv::IWebSocketHandler {
    public:
        explicit Handler(CountingFactory& owner) : owner_{owner} {}
        void on_open(Session& /*session*/) override {
            owner_.opened_.fetch_add(1);
        }
        void on_close(Session& /*session*/, const wsrv::CloseInfo& /*info*/) override {
            owner_.closed_.fetch_add(1);
        }

    private:
        CountingFactory& owner_;
    };

    std::atomic<std::size_t> opened_{0};
    std::atomic<std::size_t> closed_{0};
};

wsrv::ServerBuilder builder_for(wsrv::IHandlerFactory& factory, std::size_t threads) {
    wsrv::ServerBuilder builder;
    builder.listen("127.0.0.1", 0).handler_factory(factory);
    builder.config().threads = threads;
    builder.config().handshake.allowed_origins = {"https://app.example.com"};
    return builder;
}

std::vector<std::unique_ptr<TcpClient>> connect_clients(RunningServer& running, std::size_t count) {
    std::vector<std::unique_ptr<TcpClient>> clients;
    for (std::size_t i = 0; i < count; ++i) {
        clients.push_back(std::make_unique<TcpClient>(running.port()));
        EXPECT_TRUE(clients.back()->upgrade()) << i;
    }
    return clients;
}

TEST(MultiLoop, ConnectionsAreSpreadOverEveryLoopAndKeepUniqueIds) {  // ADR-0014 sections 1, 3
    RecordingFactory factory;
    RunningServer running{builder_for(factory, 4)};
    auto clients = connect_clients(running, 16);
    for (std::size_t i = 0; i < clients.size(); ++i) {
        ASSERT_TRUE(clients[i]->send_frame(Opcode::Text, "hi " + std::to_string(i)));
        const auto reply = clients[i]->read_frame();
        ASSERT_TRUE(reply.has_value()) << i;
        EXPECT_EQ(reply->payload, "hi " + std::to_string(i));
    }
    EXPECT_EQ(factory.distinct_threads(), 4U);  // round-robin: 16 connections over 4 loops
    EXPECT_EQ(factory.distinct_ids(), 16U);
}

TEST(MultiLoop, TheConnectionCapIsGlobalAndTheSlotComesBack) {  // FR-S1
    RecordingFactory factory;
    wsrv::ServerBuilder builder = builder_for(factory, 3);
    builder.config().max_connections = 6;
    RunningServer running{builder};
    auto clients = connect_clients(running, 6);
    TcpClient extra{running.port()};
    EXPECT_TRUE(extra.wait_for_eof());  // refused without a handshake
    EXPECT_EQ(running.server().rejected_connections(), 1U);
    clients.front()->close();  // its loop erases it and gives the slot back
    EXPECT_TRUE(eventually([&] {
        TcpClient again{running.port()};
        return again.upgrade();
    }));
}

TEST(MultiLoop, AHandleReachesConnectionsOnEveryLoop) {  // FR-S3 across loops
    RecordingFactory factory;
    RunningServer running{builder_for(factory, 3)};
    auto clients = connect_clients(running, 6);
    ASSERT_TRUE(eventually([&] { return factory.handles().size() == 6; }));
    std::thread pusher([&] {
        for (const wsrv::SessionHandle& handle : factory.handles()) {
            EXPECT_TRUE(handle.send_text("push").has_value());
        }
    });
    pusher.join();
    for (const auto& client : clients) {
        const auto frame = client->read_frame();
        ASSERT_TRUE(frame.has_value());
        EXPECT_EQ(frame->payload, "push");
    }
}

TEST(MultiLoop, AGracefulShutdownReachesEveryLoopAndEndsOnceAllAnswered) {  // EC-C18
    RecordingFactory factory;
    RunningServer running{builder_for(factory, 4)};
    auto clients = connect_clients(running, 8);
    ASSERT_TRUE(running.server().request_shutdown().has_value());
    for (const auto& client : clients) {
        const auto close = client->read_frame();
        ASSERT_TRUE(close.has_value());
        EXPECT_EQ(close->opcode, Opcode::Close);
        EXPECT_EQ(close->payload.substr(0, 2), std::string("\x03\xe9", 2));
    }
    EXPECT_FALSE(running.finished());  // nobody answered yet: the server waits
    for (const auto& client : clients) {
        ASSERT_TRUE(client->send_frame(Opcode::Close, wsrv::test::close_payload(1001)));
    }
    for (const auto& client : clients) {
        EXPECT_TRUE(client->wait_for_eof());
    }
    EXPECT_TRUE(eventually([&] { return running.finished(); }));
}

TEST(MultiLoop, ClientsThatIgnoreTheShutdownAreDroppedOnEveryLoopAfterTheGrace) {
    RecordingFactory factory;
    wsrv::ServerBuilder builder = builder_for(factory, 3);
    builder.config().shutdown_grace = 300ms;
    RunningServer running{builder};
    auto clients = connect_clients(running, 6);
    ASSERT_TRUE(running.server().request_shutdown().has_value());
    for (const auto& client : clients) {
        ASSERT_TRUE(client->read_frame().has_value());  // the 1001, ignored
    }
    for (const auto& client : clients) {
        EXPECT_TRUE(client->wait_for_eof());
    }
    EXPECT_TRUE(eventually([&] { return running.finished(); }));
}

TEST(MultiLoop, StopEndsEveryLoopEvenWithClientsConnected) {
    RecordingFactory factory;
    auto running = std::make_unique<RunningServer>(builder_for(factory, 4));
    auto clients = connect_clients(*running, 8);
    running.reset();  // stop(), then join: must not hang
    for (const auto& client : clients) {
        EXPECT_TRUE(client->wait_for_eof());
    }
}

// FR-S2 / wsrv.h: every Open is followed by exactly one Close, also when the server is cut short.
// The reviewer's claim (read from the code, never run): a forced shutdown reports the Close of the
// connections on loop 0 only; the other loops destroy theirs silently.
TEST(MultiLoop, AForcedShutdownReportsTheCloseOfEveryConnectionOnEveryLoop) {
    CountingFactory factory;
    wsrv::ServerBuilder builder = builder_for(factory, 3);
    builder.config().shutdown_grace = 100ms;
    RunningServer running{builder};
    auto clients = connect_clients(running, 6);  // two per loop, round-robin
    ASSERT_TRUE(eventually([&] { return factory.opened() == 6; }));
    ASSERT_TRUE(running.server().request_shutdown().has_value());
    for (const auto& client : clients) {
        ASSERT_TRUE(client->read_frame().has_value());  // the 1001, ignored
    }
    ASSERT_TRUE(eventually([&] { return running.finished(); }));  // run() returned: loops are gone
    EXPECT_EQ(factory.closed(), factory.opened());
}

TEST(MultiLoop, StopReportsTheCloseOfEveryConnectionOnEveryLoop) {
    CountingFactory factory;
    {
        RunningServer running{builder_for(factory, 3)};
        auto clients = connect_clients(running, 6);
        ASSERT_TRUE(eventually([&] { return factory.opened() == 6; }));
        running.server()
            .stop();  // what EmbeddedServer::stop does when the loop cannot take a request
        ASSERT_TRUE(eventually([&] { return running.finished(); }));
    }  // the server object is gone: loop 0's shard died with it, the workers' when run() returned
    EXPECT_EQ(factory.closed(), factory.opened());
}

TEST(MultiLoop, AServerThatNeverRunsIsDestroyedCleanly) {
    RecordingFactory factory;
    const auto server = builder_for(factory, 4).build();  // no run(): no thread was started
    EXPECT_NE(server->local_endpoint().port, 0U);
}

TEST(MultiLoop, ChurnAcrossLoopsLeavesTheFullCapAvailable) {  // the live count stays exact
    RecordingFactory factory;
    wsrv::ServerBuilder builder = builder_for(factory, 4);
    builder.config().max_connections = 16;
    RunningServer running{builder};
    for (int round = 0; round < 10; ++round) {  // below the cap, so nothing here is refused
        auto clients = connect_clients(running, 8);
        for (const auto& client : clients) {
            ASSERT_TRUE(client->send_frame(Opcode::Text, "x"));
            ASSERT_TRUE(client->read_frame().has_value());
        }
    }  // every round's clients disconnect; the loops erase them in their own time
    EXPECT_TRUE(eventually([&] {  // when all slots are back, the whole cap can be used again
        std::vector<std::unique_ptr<TcpClient>> clients;
        for (int i = 0; i < 16; ++i) {
            clients.push_back(std::make_unique<TcpClient>(running.port()));
            if (!clients.back()->upgrade()) {
                return false;
            }
        }
        return true;
    }));
}

}  // namespace

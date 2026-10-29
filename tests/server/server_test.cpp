// SPDX-License-Identifier: Apache-2.0
// The whole server on a real loopback port: accept, handshake, echo, caps, churn, shutdown.
#include "server/server_builder.hpp"
#include "support/running_server.hpp"
#include "support/tcp_client.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using wsrv::CloseCode;
using wsrv::Opcode;
using wsrv::Session;
using wsrv::test::RunningServer;
using wsrv::test::TcpClient;

class EchoHandler final : public wsrv::IWebSocketHandler {
public:
    void on_text(Session& session, std::string_view text) override {
        static_cast<void>(session.send_text(text).has_value());
    }
    void on_binary(Session& session, wsrv::ByteView data) override {
        static_cast<void>(session.send_binary(data).has_value());
    }
};

class EchoFactory final : public wsrv::IHandlerFactory {
public:
    std::unique_ptr<wsrv::IWebSocketHandler> create(const wsrv::ConnectionInfo&) override {
        return std::make_unique<EchoHandler>();
    }
};

wsrv::ServerBuilder echo_builder(EchoFactory& factory) {
    wsrv::ServerBuilder builder;
    builder.listen("127.0.0.1", 0).handler_factory(factory);
    builder.config().handshake.allowed_origins = {"https://app.example.com"};
    return builder;
}

std::size_t open_descriptors() {
    std::size_t count = 0;
    for ([[maybe_unused]] const auto& entry :
         std::filesystem::directory_iterator{"/proc/self/fd"}) {
        ++count;
    }
    return count;
}

/// Poll until `done()` or 5 s pass; the server runs on its own thread.
template <class Done>
bool eventually(Done&& done) {
    for (int i = 0; i < 500; ++i) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

TEST(Server, EchoesATextMessageOverARealSocket) {
    EchoFactory factory;
    RunningServer running{echo_builder(factory)};
    TcpClient client{running.port()};
    ASSERT_TRUE(client.valid());
    ASSERT_TRUE(client.upgrade());
    ASSERT_TRUE(client.send_frame(Opcode::Text, "hello over tcp"));
    const auto frame = client.read_frame();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->opcode, Opcode::Text);
    EXPECT_EQ(frame->payload, "hello over tcp");
}

TEST(Server, ARefusedOriginGets403AndIsClosed) {
    EchoFactory factory;
    RunningServer running{echo_builder(factory)};
    TcpClient client{running.port()};
    ASSERT_TRUE(client.valid());
    EXPECT_FALSE(client.upgrade("https://evil.example"));
    EXPECT_TRUE(client.wait_for_eof());
}

TEST(Server, TheConnectionCapRefusesTheExtraClientCheaply) {  // FR-S1, T4
    EchoFactory factory;
    wsrv::ServerBuilder builder = echo_builder(factory);
    builder.config().max_connections = 2;
    RunningServer running{builder};
    TcpClient first{running.port()};
    TcpClient second{running.port()};
    ASSERT_TRUE(first.upgrade());
    ASSERT_TRUE(second.upgrade());
    TcpClient third{
        running.port()};  // the TCP connect succeeds (the kernel queue), then it is closed
    EXPECT_TRUE(third.wait_for_eof());
    EXPECT_EQ(running.server().rejected_connections(), 1U);
    ASSERT_TRUE(first.send_frame(Opcode::Text, "still fine"));  // the others are unaffected
    ASSERT_TRUE(first.read_frame().has_value());
}

TEST(Server, ChurnLeavesNoDescriptorsAndTheCapFreeAgain) {  // T15, NFR-R3
    EchoFactory factory;
    wsrv::ServerBuilder builder = echo_builder(factory);
    builder.config().max_connections = 8;
    RunningServer running{builder};
    const std::size_t before = open_descriptors();
    for (int round = 0; round < 100; ++round) {
        TcpClient client{running.port()};
        ASSERT_TRUE(client.valid());
        if (round % 2 == 0) {
            ASSERT_TRUE(client.upgrade());
            ASSERT_TRUE(client.send_frame(Opcode::Text, "x"));
            ASSERT_TRUE(client.read_frame().has_value());
        }  // odd rounds: connect and vanish without a byte
    }
    EXPECT_TRUE(eventually([&] { return open_descriptors() <= before; }));
    TcpClient after{running.port()};  // the cap of 8 was never exhausted by dead connections
    EXPECT_TRUE(after.upgrade());
}

TEST(Server, AGracefulShutdownSends1001AndEndsOnceClientsAnswer) {  // EC-C18, FR-S5
    EchoFactory factory;
    RunningServer running{echo_builder(factory)};
    TcpClient client{running.port()};
    ASSERT_TRUE(client.upgrade());
    ASSERT_TRUE(running.server().request_shutdown().has_value());
    const auto close = client.read_frame();
    ASSERT_TRUE(close.has_value());
    EXPECT_EQ(close->opcode, Opcode::Close);
    EXPECT_EQ(close->payload.substr(0, 2), std::string("\x03\xe9", 2));
    ASSERT_TRUE(client.send_frame(Opcode::Close, wsrv::test::close_payload(1001)));
    EXPECT_TRUE(client.wait_for_eof());
    EXPECT_TRUE(eventually([&] { return running.finished(); }));
}

TEST(Server, AClientThatIgnoresTheShutdownIsDroppedAfterTheGracePeriod) {
    EchoFactory factory;
    wsrv::ServerBuilder builder = echo_builder(factory);
    builder.config().shutdown_grace = 300ms;
    RunningServer running{builder};
    TcpClient client{running.port()};
    ASSERT_TRUE(client.upgrade());
    ASSERT_TRUE(running.server().request_shutdown().has_value());
    ASSERT_TRUE(client.read_frame().has_value());  // the 1001, which it ignores
    EXPECT_TRUE(client.wait_for_eof());
    EXPECT_TRUE(eventually([&] { return running.finished(); }));
}

TEST(Server, ASignalStartsAGracefulShutdownAndASecondOneForcesIt) {  // EC-N17
    EchoFactory factory;
    wsrv::ServerBuilder builder = echo_builder(factory);
    builder.config().shutdown_grace = 60s;  // only a second signal can end this quickly
    RunningServer running{builder, /*with_signals=*/true};
    TcpClient client{running.port()};
    ASSERT_TRUE(client.upgrade());
    running.signal(SIGTERM);
    ASSERT_TRUE(client.read_frame().has_value());  // 1001; the client never answers
    EXPECT_FALSE(running.finished());
    running.signal(SIGTERM);
    EXPECT_TRUE(eventually([&] { return running.finished(); }));
    EXPECT_TRUE(client.wait_for_eof());
}

TEST(ServerBuilder, RejectsABadConfigurationNamingTheField) {
    EchoFactory factory;
    wsrv::ServerBuilder builder = echo_builder(factory);
    builder.config().handshake_timeout = 0ms;
    try {
        static_cast<void>(builder.build());
        FAIL() << "expected a ConfigError";
    } catch (const wsrv::ConfigError& error) {
        EXPECT_NE(std::string{error.what()}.find("handshake_timeout"), std::string::npos);
    }
}

TEST(ServerBuilder, RejectsAHostnameAndAMissingFactory) {
    EchoFactory factory;
    wsrv::ServerBuilder named;
    named.listen("localhost", 0).handler_factory(factory);
    EXPECT_THROW(static_cast<void>(named.build()), wsrv::ConfigError);  // no DNS on the loop
    wsrv::ServerBuilder no_factory;
    EXPECT_THROW(static_cast<void>(no_factory.build()), wsrv::ConfigError);
}

TEST(ServerBuilder, APortAlreadyInUseIsAClearStartupError) {  // EC-N20
    EchoFactory factory;
    RunningServer running{echo_builder(factory)};
    wsrv::ServerBuilder second;
    second.listen("127.0.0.1", running.port()).handler_factory(factory);
    try {
        static_cast<void>(second.build());
        FAIL() << "expected a ConfigError";
    } catch (const wsrv::ConfigError& error) {
        EXPECT_NE(std::string{error.what()}.find("AddressInUse"), std::string::npos);
    }
}

}  // namespace

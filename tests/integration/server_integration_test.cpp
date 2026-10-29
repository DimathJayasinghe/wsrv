// SPDX-License-Identifier: Apache-2.0
// Hostile and awkward clients against a real server on a loopback port (SECURITY_DESIGN section 7,
// "adversarial integration tests"). Each test shows that a limit protects the server AND its other
// clients, not only that a unit works.
#include "server/server_builder.hpp"
#include "support/running_server.hpp"
#include "support/tcp_client.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using wsrv::Opcode;
using wsrv::Session;
using wsrv::test::RunningServer;
using wsrv::test::TcpClient;

/// Echoes everything. "boom" makes it throw (EC-A01). In streaming mode it joins the parts of a
/// message and echoes the whole when the last part arrives.
class TestHandler final : public wsrv::IWebSocketHandler {
public:
    void on_text(Session& session, std::string_view text) override {
        if (text == "boom") {
            throw std::runtime_error{"handler failure"};
        }
        static_cast<void>(session.send_text(text).has_value());
    }
    void on_binary(Session& session, wsrv::ByteView data) override {
        static_cast<void>(session.send_binary(data).has_value());
    }
    void on_message_part(Session& session,
                         wsrv::MessageType /*type*/,
                         wsrv::ByteView part,
                         bool final) override {
        joined_.append(wsrv::as_chars(part));
        if (final) {
            static_cast<void>(session.send_text(joined_).has_value());
            joined_.clear();
        }
    }

private:
    std::string joined_;
};

class TestFactory final : public wsrv::IHandlerFactory {
public:
    std::unique_ptr<wsrv::IWebSocketHandler> create(const wsrv::ConnectionInfo&) override {
        return std::make_unique<TestHandler>();
    }
};

wsrv::ServerBuilder builder_for(TestFactory& factory) {
    wsrv::ServerBuilder builder;
    builder.listen("127.0.0.1", 0).handler_factory(factory);
    builder.config().handshake.allowed_origins = {"https://app.example.com"};
    return builder;
}

/// Joins a thread when the test scope ends (std::jthread is missing from libc++ 18).
class JoinOnExit {
public:
    explicit JoinOnExit(std::thread& thread) : thread_{thread} {}
    JoinOnExit(const JoinOnExit&) = delete;
    JoinOnExit& operator=(const JoinOnExit&) = delete;
    ~JoinOnExit() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    std::thread& thread_;
};

/// Close code of a Close frame payload.
unsigned close_code(const wsrv::test::WireFrame& frame) {
    return (static_cast<unsigned>(static_cast<std::uint8_t>(frame.payload[0])) << 8U) |
           static_cast<std::uint8_t>(frame.payload[1]);
}

// ---- messages of every size ---------------------------------------------------------------------

TEST(Integration, EchoesMessagesAcrossTheLengthEncodingBoundaries) {  // FR-F1, FR-F3
    TestFactory factory;
    RunningServer running{builder_for(factory)};
    TcpClient client{running.port()};
    ASSERT_TRUE(client.upgrade());
    for (const std::size_t size : {0U, 1U, 125U, 126U, 127U, 65535U, 65536U, 300000U}) {
        const std::string text(size, 'q');
        ASSERT_TRUE(client.send_frame(Opcode::Text, text)) << size;
        std::string echoed;
        bool done = false;
        while (!done) {  // large messages are auto-fragmented on the way back (FR-F9)
            const auto frame = client.read_frame();
            ASSERT_TRUE(frame.has_value()) << size;
            echoed += frame->payload;
            done = frame->fin;
        }
        EXPECT_EQ(echoed, text) << size;
    }
}

TEST(Integration, AMessageOverTheLimitClosesWith1009AndTheLimitHasNoOffByOne) {  // FR-F6
    TestFactory factory;
    wsrv::ServerBuilder builder = builder_for(factory);
    builder.config().session.assembler.max_message_size = 1000;
    RunningServer running{builder};
    TcpClient client{running.port()};
    ASSERT_TRUE(client.upgrade());
    ASSERT_TRUE(client.send_frame(Opcode::Text, std::string(1000, 'a')));  // exactly the limit
    ASSERT_TRUE(client.read_frame().has_value());
    ASSERT_TRUE(client.send_frame(Opcode::Text, std::string(1001, 'a')));  // one over
    const auto close = client.read_frame();
    ASSERT_TRUE(close.has_value());
    EXPECT_EQ(close->opcode, Opcode::Close);
    EXPECT_EQ(close_code(*close), 1009U);
    EXPECT_TRUE(client.wait_for_eof());
}

TEST(Integration, AFragmentedMessageWithAnInterleavedPingIsReassembledAndPonged) {  // EC-C01
    TestFactory factory;
    RunningServer running{builder_for(factory)};
    TcpClient client{running.port()};
    ASSERT_TRUE(client.upgrade());
    ASSERT_TRUE(client.send_frame(Opcode::Text, "Hel", false));
    ASSERT_TRUE(client.send_frame(Opcode::Ping, "mid"));
    ASSERT_TRUE(client.send_frame(Opcode::Continuation, "lo", true));
    const auto pong = client.read_frame();
    ASSERT_TRUE(pong.has_value());
    EXPECT_EQ(pong->opcode, Opcode::Pong);
    EXPECT_EQ(pong->payload, "mid");
    const auto text = client.read_frame();
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(text->payload, "Hello");
}

TEST(Integration, StreamingDeliveryHandsThePartsToTheHandlerInOrder) {  // FRAGMENTATION 1.3
    TestFactory factory;
    wsrv::ServerBuilder builder = builder_for(factory);
    builder.config().session.assembler.streaming = true;
    RunningServer running{builder};
    TcpClient client{running.port()};
    ASSERT_TRUE(client.upgrade());
    ASSERT_TRUE(client.send_frame(Opcode::Text, "one-", false));
    ASSERT_TRUE(client.send_frame(Opcode::Continuation, "two-", false));
    ASSERT_TRUE(client.send_frame(Opcode::Continuation, "three", true));
    const auto echoed = client.read_frame();
    ASSERT_TRUE(echoed.has_value());
    EXPECT_EQ(echoed->payload, "one-two-three");
}

// ---- hostile clients ----------------------------------------------------------------------------

TEST(Integration, ASlowlorisHandshakeIsClosedAtTheTimeoutAndOthersAreServed) {  // EC-H17, T2
    TestFactory factory;
    wsrv::ServerBuilder builder = builder_for(factory);
    builder.config().handshake_timeout = 300ms;
    RunningServer running{builder};
    TcpClient loris{running.port()};
    ASSERT_TRUE(loris.send("GET /chat HTTP/1.1\r\nHost: x\r\n"));  // and then... silence
    TcpClient honest{running.port()};
    EXPECT_TRUE(honest.upgrade());  // not blocked by the stalled client
    EXPECT_TRUE(loris.wait_for_eof());
}

TEST(Integration, GarbageAndATlsHelloOnThePlainPortAreRefused) {  // EC-H22
    TestFactory factory;
    RunningServer running{builder_for(factory)};
    TcpClient tls{running.port()};
    ASSERT_TRUE(tls.send(std::string("\x16\x03\x01\x02\x00\x01\x00\x01\xfc\x03\x03", 11) +
                         std::string(64, 'x') + "\r\n\r\n"));
    EXPECT_EQ(tls.read_http_head().rfind("HTTP/1.1 400 ", 0), 0U);
    EXPECT_TRUE(tls.wait_for_eof());
    TcpClient oversize{running.port()};
    ASSERT_TRUE(oversize.send("GET / HTTP/1.1\r\n" + std::string(20000, 'a')));
    EXPECT_EQ(oversize.read_http_head().rfind("HTTP/1.1 431 ", 0), 0U);
}

TEST(Integration, AThrowingHandlerClosesOnlyItsOwnConnectionWith1011) {  // EC-A01, FR-S6
    TestFactory factory;
    RunningServer running{builder_for(factory)};
    TcpClient bad{running.port()};
    TcpClient good{running.port()};
    ASSERT_TRUE(bad.upgrade());
    ASSERT_TRUE(good.upgrade());
    ASSERT_TRUE(bad.send_frame(Opcode::Text, "boom"));
    const auto close = bad.read_frame();
    ASSERT_TRUE(close.has_value());
    EXPECT_EQ(close_code(*close), 1011U);
    ASSERT_TRUE(good.send_frame(Opcode::Text, "still here"));
    const auto reply = good.read_frame();
    ASSERT_TRUE(reply.has_value());
    EXPECT_EQ(reply->payload, "still here");
}

TEST(Integration, ASlowReaderIsDroppedAndANeighbourIsUnaffected) {  // EC-N09, T3
    TestFactory factory;
    wsrv::ServerBuilder builder = builder_for(factory);
    builder.config().session.outbound.max_queued_bytes = 256 * 1024;
    builder.config().write_stall_timeout = 500ms;
    builder.config().session.close_timeout = 1s;  // the peer will not answer our Close
    RunningServer running{builder};
    TcpClient slow{running.port()};
    TcpClient neighbour{running.port()};
    ASSERT_TRUE(slow.upgrade());
    ASSERT_TRUE(neighbour.upgrade());
    const std::string chunk(60000, 's');
    bool dropped = false;
    for (int i = 0; i < 400 && !dropped; ++i) {  // asks for echoes and never reads them
        dropped = !slow.send_frame(Opcode::Text, chunk);
    }
    ASSERT_TRUE(neighbour.send_frame(Opcode::Text, "ping"));
    const auto reply = neighbour.read_frame();
    ASSERT_TRUE(reply.has_value());
    EXPECT_EQ(reply->payload, "ping");
    // Two legitimate endings: the stall timer drops the connection while the peer still reads
    // nothing (reset/EOF), or the peer starts reading, receives Close 1008 and the connection ends
    // when the unanswered closing handshake times out. Either way it must END.
    while (const auto frame = slow.read_frame()) {
        if (frame->opcode == Opcode::Close) {
            EXPECT_EQ(close_code(*frame), 1008U);
            break;
        }
    }
    EXPECT_TRUE(slow.wait_for_eof());
}

TEST(Integration, AFloodingClientDoesNotStarveAQuietNeighbour) {  // EC-N08, T7
    TestFactory factory;
    RunningServer running{builder_for(factory)};
    TcpClient flooder{running.port()};
    TcpClient quiet{running.port()};
    ASSERT_TRUE(flooder.upgrade());
    ASSERT_TRUE(quiet.upgrade());
    std::thread flood([&] {
        const std::string chunk(16000, 'f');
        for (int i = 0; i < 300; ++i) {
            if (!flooder.send_frame(Opcode::Text, chunk)) {
                return;
            }
        }
    });
    const JoinOnExit joiner(flood);  // an early ASSERT must not leave a joinable thread behind
    for (int i = 0; i < 20; ++i) {
        const auto start = std::chrono::steady_clock::now();
        ASSERT_TRUE(quiet.send_frame(Opcode::Text, "q"));
        const auto reply = quiet.read_frame();
        ASSERT_TRUE(reply.has_value());
        EXPECT_LT(std::chrono::steady_clock::now() - start, 2s) << "round " << i;
    }
    flood.join();  // never close a socket another thread is still writing to
}

TEST(Integration, ASilentPeerIsPingedThenClosedByTheHeartbeat) {  // EC-C16, EC-N11
    TestFactory factory;
    wsrv::ServerBuilder builder = builder_for(factory);
    builder.config().session.ping_interval = 1s;
    builder.config().session.pong_timeout = 1s;
    RunningServer running{builder};
    TcpClient silent{running.port()};
    ASSERT_TRUE(silent.upgrade());
    const auto ping = silent.read_frame();  // never answered
    ASSERT_TRUE(ping.has_value());
    EXPECT_EQ(ping->opcode, Opcode::Ping);
    const auto close = silent.read_frame();
    ASSERT_TRUE(close.has_value());
    EXPECT_EQ(close->opcode, Opcode::Close);
    EXPECT_EQ(close_code(*close), 1001U);
}

}  // namespace

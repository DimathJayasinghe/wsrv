// SPDX-License-Identifier: Apache-2.0
// permessage-deflate end to end (RFC 7692, ADR-0020): a real server on a real loopback port and a
// client that negotiates, compresses and inflates with real zlib. Built only with zlib.
#include "server/server_builder.hpp"
#include "server/session_handle.hpp"
#include "support/peer_deflate.hpp"
#include "support/running_server.hpp"
#include "support/tcp_client.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;
using wsrv::Opcode;
using wsrv::Session;
using wsrv::test::PeerCompressor;
using wsrv::test::PeerDecompressor;
using wsrv::test::RunningServer;
using wsrv::test::TcpClient;

class EchoHandler final : public wsrv::IWebSocketHandler {
public:
    void on_text(Session& session, std::string_view text) override {
        static_cast<void>(session.send_text(text).has_value());
    }
};

class EchoFactory final : public wsrv::IHandlerFactory {
public:
    std::unique_ptr<wsrv::IWebSocketHandler> create(const wsrv::ConnectionInfo&) override {
        return std::make_unique<EchoHandler>();
    }
};

wsrv::ServerBuilder deflate_builder(EchoFactory& factory,
                                    const wsrv::DeflateOptions& options = {}) {
    wsrv::ServerBuilder builder;
    builder.listen("127.0.0.1", 0).handler_factory(factory);
    builder.config().handshake.allowed_origins = {"https://app.example.com"};
    builder.permessage_deflate(options);
    return builder;
}

constexpr std::string_view kOffer =
    "Sec-WebSocket-Extensions: permessage-deflate; client_max_window_bits\r\n";

/// Text that compresses well, like the JSON real applications send. Only for short messages: a
/// long run of one repeated record compresses so well that the bomb limit (ratio 100) is right to
/// refuse it.
std::string json_like(std::size_t size) {
    std::string text;
    while (text.size() < size) {
        text += R"({"id":42,"name":"alice","tags":["a","b","c"],"active":true},)";
    }
    text.resize(size);
    return text;
}

/// Letters from a fixed pseudo-random sequence: compresses only about 2:1, so long honest messages
/// stay well under the ratio limit.
std::string varied_text(std::size_t size) {
    std::string text(size, ' ');
    std::uint32_t state = 2463534242U;
    for (char& c : text) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        c = static_cast<char>('a' + (state % 16U));
    }
    return text;
}

/// The two-byte close code at the start of a Close frame's payload.
unsigned close_code_of(const wsrv::test::WireFrame& frame) {
    EXPECT_EQ(frame.opcode, Opcode::Close);
    EXPECT_GE(frame.payload.size(), 2U);
    if (frame.payload.size() < 2) {
        return 0;
    }
    return (static_cast<unsigned>(static_cast<unsigned char>(frame.payload[0])) << 8U) |
           static_cast<unsigned char>(frame.payload[1]);
}

std::size_t open_descriptors() {
    std::size_t count = 0;
    for ([[maybe_unused]] const auto& entry :
         std::filesystem::directory_iterator{"/proc/self/fd"}) {
        ++count;
    }
    return count;
}

TEST(ServerDeflate, ACompressedMessageIsEchoedCompressedAndReadsBack) {
    EchoFactory factory;
    RunningServer server{deflate_builder(factory)};
    TcpClient client{server.port()};
    ASSERT_TRUE(client.valid());
    const std::string head = client.upgrade_head(kOffer);
    ASSERT_EQ(head.rfind("HTTP/1.1 101 ", 0), 0U) << head;
    EXPECT_NE(head.find("Sec-WebSocket-Extensions: permessage-deflate"), std::string::npos);

    PeerCompressor compressor;
    PeerDecompressor decompressor;
    for (const std::size_t size :
         {4000U, 70000U, 4000U}) {  // several messages: no state leaks over
        SCOPED_TRACE("message of " + std::to_string(size) + " bytes");
        const std::string text = varied_text(size);
        ASSERT_TRUE(client.send_frame(Opcode::Text, compressor.compress(text), true, true));
        const auto reply = client.read_frame();
        ASSERT_TRUE(reply.has_value());
        EXPECT_TRUE(reply->rsv1) << "a large compressible reply is sent compressed";
        EXPECT_LT(reply->payload.size(), text.size());
        EXPECT_EQ(decompressor.decompress(reply->payload), text);
    }
}

TEST(ServerDeflate, AShortReplyIsSentPlainEvenOverAnAgreedConnection) {  // EC-D07
    EchoFactory factory;
    RunningServer server{deflate_builder(factory)};
    TcpClient client{server.port()};
    ASSERT_EQ(client.upgrade_head(kOffer).rfind("HTTP/1.1 101 ", 0), 0U);
    PeerCompressor compressor;
    ASSERT_TRUE(client.send_frame(Opcode::Text, compressor.compress("hi"), true, true));
    const auto reply = client.read_frame();
    ASSERT_TRUE(reply.has_value());
    EXPECT_FALSE(reply->rsv1);
    EXPECT_EQ(reply->payload, "hi");
}

TEST(ServerDeflate, AClientThatDidNotNegotiateIsFailedWith1002ForRsv1) {  // RFC 6455 5.2, EC-F24
    EchoFactory factory;
    RunningServer server{deflate_builder(factory)};
    TcpClient client{server.port()};
    const std::string head = client.upgrade_head({});  // the server could, but nothing was offered
    ASSERT_EQ(head.rfind("HTTP/1.1 101 ", 0), 0U);
    EXPECT_EQ(head.find("Sec-WebSocket-Extensions"), std::string::npos);
    ASSERT_TRUE(client.send_frame(Opcode::Text, "x", true, true));
    const auto reply = client.read_frame();
    ASSERT_TRUE(reply.has_value());
    EXPECT_EQ(close_code_of(*reply), 1002U);
    EXPECT_TRUE(client.wait_for_eof());
}

TEST(ServerDeflate, InvalidCompressedDataIsFailedWith1007) {  // RFC 6455 7.4.1
    EchoFactory factory;
    RunningServer server{deflate_builder(factory)};
    TcpClient client{server.port()};
    ASSERT_EQ(client.upgrade_head(kOffer).rfind("HTTP/1.1 101 ", 0), 0U);
    ASSERT_TRUE(client.send_frame(Opcode::Text, "\xff\xff\xff\xff\xff", true, true));
    const auto reply = client.read_frame();
    ASSERT_TRUE(reply.has_value());
    EXPECT_EQ(close_code_of(*reply), 1007U);
    EXPECT_TRUE(client.wait_for_eof());
}

TEST(ServerDeflate, ADecompressionBombIsRefusedWith1009AndTheServerCarriesOn) {  // T6
    EchoFactory factory;
    RunningServer server{deflate_builder(factory)};
    {
        TcpClient attacker{server.port()};
        ASSERT_EQ(attacker.upgrade_head(kOffer).rfind("HTTP/1.1 101 ", 0), 0U);
        PeerCompressor compressor;
        const std::string bomb(8U << 20U, '\0');  // 8 MiB of zeros: about 8 KiB on the wire
        const std::string wire = compressor.compress(bomb);
        ASSERT_LT(wire.size(), 16U << 10U);
        ASSERT_TRUE(attacker.send_frame(Opcode::Text, wire, true, true));
        const auto reply = attacker.read_frame();
        ASSERT_TRUE(reply.has_value());
        EXPECT_EQ(close_code_of(*reply), 1009U);
        EXPECT_TRUE(attacker.wait_for_eof());
    }
    TcpClient honest{server.port()};  // the server is still serving
    ASSERT_TRUE(honest.upgrade());
    ASSERT_TRUE(honest.send_frame(Opcode::Text, "still here"));
    const auto reply = honest.read_frame();
    ASSERT_TRUE(reply.has_value());
    EXPECT_EQ(reply->payload, "still here");
}

// RFC 7692 section 7.1.2.2 lets a client offer a window of 8. The server agrees (its inflater
// always uses 15 bits, which decodes any window) and must keep running: a rejected agreement used
// to be an always-on assertion in the session constructor, i.e. a one-request process abort.
TEST(ServerDeflate, AClientWindowOfEightIsAgreedAndServed) {
    EchoFactory factory;
    RunningServer server{deflate_builder(factory)};
    TcpClient client{server.port()};
    const std::string head = client.upgrade_head("Sec-WebSocket-Extensions: permessage-deflate; "
                                                 "client_max_window_bits=8\r\n");
    ASSERT_EQ(head.rfind("HTTP/1.1 101 ", 0), 0U) << head;
    EXPECT_NE(head.find("client_max_window_bits=8"), std::string::npos);
    PeerCompressor compressor;
    PeerDecompressor decompressor;
    const std::string text = json_like(3000);
    ASSERT_TRUE(client.send_frame(Opcode::Text, compressor.compress(text), true, true));
    const auto reply = client.read_frame();
    ASSERT_TRUE(reply.has_value());
    EXPECT_EQ(decompressor.decompress(reply->payload), text);
}

TEST(ServerDeflate, ABadOfferIsDeclinedNotFatalAndTheConnectionWorksUncompressed) {
    EchoFactory factory;
    RunningServer server{deflate_builder(factory)};
    TcpClient client{server.port()};
    const std::string head = client.upgrade_head(
        "Sec-WebSocket-Extensions: permessage-deflate; server_max_window_bits=99\r\n");
    ASSERT_EQ(head.rfind("HTTP/1.1 101 ", 0), 0U) << head;
    EXPECT_EQ(head.find("Sec-WebSocket-Extensions"), std::string::npos);
    ASSERT_TRUE(client.send_frame(Opcode::Text, "plain"));
    const auto reply = client.read_frame();
    ASSERT_TRUE(reply.has_value());
    EXPECT_FALSE(reply->rsv1);
    EXPECT_EQ(reply->payload, "plain");
}

// Many connections that each allocate zlib state and then leave: nothing may stay behind. The
// sanitizer build (LeakSanitizer) reports leaked codecs when the process ends; the descriptors are
// checked here.
TEST(ServerDeflate, ChurnOfCompressedConnectionsLeavesNoDescriptorsBehind) {
    EchoFactory factory;
    RunningServer server{deflate_builder(factory)};
    const std::size_t before = open_descriptors();
    const std::string text = json_like(2000);
    for (int i = 0; i < 100; ++i) {
        TcpClient client{server.port()};
        ASSERT_EQ(client.upgrade_head(kOffer).rfind("HTTP/1.1 101 ", 0), 0U);
        PeerCompressor compressor;
        ASSERT_TRUE(client.send_frame(Opcode::Text, compressor.compress(text), true, true));
        ASSERT_TRUE(client.read_frame().has_value());
    }
    std::size_t after = open_descriptors();
    for (int i = 0; i < 500 && after > before; ++i) {  // the server closes on its own thread
        std::this_thread::sleep_for(10ms);
        after = open_descriptors();
    }
    EXPECT_LE(after, before);
}

// ---- the builder checks what the operator configured
// ---------------------------------------------

std::string build_error(const wsrv::DeflateOptions& options) {
    EchoFactory factory;
    try {
        static_cast<void>(deflate_builder(factory, options).build());
    } catch (const wsrv::ConfigError& error) {
        return error.what();
    }
    return {};
}

TEST(ServerDeflate, ImpossibleSettingsAreRefusedAtBuildTimeNamingTheFeature) {
    EXPECT_EQ(build_error(wsrv::DeflateOptions{}), "");
    wsrv::DeflateOptions options;
    options.level = 0;
    EXPECT_NE(build_error(options).find("permessage_deflate"), std::string::npos);
    options = {};
    options.memory_level = 10;
    EXPECT_NE(build_error(options).find("permessage_deflate"), std::string::npos);
    options = {};
    options.policy.server_max_window_bits = 8;  // zlib cannot compress with a window of 8
    EXPECT_NE(build_error(options).find("permessage_deflate"), std::string::npos);
    options = {};
    options.policy.client_max_window_bits = 16;
    EXPECT_NE(build_error(options).find("permessage_deflate"), std::string::npos);
    options = {};
    options.max_inflate_ratio = 0;  // no ratio limit is not an option (T6)
    EXPECT_NE(build_error(options), "");
    options = {};
    options.max_inflate_ratio = wsrv::kMaxConfiguredInflateRatio + 1;
    EXPECT_NE(build_error(options), "");
}

}  // namespace

// SPDX-License-Identifier: Apache-2.0
// wss:// end to end: the real Server with TLS on a loopback port, an OpenSSL client over a real
// socket. ADR-0019 sections 4 and 9; edge cases EC-T01 to EC-T07.
#include "net/transport_wrapper.hpp"
#include "server/server_builder.hpp"
#include "support/running_server.hpp"
#include "support/tls_client.hpp"
#include "support/tls_fixtures.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "tls/server_context.hpp"
#include "tls/tls_transport_wrapper.hpp"

namespace {

using namespace std::chrono_literals;
using wsrv::Opcode;
using wsrv::test::RunningServer;
using wsrv::test::TcpClient;
using wsrv::test::TlsClient;

namespace fs = std::filesystem;
constexpr fs::perms kPublic =
    fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read;
constexpr fs::perms kPrivate = fs::perms::owner_read | fs::perms::owner_write;

class EchoHandler final : public wsrv::IWebSocketHandler {
public:
    void on_text(wsrv::Session& session, std::string_view text) override {
        static_cast<void>(session.send_text(text).has_value());
    }
    void on_binary(wsrv::Session& session, wsrv::ByteView data) override {
        static_cast<void>(session.send_binary(data).has_value());
    }
};

class EchoFactory final : public wsrv::IHandlerFactory {
public:
    std::unique_ptr<wsrv::IWebSocketHandler> create(const wsrv::ConnectionInfo&) override {
        return std::make_unique<EchoHandler>();
    }
};

/// A certificate in a temp dir and the TLS pieces built from it.
struct Tls {
    wsrv::test::TempDir dir;
    wsrv::NullLogger logger;
    std::unique_ptr<wsrv::TlsServerContext> context;
    std::unique_ptr<wsrv::TlsTransportWrapper> wrapper;
    wsrv::TlsConfig config;

    explicit Tls(const std::string& common_name = "localhost") {
        write_certificate(common_name);
        auto loaded = wsrv::TlsServerContext::load(config, logger);
        EXPECT_TRUE(loaded.has_value());
        context = std::move(*loaded);
        wrapper = std::make_unique<wsrv::TlsTransportWrapper>(*context);
    }

    void write_certificate(const std::string& common_name) {
        const auto pem = wsrv::test::make_pem({.common_name = common_name});
        config.certificate_chain_file = dir.write("chain.pem", pem.certificate, kPublic);
        config.private_key_file = dir.write("key.pem", pem.key, kPrivate);
    }
};

wsrv::ServerBuilder builder_for(EchoFactory& factory, wsrv::ITransportWrapper* wrapper) {
    wsrv::ServerBuilder builder;
    builder.listen("127.0.0.1", 0).handler_factory(factory);
    builder.config().handshake.allowed_origins = {"https://app.example.com"};
    if (wrapper != nullptr) {
        builder.transport_wrapper(*wrapper);
    }
    return builder;
}

/// Echo one message of `size` bytes; the answer may come in fragments (FR-F9).
bool echoes(TlsClient& client, std::size_t size) {
    const std::string text(size, 'w');
    if (!client.send_frame(Opcode::Text, text)) {
        return false;
    }
    std::string back;
    for (bool done = false; !done;) {
        const auto frame = client.read_frame();
        if (!frame) {
            return false;
        }
        back += frame->payload;
        done = frame->fin;
    }
    return back == text;
}

// ---- the happy path -----------------------------------------------------------------------------

TEST(TlsServer, WssEchoesMessagesAcrossTheLengthEncodingBoundaries) {
    Tls tls;
    EchoFactory factory;
    RunningServer running{builder_for(factory, tls.wrapper.get())};
    TlsClient client{running.port()};
    ASSERT_TRUE(client.tls_connected());
    ASSERT_TRUE(client.upgrade());
    for (const std::size_t size : {0U, 1U, 125U, 126U, 65535U, 65536U, 300000U}) {
        EXPECT_TRUE(echoes(client, size)) << size;
    }
}

TEST(TlsServer, TheCertificateIsServedOverTls13AndAlpnPicksHttp11) {
    Tls tls{"first.example"};
    EchoFactory factory;
    RunningServer running{builder_for(factory, tls.wrapper.get())};
    TlsClient client{running.port(), {.alpn = {"h2", "http/1.1"}}};
    ASSERT_TRUE(client.tls_connected());
    EXPECT_EQ(client.version(), TLS1_3_VERSION);
    EXPECT_EQ(client.peer_common_name(), "first.example");
    EXPECT_EQ(client.alpn(), "http/1.1");
}

TEST(TlsServer, ATls12ClientIsServed) {
    Tls tls;
    EchoFactory factory;
    RunningServer running{builder_for(factory, tls.wrapper.get())};
    TlsClient client{running.port(), {.max_version = TLS1_2_VERSION}};
    ASSERT_TRUE(client.tls_connected());
    EXPECT_EQ(client.version(), TLS1_2_VERSION);
    ASSERT_TRUE(client.upgrade());
    EXPECT_TRUE(echoes(client, 1000));
}

TEST(TlsServer, AClientThatOffersOnlyH2IsRefusedByAlpn) {
    Tls tls;
    EchoFactory factory;
    RunningServer running{builder_for(factory, tls.wrapper.get())};
    TlsClient client{running.port(), {.alpn = {"h2"}}};
    EXPECT_FALSE(client.tls_connected());
}

TEST(TlsServer, ACloseFromTheClientEndsTheConnectionCleanly) {
    Tls tls;
    EchoFactory factory;
    RunningServer running{builder_for(factory, tls.wrapper.get())};
    TlsClient client{running.port()};
    ASSERT_TRUE(client.upgrade());
    ASSERT_TRUE(
        client.send(wsrv::test::client_frame(Opcode::Close, true, std::string{"\x03\xE8", 2})));
    const auto reply = client.read_frame();
    ASSERT_TRUE(reply.has_value());
    EXPECT_EQ(reply->opcode, Opcode::Close);
    EXPECT_TRUE(client.wait_for_eof());  // close_notify and FIN reached us
}

TEST(TlsServer, AGracefulShutdownSendsClose1001OverTls) {  // EC-C18 through the transport
    Tls tls;
    EchoFactory factory;
    RunningServer running{builder_for(factory, tls.wrapper.get())};
    TlsClient client{running.port()};
    ASSERT_TRUE(client.upgrade());
    ASSERT_TRUE(running.server().request_shutdown().has_value());
    const auto frame = client.read_frame();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->opcode, Opcode::Close);
    ASSERT_GE(frame->payload.size(), 2U);
    EXPECT_EQ(static_cast<std::uint8_t>(frame->payload[0]), 0x03);
    EXPECT_EQ(static_cast<std::uint8_t>(frame->payload[1]), 0xE9);  // 1001
}

// ---- hostile and broken clients -----------------------------------------------------------------

TEST(TlsServer, AClientThatSendsNothingIsClosedAtTheHandshakeTimeout) {  // EC-T07, slowloris
    Tls tls;
    EchoFactory factory;
    auto builder = builder_for(factory, tls.wrapper.get());
    builder.config().handshake_timeout = 300ms;
    RunningServer running{std::move(builder)};
    TcpClient silent{running.port()};  // a bare TCP connection: no ClientHello
    ASSERT_TRUE(silent.valid());
    EXPECT_TRUE(silent.wait_for_eof());
    TlsClient honest{running.port()};  // the server is still serving others
    EXPECT_TRUE(honest.upgrade());
}

TEST(TlsServer, AClientThatStallsInsideTheTlsHandshakeIsClosedAtTheTimeout) {  // EC-T07
    Tls tls;
    EchoFactory factory;
    auto builder = builder_for(factory, tls.wrapper.get());
    builder.config().handshake_timeout = 300ms;
    RunningServer running{std::move(builder)};
    TcpClient stalled{running.port()};
    ASSERT_TRUE(stalled.valid());
    ASSERT_TRUE(
        stalled.send(std::string{"\x16\x03\x01\x00\x80\x01", 6}));  // the start of a ClientHello
    EXPECT_TRUE(stalled.wait_for_eof());
}

TEST(TlsServer, APlainHttpRequestOnTheTlsPortIsClosed) {  // EC-T02
    Tls tls;
    EchoFactory factory;
    RunningServer running{builder_for(factory, tls.wrapper.get())};
    TcpClient plain{running.port()};
    ASSERT_TRUE(plain.valid());
    ASSERT_TRUE(plain.send(TcpClient::upgrade_request()));
    EXPECT_TRUE(plain.wait_for_eof());
    TlsClient honest{running.port()};
    EXPECT_TRUE(honest.upgrade());
}

TEST(TlsServer, ATlsClientOnAPlainPortIsClosed) {  // EC-T02, EC-H22
    EchoFactory factory;
    RunningServer running{builder_for(factory, nullptr)};
    TlsClient client{running.port()};
    EXPECT_FALSE(client.tls_connected());
    TcpClient honest{running.port()};
    EXPECT_TRUE(honest.upgrade());
}

TEST(TlsServer, AnUpgradeRequestMayArriveOneByteAtATimeInsideTls) {  // EC-H17 under encryption
    Tls tls;
    EchoFactory factory;
    RunningServer running{builder_for(factory, tls.wrapper.get())};
    TlsClient client{running.port()};
    ASSERT_TRUE(client.tls_connected());
    for (const char byte : TcpClient::upgrade_request()) {
        ASSERT_TRUE(client.send(std::string_view{&byte, 1}));
    }
    EXPECT_EQ(client.read_http_head().rfind("HTTP/1.1 101 ", 0), 0U);
}

// ---- several loops ------------------------------------------------------------------------------

TEST(TlsServer, ManyClientsOnSeveralLoopsAreServed) {
    Tls tls;
    EchoFactory factory;
    auto builder = builder_for(factory, tls.wrapper.get());
    builder.config().threads = 3;
    RunningServer running{std::move(builder)};
    std::atomic<int> served{0};
    std::vector<std::thread> clients;
    for (int i = 0; i < 24; ++i) {
        clients.emplace_back([&, i] {
            TlsClient client{running.port()};
            if (client.upgrade() && echoes(client, 500 + static_cast<std::size_t>(i) * 1000)) {
                ++served;
            }
        });
    }
    for (std::thread& client : clients) {
        client.join();
    }
    EXPECT_EQ(served.load(), 24);
}

// ---- certificate reload -------------------------------------------------------------------------

TEST(TlsServer, AReloadedCertificateServesNewConnectionsAndKeepsOldOnes) {
    Tls tls{"before"};
    EchoFactory factory;
    RunningServer running{builder_for(factory, tls.wrapper.get())};
    TlsClient old_client{running.port()};
    ASSERT_TRUE(old_client.upgrade());
    EXPECT_EQ(old_client.peer_common_name(), "before");

    tls.write_certificate("after");
    ASSERT_TRUE(tls.context->reload().has_value());

    TlsClient new_client{running.port()};
    ASSERT_TRUE(new_client.upgrade());
    EXPECT_EQ(new_client.peer_common_name(), "after");
    EXPECT_TRUE(echoes(old_client, 2000));  // the connection that began before keeps working
    EXPECT_TRUE(echoes(new_client, 2000));
}

TEST(TlsServer, AFailedReloadKeepsServingTheOldCertificate) {  // EC-T03
    Tls tls{"steady"};
    EchoFactory factory;
    RunningServer running{builder_for(factory, tls.wrapper.get())};
    const auto expired = wsrv::test::make_pem(
        {.common_name = "expired", .not_before_offset_s = -7200, .not_after_offset_s = -3600});
    tls.dir.write("chain.pem", expired.certificate, kPublic);
    tls.dir.write("key.pem", expired.key, kPrivate);
    EXPECT_FALSE(tls.context->reload().has_value());
    TlsClient client{running.port()};
    ASSERT_TRUE(client.upgrade());
    EXPECT_EQ(client.peer_common_name(), "steady");
}

// ---- the wrapper may fail -----------------------------------------------------------------------

class RefusingWrapper final : public wsrv::ITransportWrapper {
public:
    std::unique_ptr<wsrv::ITransport> wrap(std::unique_ptr<wsrv::ITransport> /*plain*/) override {
        ++calls;
        return nullptr;  // out of memory, say: the contract says the connection is dropped
    }
    std::atomic<int> calls{0};
};

TEST(TlsServer, AWrapperThatFailsDropsTheConnectionAndTheServerCarriesOn) {
    RefusingWrapper wrapper;
    EchoFactory factory;
    RunningServer running{builder_for(factory, &wrapper)};
    for (int i = 0; i < 3; ++i) {
        TcpClient client{running.port()};
        ASSERT_TRUE(client.valid());
        EXPECT_TRUE(client.wait_for_eof());
    }
    EXPECT_EQ(wrapper.calls.load(), 3);
}

}  // namespace

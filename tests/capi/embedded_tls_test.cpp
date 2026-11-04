// SPDX-License-Identifier: Apache-2.0
// TLS through the embedded server (ADR-0019 sections 7 and 8): the config keys turn on wss://, a
// bad setup fails closed at start, and reload_tls() swaps the certificate for new connections.
#include "capi/embedded_server.hpp"
#include "support/tls_client.hpp"
#include "support/tls_fixtures.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

namespace {

using namespace std::chrono_literals;
using wsrv::EmbeddedConfig;
using wsrv::EmbeddedServer;
using wsrv::ErrorCode;
using wsrv::test::TcpClient;
using wsrv::test::TempDir;
using wsrv::test::TlsClient;

namespace fs = std::filesystem;
constexpr fs::perms kPublic =
    fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read;
constexpr fs::perms kPrivate = fs::perms::owner_read | fs::perms::owner_write;

/// Certificate and key files in a temp dir that tests can overwrite to exercise a reload.
struct Certificate {
    TempDir dir;
    std::string chain;
    std::string key;

    explicit Certificate(const std::string& common_name = "first.example") {
        write(common_name);
    }
    void write(const std::string& common_name) {
        const auto pem = wsrv::test::make_pem({.common_name = common_name});
        chain = dir.write("chain.pem", pem.certificate, kPublic);
        key = dir.write("key.pem", pem.key, kPrivate);
    }
};

EmbeddedConfig tls_config(const Certificate& certificate) {
    EmbeddedConfig config;
    EXPECT_TRUE(config.set("allow_origin", "https://app.example.com").has_value());
    EXPECT_TRUE(config.set("shutdown_grace_ms", "2000").has_value());
    EXPECT_TRUE(config.set("tls_cert", certificate.chain).has_value());
    EXPECT_TRUE(config.set("tls_key", certificate.key).has_value());
    return config;
}

std::unique_ptr<EmbeddedServer> created(const EmbeddedConfig& config) {
    auto server = EmbeddedServer::create(config);
    EXPECT_TRUE(server.has_value());
    return std::move(*server);
}

TEST(EmbeddedTls, WssWorksEndToEndThroughTheHost) {
    const Certificate certificate;
    const auto server = created(tls_config(certificate));
    ASSERT_TRUE(server->start().has_value());

    TlsClient client{server->port()};
    ASSERT_TRUE(client.upgrade());
    EXPECT_EQ(client.peer_common_name(), "first.example");
    const auto open = server->poll(3000ms);
    ASSERT_TRUE(open.has_value());

    ASSERT_TRUE(client.send_frame(wsrv::Opcode::Text, "over tls"));
    const auto message = server->poll(3000ms);
    ASSERT_TRUE(message.has_value());
    ASSERT_TRUE(server->send_text(message->conn, "back").has_value());
    const auto frame = client.read_frame();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->payload, "back");
}

TEST(EmbeddedTls, APlainClientOnTheTlsPortIsNotServed) {
    const Certificate certificate;
    const auto server = created(tls_config(certificate));
    ASSERT_TRUE(server->start().has_value());
    TcpClient client{server->port()};
    EXPECT_FALSE(client.upgrade());  // never a 101: the bytes are not a TLS ClientHello
}

TEST(EmbeddedTls, HalfASetupFailsClosedInsteadOfServingPlainText) {
    const Certificate certificate;
    EmbeddedConfig config;
    ASSERT_TRUE(config.set("tls_cert", certificate.chain).has_value());  // no key
    const auto server = created(config);
    const auto started = server->start();
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error().code, ErrorCode::InvalidConfig);
    EXPECT_NE(server->start_error().find("tls_key"), std::string::npos) << server->start_error();
    EXPECT_FALSE(server->running());
}

TEST(EmbeddedTls, AMissingCertificateFileFailsAtStartWithTheReason) {
    const Certificate certificate;
    EmbeddedConfig config = tls_config(certificate);
    ASSERT_TRUE(config.set("tls_cert", (certificate.dir.path() / "nope.pem").string()).has_value());
    const auto server = created(config);
    const auto started = server->start();
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error().code, ErrorCode::InvalidConfig);
    EXPECT_NE(server->start_error().find("nope.pem"), std::string::npos) << server->start_error();
}

TEST(EmbeddedTls, AKeyFileOpenToEveryoneIsRefusedAtStart) {  // SECURITY_DESIGN rule 7
    const Certificate certificate;
    fs::permissions(certificate.key, kPrivate | fs::perms::others_read);
    const auto server = created(tls_config(certificate));
    EXPECT_FALSE(server->start().has_value());
    EXPECT_FALSE(server->start_error().empty());
}

TEST(EmbeddedTls, ReloadGivesNewConnectionsTheNewCertificateAndKeepsOldOnes) {
    Certificate certificate;
    const auto server = created(tls_config(certificate));
    ASSERT_TRUE(server->start().has_value());

    TlsClient before{server->port()};
    ASSERT_TRUE(before.upgrade());
    ASSERT_EQ(before.peer_common_name(), "first.example");

    certificate.write("second.example");
    ASSERT_TRUE(server->reload_tls().has_value());

    TlsClient after{server->port()};
    ASSERT_TRUE(after.upgrade());
    EXPECT_EQ(after.peer_common_name(), "second.example");
    // The connection from before the reload still works.
    ASSERT_TRUE(before.send_frame(wsrv::Opcode::Text, "still here"));
    ASSERT_TRUE(server->poll(3000ms).has_value());  // its open event
    const auto message = server->poll(3000ms);
    ASSERT_TRUE(message.has_value());
}

TEST(EmbeddedTls, AFailedReloadKeepsTheOldCertificateAndSaysWhy) {
    Certificate certificate;
    const auto server = created(tls_config(certificate));
    ASSERT_TRUE(server->start().has_value());

    certificate.dir.write("chain.pem", "not a certificate", kPublic);
    const auto reloaded = server->reload_tls();
    ASSERT_FALSE(reloaded.has_value());
    EXPECT_EQ(reloaded.error().code, ErrorCode::InvalidConfig);
    EXPECT_NE(server->tls_detail().find("chain.pem"), std::string::npos) << server->tls_detail();

    TlsClient client{server->port()};
    ASSERT_TRUE(client.upgrade());
    EXPECT_EQ(client.peer_common_name(), "first.example");  // the old certificate still serves
}

TEST(EmbeddedTls, ReloadOnAServerWithoutTlsIsAnError) {
    EmbeddedConfig config;
    ASSERT_TRUE(config.set("allow_origin", "https://app.example.com").has_value());
    const auto server = created(config);
    ASSERT_TRUE(server->start().has_value());
    const auto reloaded = server->reload_tls();
    ASSERT_FALSE(reloaded.has_value());
    EXPECT_EQ(reloaded.error().code, ErrorCode::InvalidArgument);
}

}  // namespace

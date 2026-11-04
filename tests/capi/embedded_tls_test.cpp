// SPDX-License-Identifier: Apache-2.0
// TLS through the embedded server (ADR-0019 sections 7 and 8): the config keys turn on wss://, a
// bad setup fails closed at start, and reload_tls() swaps the certificate for new connections.
#include "support/tls_client.hpp"
#include "support/tls_fixtures.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include "capi/embedded_server.hpp"
#include "tls/server_context.hpp"

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

// The embedded config bounds its list on its own (it cannot include tls/); keep both limits equal.
static_assert(wsrv::kMaxTlsSniSettings == wsrv::kMaxSniEntries);

/// "name=cert:key" for the tls_sni setting.
std::string sni_setting(const std::string& name, const Certificate& certificate) {
    return name + "=" + certificate.chain + ":" + certificate.key;
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

TEST(EmbeddedTls, SniChoosesTheCertificateByNameAndFallsBackToTheDefault) {
    const Certificate fallback{"default.example"};
    const Certificate app{"app.example.com"};
    const Certificate wild{"wild.example.com"};
    EmbeddedConfig config = tls_config(fallback);
    ASSERT_TRUE(config.set("tls_sni", sni_setting("app.example.com", app)).has_value());
    ASSERT_TRUE(config.set("tls_sni", sni_setting("*.example.org", wild)).has_value());
    const auto server = created(config);
    ASSERT_TRUE(server->start().has_value());

    const auto served_for = [&](const std::string& name) {
        TlsClient client{server->port(), {.server_name = name}};
        return client.peer_common_name();
    };
    EXPECT_EQ(served_for("app.example.com"), "app.example.com");
    EXPECT_EQ(served_for("x.example.org"), "wild.example.com");
    EXPECT_EQ(served_for("unknown.example"), "default.example");
    EXPECT_EQ(served_for("localhost"), "default.example");
}

TEST(EmbeddedTls, SniWithoutADefaultCertificateFailsClosed) {
    const Certificate app{"app.example.com"};
    EmbeddedConfig config;
    ASSERT_TRUE(config.set("tls_sni", sni_setting("app.example.com", app)).has_value());
    const auto server = created(config);
    const auto started = server->start();
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error().code, ErrorCode::InvalidConfig);
    EXPECT_NE(server->start_error().find("tls_cert"), std::string::npos) << server->start_error();
}

TEST(EmbeddedTls, AnUnusableSniFileFailsAtStartNamingTheFile) {
    const Certificate fallback;
    EmbeddedConfig config = tls_config(fallback);
    ASSERT_TRUE(
        config.set("tls_sni", "app.example.com=/nonexistent/sni-cert.pem:/nonexistent/k.pem")
            .has_value());
    const auto server = created(config);
    ASSERT_FALSE(server->start().has_value());
    EXPECT_NE(server->start_error().find("sni-cert.pem"), std::string::npos)
        << server->start_error();
}

TEST(EmbeddedTls, ReloadAlsoRereadsTheSniFiles) {
    const Certificate fallback;
    Certificate app{"before.example.com"};
    EmbeddedConfig config = tls_config(fallback);
    ASSERT_TRUE(config.set("tls_sni", sni_setting("app.example.com", app)).has_value());
    const auto server = created(config);
    ASSERT_TRUE(server->start().has_value());

    app.write("after.example.com");
    ASSERT_TRUE(server->reload_tls().has_value());
    TlsClient client{server->port(), {.server_name = "app.example.com"}};
    EXPECT_EQ(client.peer_common_name(), "after.example.com");
}

}  // namespace

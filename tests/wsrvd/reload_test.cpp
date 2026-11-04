// SPDX-License-Identifier: Apache-2.0
// What the daemon does on SIGHUP (ADR-0019 section 6): re-read the TLS files and say what happened.
#include "capi/embedded_server.hpp"
#include "support/tls_client.hpp"
#include "support/tls_fixtures.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "wsrvd/reload.hpp"

namespace {

using wsrv::EmbeddedConfig;
using wsrv::EmbeddedServer;
using wsrv::test::TempDir;
using wsrv::test::TlsClient;
using wsrv::wsrvd::reload_tls_on_hangup;

namespace fs = std::filesystem;
constexpr fs::perms kPublic = fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read;
constexpr fs::perms kPrivate = fs::perms::owner_read | fs::perms::owner_write;

TEST(Reload, WithoutTlsThereIsNothingToReloadAndTheDaemonSaysSo) {
    auto server = EmbeddedServer::create(EmbeddedConfig{});
    ASSERT_TRUE(server.has_value());
    ASSERT_TRUE((*server)->start().has_value());
    const auto outcome = reload_tls_on_hangup(**server);
    EXPECT_FALSE(outcome.reloaded);
    EXPECT_NE(outcome.line.find("not enabled"), std::string::npos) << outcome.line;
}

#if defined(WSRV_TEST_HAS_TLS)

struct Material {
    TempDir dir;
    std::string chain;
    std::string key;
    void write(const std::string& name) {
        const auto pem = wsrv::test::make_pem({.common_name = name});
        chain = dir.write("chain.pem", pem.certificate, kPublic);
        key = dir.write("key.pem", pem.key, kPrivate);
    }
};

TEST(Reload, ARenewedCertificateIsServedAfterTheHangupAndTheLineSaysSo) {
    Material material;
    material.write("old.example");
    EmbeddedConfig config;
    ASSERT_TRUE(config.set("tls_cert", material.chain).has_value());
    ASSERT_TRUE(config.set("tls_key", material.key).has_value());
    auto server = EmbeddedServer::create(config);
    ASSERT_TRUE(server.has_value());
    ASSERT_TRUE((*server)->start().has_value());

    material.write("new.example");
    const auto outcome = reload_tls_on_hangup(**server);
    EXPECT_TRUE(outcome.reloaded) << outcome.line;
    EXPECT_NE(outcome.line.find("reloaded"), std::string::npos) << outcome.line;

    TlsClient client{(*server)->port()};
    EXPECT_EQ(client.peer_common_name(), "new.example");
}

TEST(Reload, ABrokenRenewalKeepsTheOldCertificateAndTheLineNamesTheFile) {
    Material material;
    material.write("old.example");
    EmbeddedConfig config;
    ASSERT_TRUE(config.set("tls_cert", material.chain).has_value());
    ASSERT_TRUE(config.set("tls_key", material.key).has_value());
    auto server = EmbeddedServer::create(config);
    ASSERT_TRUE(server.has_value());
    ASSERT_TRUE((*server)->start().has_value());

    material.dir.write("chain.pem", "garbage", kPublic);
    const auto outcome = reload_tls_on_hangup(**server);
    EXPECT_FALSE(outcome.reloaded);
    EXPECT_NE(outcome.line.find("chain.pem"), std::string::npos) << outcome.line;
    EXPECT_NE(outcome.line.find("keeping"), std::string::npos) << outcome.line;

    TlsClient client{(*server)->port()};
    EXPECT_EQ(client.peer_common_name(), "old.example");
}

#endif  // WSRV_TEST_HAS_TLS

}  // namespace

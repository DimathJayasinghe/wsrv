// SPDX-License-Identifier: Apache-2.0
#include "tls/server_context.hpp"

#include "support/fakes.hpp"
#include "support/tls_fixtures.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "tls/tls_material.hpp"

namespace {

using wsrv::ErrorCode;
using wsrv::TlsConfig;
using wsrv::TlsServerContext;
using wsrv::test::CertSpec;
using wsrv::test::ClientOptions;
using wsrv::test::KeyKind;
using wsrv::test::PemPair;
using wsrv::test::TempDir;

namespace fs = std::filesystem;
constexpr fs::perms kPrivate = fs::perms::owner_read | fs::perms::owner_write;            // 0600
constexpr fs::perms kPublic = kPrivate | fs::perms::group_read | fs::perms::others_read;  // 0644

/// Certificate files in a temp dir, and the config that points at them.
struct Files {
    TempDir dir;
    TlsConfig config;

    void put(const PemPair& pair, fs::perms key_mode = kPrivate) {
        config.certificate_chain_file = dir.write("chain.pem", pair.certificate, kPublic);
        config.private_key_file = dir.write("key.pem", pair.key, key_mode);
    }
};

struct Loaded {
    wsrv::Result<std::unique_ptr<TlsServerContext>> result;
    wsrv::test::RecordingLogger logger;
};

wsrv::test::HandshakeOutcome handshake(const TlsServerContext& context,
                                       const ClientOptions& client = {}) {
    const auto material = context.current();
    return wsrv::test::handshake_in_memory(material->ctx.get(), client);
}

/// A server context with the defaults relaxed (TLS 1.0, CBC, RSA key exchange): a control that
/// proves a "refused" test used a client that really could have spoken the weak thing.
wsrv::test::SslCtxPtr permissive_server(const PemPair& pair) {
    wsrv::test::SslCtxPtr ctx{SSL_CTX_new(TLS_server_method())};
    SSL_CTX_set_security_level(ctx.get(), 0);
    SSL_CTX_set_min_proto_version(ctx.get(), TLS1_VERSION);
    SSL_CTX_set_cipher_list(ctx.get(), "ALL:@SECLEVEL=0");
    const wsrv::test::BioPtr cert_bio{
        BIO_new_mem_buf(pair.certificate.data(), static_cast<int>(pair.certificate.size()))};
    const wsrv::test::BioPtr key_bio{
        BIO_new_mem_buf(pair.key.data(), static_cast<int>(pair.key.size()))};
    const wsrv::test::X509Ptr cert{PEM_read_bio_X509(cert_bio.get(), nullptr, nullptr, nullptr)};
    const wsrv::test::PkeyPtr key{
        PEM_read_bio_PrivateKey(key_bio.get(), nullptr, nullptr, nullptr)};
    SSL_CTX_use_certificate(ctx.get(), cert.get());
    SSL_CTX_use_PrivateKey(ctx.get(), key.get());
    return ctx;
}

std::unique_ptr<TlsServerContext> must_load(const TlsConfig& config, wsrv::ILogger& logger) {
    auto loaded = TlsServerContext::load(config, logger);
    EXPECT_TRUE(loaded.has_value());
    return loaded ? std::move(*loaded) : nullptr;
}

// ---- a good certificate works, with the hardened profile ----------------------------------------

TEST(TlsServerContext, ServesAGoodEcdsaChainOverTls13) {
    Files files;
    files.put(wsrv::test::make_pem({}));
    wsrv::test::RecordingLogger logger;
    const auto context = must_load(files.config, logger);
    ASSERT_NE(context, nullptr);

    const auto outcome = handshake(*context);
    ASSERT_TRUE(outcome.ok());
    EXPECT_EQ(outcome.version, TLS1_3_VERSION);
    EXPECT_EQ(outcome.peer_cn, "localhost");
}

TEST(TlsServerContext, ServesAnRsaChain) {
    Files files;
    files.put(wsrv::test::make_pem({.key = KeyKind::Rsa2048}));
    wsrv::test::RecordingLogger logger;
    const auto context = must_load(files.config, logger);
    ASSERT_NE(context, nullptr);
    EXPECT_TRUE(handshake(*context).ok());
}

TEST(TlsServerContext, ATls12ClientGetsAnEcdheAeadSuite) {
    Files files;
    files.put(wsrv::test::make_pem({}));
    wsrv::test::RecordingLogger logger;
    const auto context = must_load(files.config, logger);
    ASSERT_NE(context, nullptr);

    const auto outcome = handshake(*context, {.max_version = TLS1_2_VERSION});
    ASSERT_TRUE(outcome.ok());
    EXPECT_EQ(outcome.version, TLS1_2_VERSION);
    EXPECT_TRUE(outcome.cipher.starts_with("ECDHE-")) << outcome.cipher;
    EXPECT_TRUE(outcome.cipher.find("GCM") != std::string::npos ||
                outcome.cipher.find("CHACHA20") != std::string::npos)
        << outcome.cipher;
}

TEST(TlsServerContext, RefusesProtocolsBelowTls12) {  // T12
    const PemPair pair = wsrv::test::make_pem({});
    const auto weak_client = ClientOptions{
        .min_version = TLS1_VERSION, .max_version = TLS1_1_VERSION, .allow_weak_security = true};
    // Control: a permissive server does speak TLS 1.1 to this client.
    EXPECT_TRUE(wsrv::test::handshake_in_memory(permissive_server(pair).get(), weak_client).ok());

    Files files;
    files.put(pair);
    wsrv::test::RecordingLogger logger;
    const auto context = must_load(files.config, logger);
    ASSERT_NE(context, nullptr);
    EXPECT_FALSE(handshake(*context, weak_client).ok());
}

TEST(TlsServerContext, RefusesCbcSuites) {  // T12: AEAD only
    const PemPair pair = wsrv::test::make_pem({});
    const auto cbc_client =
        ClientOptions{.max_version = TLS1_2_VERSION, .cipher_list = "ECDHE-ECDSA-AES128-SHA"};
    EXPECT_TRUE(wsrv::test::handshake_in_memory(permissive_server(pair).get(), cbc_client).ok());

    Files files;
    files.put(pair);
    wsrv::test::RecordingLogger logger;
    const auto context = must_load(files.config, logger);
    ASSERT_NE(context, nullptr);
    EXPECT_FALSE(handshake(*context, cbc_client).ok());
}

TEST(TlsServerContext, RefusesRsaKeyExchange) {  // T12: forward secrecy only
    const PemPair pair = wsrv::test::make_pem({.key = KeyKind::Rsa2048});
    const auto rsa_kx =
        ClientOptions{.max_version = TLS1_2_VERSION, .cipher_list = "AES128-GCM-SHA256"};
    EXPECT_TRUE(wsrv::test::handshake_in_memory(permissive_server(pair).get(), rsa_kx).ok());

    Files files;
    files.put(pair);
    wsrv::test::RecordingLogger logger;
    const auto context = must_load(files.config, logger);
    ASSERT_NE(context, nullptr);
    EXPECT_FALSE(handshake(*context, rsa_kx).ok());
}

TEST(TlsServerContext, PinsTheProtocolRangeAndTheHardeningOptions) {
    // The AEAD-only suite list already makes TLS 1.1 unreachable; these assertions keep the
    // intent visible and catch someone widening one setting while the other still hides it.
    Files files;
    files.put(wsrv::test::make_pem({}));
    wsrv::test::RecordingLogger logger;
    const auto context = must_load(files.config, logger);
    ASSERT_NE(context, nullptr);
    SSL_CTX* ctx = context->current()->ctx.get();

    EXPECT_EQ(SSL_CTX_get_min_proto_version(ctx), TLS1_2_VERSION);
    EXPECT_EQ(SSL_CTX_get_max_proto_version(ctx), TLS1_3_VERSION);
    const std::uint64_t options = SSL_CTX_get_options(ctx);
    for (const std::uint64_t flag : {SSL_OP_NO_COMPRESSION,
                                     SSL_OP_NO_RENEGOTIATION,
                                     SSL_OP_CIPHER_SERVER_PREFERENCE,
                                     SSL_OP_NO_TICKET}) {
        EXPECT_NE(options & flag, 0U) << "option bit " << flag;
    }
    // The transport relies on these two for its write retry rule (ADR-0019 section 2).
    const auto modes = static_cast<std::uint64_t>(SSL_CTX_get_mode(ctx));
    EXPECT_NE(modes & SSL_MODE_ENABLE_PARTIAL_WRITE, 0U);
    EXPECT_NE(modes & SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER, 0U);
    EXPECT_EQ(SSL_CTX_get_security_level(ctx), 2);
}

TEST(TlsServerContext, IssuesNoSessionTickets) {  // resumption is off (ADR-0019 section 5)
    Files files;
    files.put(wsrv::test::make_pem({}));
    wsrv::test::RecordingLogger logger;
    const auto context = must_load(files.config, logger);
    ASSERT_NE(context, nullptr);

    EXPECT_EQ(handshake(*context).tickets_received, 0);
    EXPECT_EQ(handshake(*context, {.max_version = TLS1_2_VERSION}).tickets_received, 0);
    // Control: the permissive server does issue them to a TLS 1.3 client.
    const PemPair pair = wsrv::test::make_pem({});
    EXPECT_GT(wsrv::test::handshake_in_memory(permissive_server(pair).get()).tickets_received, 0);
}

// ---- ALPN: HTTP/1.1 only ------------------------------------------------------------------------

TEST(TlsServerContext, AlpnChoosesHttp11) {
    Files files;
    files.put(wsrv::test::make_pem({}));
    wsrv::test::RecordingLogger logger;
    const auto context = must_load(files.config, logger);
    ASSERT_NE(context, nullptr);

    const auto outcome = handshake(*context, {.alpn = {"h2", "http/1.1"}});
    ASSERT_TRUE(outcome.ok());
    EXPECT_EQ(outcome.alpn, "http/1.1");
}

TEST(TlsServerContext, AlpnWithoutHttp11IsRefused) {  // cross-protocol confusion ("ALPACA")
    Files files;
    files.put(wsrv::test::make_pem({}));
    wsrv::test::RecordingLogger logger;
    const auto context = must_load(files.config, logger);
    ASSERT_NE(context, nullptr);

    EXPECT_FALSE(handshake(*context, {.alpn = {"h2"}}).ok());
    EXPECT_FALSE(handshake(*context, {.alpn = {"imap", "h2"}}).ok());
}

TEST(TlsServerContext, NoAlpnExtensionIsFine) {  // many non-browser clients send none
    Files files;
    files.put(wsrv::test::make_pem({}));
    wsrv::test::RecordingLogger logger;
    const auto context = must_load(files.config, logger);
    ASSERT_NE(context, nullptr);
    const auto outcome = handshake(*context);
    EXPECT_TRUE(outcome.ok());
    EXPECT_TRUE(outcome.alpn.empty());
}

// ---- what a bad certificate or key does at load (EC-T03)
// -----------------------------------------

struct Refusal {
    ErrorCode code;
    std::string context;
};

Refusal load_refusal(const TlsConfig& config) {
    wsrv::test::RecordingLogger logger;
    const auto loaded = TlsServerContext::load(config, logger);
    EXPECT_FALSE(loaded.has_value());
    for (const auto& entry : logger.entries) {  // a key never reaches a log
        EXPECT_EQ(entry.message.find("PRIVATE KEY"), std::string::npos) << entry.message;
    }
    return loaded ? Refusal{ErrorCode::Io, "loaded"}
                  : Refusal{loaded.error().code, std::string{loaded.error().context}};
}

TEST(TlsServerContext, RefusesAMissingCertificateFile) {
    Files files;
    files.put(wsrv::test::make_pem({}));
    fs::remove(files.config.certificate_chain_file);
    const Refusal refusal = load_refusal(files.config);
    EXPECT_EQ(refusal.code, ErrorCode::InvalidConfig);
    EXPECT_EQ(refusal.context, "tls: cannot read the certificate chain");
}

TEST(TlsServerContext, RefusesAMissingKeyFile) {
    Files files;
    files.put(wsrv::test::make_pem({}));
    fs::remove(files.config.private_key_file);
    const Refusal refusal = load_refusal(files.config);
    EXPECT_EQ(refusal.code, ErrorCode::InvalidConfig);
    EXPECT_EQ(refusal.context, "tls: cannot read the private key");
}

TEST(TlsServerContext, RefusesEmptyPaths) {
    EXPECT_EQ(load_refusal(TlsConfig{}).code, ErrorCode::InvalidConfig);
}

TEST(TlsServerContext, RefusesTextThatIsNotPem) {
    Files files;
    files.put(wsrv::test::make_pem({}));
    files.config.certificate_chain_file =
        files.dir.write("chain.pem", "this is not a certificate\n", kPublic);
    EXPECT_EQ(load_refusal(files.config).context, "tls: cannot read the certificate chain");
}

TEST(TlsServerContext, RefusesAKeyThatDoesNotMatchTheCertificate) {
    Files files;
    files.put(wsrv::test::make_pem({}));
    files.config.private_key_file =
        files.dir.write("other.pem", wsrv::test::make_pem({}).key, kPrivate);
    const Refusal refusal = load_refusal(files.config);
    EXPECT_EQ(refusal.code, ErrorCode::InvalidConfig);
    EXPECT_EQ(refusal.context, "tls: the private key does not match the certificate");
}

TEST(TlsServerContext, RefusesAnExpiredCertificate) {
    Files files;
    files.put(wsrv::test::make_pem({.not_before_offset_s = -7200, .not_after_offset_s = -3600}));
    const Refusal refusal = load_refusal(files.config);
    EXPECT_EQ(refusal.code, ErrorCode::InvalidConfig);
    EXPECT_EQ(refusal.context, "tls: the certificate has expired");
}

TEST(TlsServerContext, RefusesACertificateThatIsNotValidYet) {
    Files files;
    files.put(wsrv::test::make_pem({.not_before_offset_s = 3600, .not_after_offset_s = 7200}));
    const Refusal refusal = load_refusal(files.config);
    EXPECT_EQ(refusal.code, ErrorCode::InvalidConfig);
    EXPECT_EQ(refusal.context, "tls: the certificate is not valid yet");
}

TEST(TlsServerContext, RefusesAWeakRsaKey) {
    Files files;
    files.put(wsrv::test::make_pem({.key = KeyKind::Rsa1024}));
    const Refusal refusal = load_refusal(files.config);
    EXPECT_EQ(refusal.code, ErrorCode::InvalidConfig);
    EXPECT_EQ(refusal.context, "tls: cannot read the certificate chain");  // security level 2
}

TEST(TlsServerContext, RefusesAKeyFileThatEveryoneCanRead) {  // SECURITY_DESIGN rule 7
    Files files;
    files.put(wsrv::test::make_pem({}), fs::perms::owner_read | fs::perms::others_read);
    const Refusal refusal = load_refusal(files.config);
    EXPECT_EQ(refusal.code, ErrorCode::InvalidConfig);
    EXPECT_EQ(refusal.context, "tls: the private key file is readable by everyone");
}

TEST(TlsServerContext,
     AcceptsAKeyFileThatTheGroupCanRead) {  // the Debian ssl-cert convention (0640)
    Files files;
    files.put(wsrv::test::make_pem({}),
              fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read);
    wsrv::test::RecordingLogger logger;
    EXPECT_TRUE(TlsServerContext::load(files.config, logger).has_value());
}

// ---- reload (FR-T1, EC-T03) ---------------------------------------------------------------------

TEST(TlsServerContext, ReloadServesTheNewCertificateToNewConnections) {
    Files files;
    files.put(wsrv::test::make_pem({.common_name = "first"}));
    wsrv::test::RecordingLogger logger;
    const auto context = must_load(files.config, logger);
    ASSERT_NE(context, nullptr);
    const auto before = context->current();  // a connection that began under the old certificate
    EXPECT_EQ(handshake(*context).peer_cn, "first");

    files.put(wsrv::test::make_pem({.common_name = "second"}));
    ASSERT_TRUE(context->reload().has_value());

    EXPECT_EQ(handshake(*context).peer_cn, "second");
    EXPECT_NE(context->current(), before);
    // The old context stays usable by whoever still holds it.
    EXPECT_EQ(wsrv::test::handshake_in_memory(before->ctx.get()).peer_cn, "first");
}

TEST(TlsServerContext, AFailedReloadKeepsTheOldCertificate) {  // EC-T03
    Files files;
    files.put(wsrv::test::make_pem({.common_name = "good"}));
    wsrv::test::RecordingLogger logger;
    const auto context = must_load(files.config, logger);
    ASSERT_NE(context, nullptr);
    const auto before = context->current();

    files.put(wsrv::test::make_pem(
        {.common_name = "stale", .not_before_offset_s = -7200, .not_after_offset_s = -3600}));
    const auto reloaded = context->reload();
    ASSERT_FALSE(reloaded.has_value());
    EXPECT_EQ(reloaded.error().code, ErrorCode::InvalidConfig);

    EXPECT_EQ(context->current(), before);
    EXPECT_EQ(handshake(*context).peer_cn, "good");
}

TEST(TlsServerContext, AReloadFromADeletedFileKeepsTheOldCertificate) {
    Files files;
    files.put(wsrv::test::make_pem({.common_name = "good"}));
    wsrv::test::RecordingLogger logger;
    const auto context = must_load(files.config, logger);
    ASSERT_NE(context, nullptr);
    fs::remove(files.config.private_key_file);

    EXPECT_FALSE(context->reload().has_value());
    EXPECT_EQ(handshake(*context).peer_cn, "good");
}

}  // namespace

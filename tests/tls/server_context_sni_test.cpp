// SPDX-License-Identifier: Apache-2.0
// Choosing the certificate by SNI (ADR-0019 section 7): matching, fallback, hostile names, the
// shared hardened profile, limits and reload.
#include "support/fakes.hpp"
#include "support/tls_fixtures.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "tls/server_context.hpp"
#include "tls/tls_material.hpp"

namespace {

using wsrv::ErrorCode;
using wsrv::TlsConfig;
using wsrv::TlsServerContext;
using wsrv::TlsSniEntry;
using wsrv::test::ClientOptions;
using wsrv::test::PemPair;
using wsrv::test::TempDir;

namespace fs = std::filesystem;
constexpr fs::perms kPrivate = fs::perms::owner_read | fs::perms::owner_write;
constexpr fs::perms kPublic = kPrivate | fs::perms::group_read | fs::perms::others_read;

/// A directory of certificates, each named after its common name so a test can see which one was
/// served. `entry()` adds an SNI entry for a pattern using a certificate with that common name.
struct Site {
    TempDir dir;
    TlsConfig config;

    Site() {
        set_default("default.example");
    }
    void set_default(const std::string& common_name) {
        const PemPair pair = wsrv::test::make_pem({.common_name = common_name});
        config.certificate_chain_file = dir.write("default-chain.pem", pair.certificate, kPublic);
        config.private_key_file = dir.write("default-key.pem", pair.key, kPrivate);
    }
    TlsSniEntry make_entry(const std::string& pattern, const std::string& common_name) {
        const PemPair pair = wsrv::test::make_pem({.common_name = common_name});
        const std::string stem = common_name + "-" + std::to_string(config.sni.size());
        return {pattern,
                dir.write(stem + "-chain.pem", pair.certificate, kPublic),
                dir.write(stem + "-key.pem", pair.key, kPrivate)};
    }
    void entry(const std::string& pattern, const std::string& common_name) {
        config.sni.push_back(make_entry(pattern, common_name));
    }
};

std::unique_ptr<TlsServerContext> must_load(const TlsConfig& config, wsrv::ILogger& logger) {
    auto loaded = TlsServerContext::load(config, logger);
    EXPECT_TRUE(loaded.has_value());
    return loaded ? std::move(*loaded) : nullptr;
}

std::string served_for(const TlsServerContext& context, const std::string& name) {
    ClientOptions client;
    client.server_name = name;
    const auto outcome = wsrv::test::handshake_in_memory(context.current()->ctx.get(), client);
    return outcome.ok() ? outcome.peer_cn : "<handshake failed>";
}

// ---- choosing
// ------------------------------------------------------------------------------------

TEST(TlsSni, TheCertificateFollowsTheNameTheClientAsksFor) {
    Site site;
    site.entry("a.example.com", "exact-cert");
    site.entry("*.wild.example", "wild-cert");
    wsrv::test::RecordingLogger logger;
    const auto context = must_load(site.config, logger);
    ASSERT_NE(context, nullptr);

    EXPECT_EQ(served_for(*context, "a.example.com"), "exact-cert");
    EXPECT_EQ(served_for(*context, "x.wild.example"), "wild-cert");
    EXPECT_EQ(served_for(*context, "A.EXAMPLE.Com"), "exact-cert");  // DNS names are case-blind
}

TEST(TlsSni, AnyOtherNameOrNoNameGetsTheDefaultCertificateNeverAnError) {
    Site site;
    site.entry("a.example.com", "exact-cert");
    site.entry("*.wild.example", "wild-cert");
    wsrv::test::RecordingLogger logger;
    const auto context = must_load(site.config, logger);
    ASSERT_NE(context, nullptr);

    EXPECT_EQ(served_for(*context, ""), "default.example");                  // no SNI at all
    EXPECT_EQ(served_for(*context, "other.example"), "default.example");     // unknown name
    EXPECT_EQ(served_for(*context, "wild.example"), "default.example");      // zero labels
    EXPECT_EQ(served_for(*context, "x.y.wild.example"), "default.example");  // two labels
    EXPECT_EQ(served_for(*context, "10.0.0.1"), "default.example");          // an address literal
}

TEST(TlsSni, AnExactEntryBeatsAWildcardThatAlsoMatches) {
    Site site;
    site.entry("*.example.com", "wild-cert");  // listed first on purpose
    site.entry("www.example.com", "exact-cert");
    wsrv::test::RecordingLogger logger;
    const auto context = must_load(site.config, logger);
    ASSERT_NE(context, nullptr);

    EXPECT_EQ(served_for(*context, "www.example.com"), "exact-cert");
    EXPECT_EQ(served_for(*context, "api.example.com"), "wild-cert");
}

TEST(TlsSni, AHostileNameServesTheDefaultAndNeverAnEntry) {
    Site site;
    site.entry("*.example.com", "wild-cert");
    wsrv::test::RecordingLogger logger;
    const auto context = must_load(site.config, logger);
    ASSERT_NE(context, nullptr);

    EXPECT_EQ(served_for(*context, "bad name.example.com"), "default.example");  // a space
    EXPECT_EQ(served_for(*context, "a.example.com."), "default.example");        // trailing dot
    EXPECT_EQ(served_for(*context, "*.example.com"), "default.example");         // a wildcard
    EXPECT_EQ(served_for(*context, std::string(255, 'a')), "default.example");   // too long
    // Nothing a client sent may reach the log (log injection, T14).
    for (const auto& entry : logger.entries) {
        EXPECT_EQ(entry.message.find("bad name"), std::string::npos) << entry.message;
    }
}

// ---- every certificate gets the same hardened profile
// -----------------------------------------------

TEST(TlsSni, AnEntryIsServedWithTheSameHardenedProfile) {
    Site site;
    site.entry("a.example.com", "exact-cert");
    wsrv::test::RecordingLogger logger;
    const auto context = must_load(site.config, logger);
    ASSERT_NE(context, nullptr);
    const auto ctx = context->current()->ctx.get();

    ClientOptions tls12;
    tls12.server_name = "a.example.com";
    tls12.max_version = TLS1_2_VERSION;
    const auto old = wsrv::test::handshake_in_memory(ctx, tls12);
    ASSERT_TRUE(old.ok());
    EXPECT_EQ(old.peer_cn, "exact-cert");
    EXPECT_NE(old.cipher.find("GCM"), std::string::npos) << old.cipher;  // an AEAD suite

    ClientOptions legacy;
    legacy.server_name = "a.example.com";
    legacy.max_version = TLS1_1_VERSION;
    legacy.allow_weak_security = true;
    EXPECT_FALSE(wsrv::test::handshake_in_memory(ctx, legacy).ok());  // no TLS below 1.2

    ClientOptions h2;
    h2.server_name = "a.example.com";
    h2.alpn = {"h2"};
    EXPECT_FALSE(wsrv::test::handshake_in_memory(ctx, h2).ok());  // ALPN policy still applies

    ClientOptions http;
    http.server_name = "a.example.com";
    http.alpn = {"http/1.1"};
    const auto served = wsrv::test::handshake_in_memory(ctx, http);
    ASSERT_TRUE(served.ok());
    EXPECT_EQ(served.alpn, "http/1.1");
    EXPECT_EQ(served.tickets_received, 0);  // still no resumption
}

// ---- configuration is checked like the default certificate is
// ---------------------------------------

TEST(TlsSni, SixtyFourEntriesAreAllowedAndSixtyFiveAreNot) {
    Site site;
    const TlsSniEntry shared = site.make_entry("n0.example.com", "bulk-cert");
    for (int i = 0; i < 64; ++i) {
        site.config.sni.push_back({"n" + std::to_string(i) + ".example.com",
                                   shared.certificate_chain_file,
                                   shared.private_key_file});
    }
    wsrv::test::RecordingLogger logger;
    EXPECT_NE(must_load(site.config, logger), nullptr);

    site.config.sni.push_back(
        {"n64.example.com", shared.certificate_chain_file, shared.private_key_file});
    wsrv::test::RecordingLogger refused_logger;
    const auto refused = TlsServerContext::load(site.config, refused_logger);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code, ErrorCode::InvalidConfig);
}

TEST(TlsSni, DuplicateAndMalformedPatternsAreRefused) {
    for (const char* bad : {"", "*", "*.com", "a..b.com", "bad name.com", "foo*.example.com"}) {
        Site site;
        site.entry("ok.example.com", "fine-cert");
        wsrv::test::RecordingLogger control_logger;
        ASSERT_NE(must_load(site.config, control_logger), nullptr) << "control failed";  // loads

        site.entry(bad, "bad-cert");
        wsrv::test::RecordingLogger logger;
        const auto refused = TlsServerContext::load(site.config, logger);
        ASSERT_FALSE(refused.has_value()) << '"' << bad << '"';
        EXPECT_EQ(refused.error().code, ErrorCode::InvalidConfig);
    }
    Site site;
    site.entry("same.example.com", "first-cert");
    site.entry("SAME.example.com", "second-cert");  // the same name once lower-cased
    wsrv::test::RecordingLogger logger;
    EXPECT_FALSE(TlsServerContext::load(site.config, logger).has_value());
}

TEST(TlsSni, AnEntryIsHeldToTheSameRulesAsTheDefaultCertificate) {
    struct Case {
        const char* what;
        void (*spoil)(TlsSniEntry&, Site&);
    };
    const Case cases[] = {
        {"missing certificate", [](TlsSniEntry& e, Site&) { e.certificate_chain_file += ".nope"; }},
        {"missing key", [](TlsSniEntry& e, Site&) { e.private_key_file += ".nope"; }},
        {"key open to everyone",
         [](TlsSniEntry& e, Site&) { fs::permissions(e.private_key_file, kPublic); }},
        {"key of another certificate",
         [](TlsSniEntry& e, Site& site) {
             const PemPair other = wsrv::test::make_pem({.common_name = "other"});
             e.private_key_file = site.dir.write("other-key.pem", other.key, kPrivate);
         }},
        {"expired certificate",
         [](TlsSniEntry& e, Site& site) {
             const PemPair old =
                 wsrv::test::make_pem({.common_name = "old", .not_after_offset_s = -60});
             e.certificate_chain_file = site.dir.write("old-chain.pem", old.certificate, kPublic);
             e.private_key_file = site.dir.write("old-key.pem", old.key, kPrivate);
         }},
    };
    for (const Case& c : cases) {
        Site site;
        TlsSniEntry entry = site.make_entry("a.example.com", "entry-cert");
        c.spoil(entry, site);
        site.config.sni.push_back(entry);
        wsrv::test::RecordingLogger logger;
        const auto refused = TlsServerContext::load(site.config, logger);
        ASSERT_FALSE(refused.has_value()) << c.what;
        EXPECT_EQ(refused.error().code, ErrorCode::InvalidConfig) << c.what;
    }
}

// ---- reload
// ------------------------------------------------------------------------------------------

TEST(TlsSni, ReloadRereadsTheEntriesToo) {
    Site site;
    site.entry("a.example.com", "before");
    wsrv::test::RecordingLogger logger;
    const auto context = must_load(site.config, logger);
    ASSERT_NE(context, nullptr);
    ASSERT_EQ(served_for(*context, "a.example.com"), "before");

    const PemPair renewed = wsrv::test::make_pem({.common_name = "after"});
    site.dir.write("before-0-chain.pem", renewed.certificate, kPublic);
    site.dir.write("before-0-key.pem", renewed.key, kPrivate);
    ASSERT_TRUE(context->reload().has_value());
    EXPECT_EQ(served_for(*context, "a.example.com"), "after");
}

TEST(TlsSni, AReloadThatBreaksOneEntryKeepsEverythingAsItWas) {  // all or nothing
    Site site;
    site.entry("a.example.com", "entry-before");
    wsrv::test::RecordingLogger logger;
    const auto context = must_load(site.config, logger);
    ASSERT_NE(context, nullptr);

    const PemPair renewed = wsrv::test::make_pem({.common_name = "default-after"});
    site.dir.write("default-chain.pem", renewed.certificate, kPublic);  // the default is fine ...
    site.dir.write("default-key.pem", renewed.key, kPrivate);
    site.dir.write("entry-before-0-chain.pem", "garbage", kPublic);  // ... the entry is not
    EXPECT_FALSE(context->reload().has_value());

    EXPECT_EQ(served_for(*context, "a.example.com"), "entry-before");
    EXPECT_EQ(served_for(*context, ""), "default.example");  // the new default was not half-applied
}

}  // namespace

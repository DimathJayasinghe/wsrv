// SPDX-License-Identifier: Apache-2.0
#include "tls/server_context.hpp"

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <new>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "tls/sni.hpp"
#include "tls/tls_material.hpp"

namespace wsrv {

namespace {

namespace fs = std::filesystem;

// The profile of SECURITY_DESIGN section 5 / ADR-0019 section 5. One place, no knobs.
constexpr const char* kTls13Suites =
    "TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256";
// TLS 1.2: ECDHE (forward secrecy) with an AEAD cipher only: no CBC, no RSA key exchange.
constexpr const char* kTls12Ciphers = "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
                                      "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:"
                                      "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256";
constexpr const char* kGroups = "X25519:prime256v1:secp384r1";
constexpr int kSecurityLevel = 2;  // RSA/DH at least 2048 bits, no SHA-1 signatures, no 3DES/RC4

// ALPN wire format: a length byte, then the name. WebSocket upgrades are HTTP/1.1 (RFC 6455
// section 4); refusing a client that offers only other protocols (h2, imap, ...) stops it using
// our port for a protocol we do not speak (cross-protocol attacks).
constexpr std::array<unsigned char, 9> kAlpnHttp11{8, 'h', 't', 't', 'p', '/', '1', '.', '1'};

// NOLINTNEXTLINE(readability-function-size): the signature is fixed by OpenSSL
int select_alpn(SSL* /*ssl*/,
                const unsigned char** out,
                unsigned char* out_length,
                const unsigned char* offered,
                unsigned int offered_length,
                void* /*arg*/) {
    unsigned char* chosen = nullptr;
    // The third and fourth arguments are OUR list; OpenSSL looks for the first entry of it that
    // the client also offered.
    const int outcome = SSL_select_next_proto(&chosen,
                                              out_length,
                                              kAlpnHttp11.data(),
                                              static_cast<unsigned int>(kAlpnHttp11.size()),
                                              offered,
                                              offered_length);
    if (outcome != OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_ALERT_FATAL;  // alert no_application_protocol
    }
    *out = chosen;
    return SSL_TLSEXT_ERR_OK;
}

/// We never prompt for a passphrase (that would block a server on a terminal): an encrypted key
/// is simply unreadable.
int no_passphrase(char* /*buffer*/, int /*size*/, int /*rwflag*/, void* /*userdata*/) {
    return 0;
}

/// The reason OpenSSL gave for the last failure, as text for the log (never for an Error: those
/// carry fixed text only). Empties the error queue so the failure is not blamed on a later call.
std::string take_openssl_reason() {
    std::array<char, 256> text{};
    const unsigned long code = ERR_peek_last_error();
    if (code != 0) {
        ERR_error_string_n(code, text.data(), text.size());
    }
    ERR_clear_error();
    return code != 0 ? std::string{text.data()} : std::string{"no reason given"};
}

/// Log a refusal (path and OpenSSL's reason) and make the Error. `context` is a literal.
std::unexpected<Error>
refuse(ILogger& logger, std::string_view what, const std::string& path, std::string_view context) {
    std::string line = "tls: ";
    line += what;
    line += " '";
    line += path;
    line += "': ";
    line += take_openssl_reason();
    logger.log(LogLevel::Error, "tls_config", line);
    return fail(ErrorCode::InvalidConfig, context);
}

/// Protocol versions, suites, groups and options. See the constants above for the why of each.
bool apply_profile(SSL_CTX* ctx) {
    constexpr std::uint64_t kOptions = SSL_OP_NO_COMPRESSION      // CRIME
                                       | SSL_OP_NO_RENEGOTIATION  // EC-T05
                                       | SSL_OP_CIPHER_SERVER_PREFERENCE | SSL_OP_NO_TICKET;
    constexpr long kModes = SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER |
                            SSL_MODE_RELEASE_BUFFERS;
    SSL_CTX_set_security_level(ctx, kSecurityLevel);
    SSL_CTX_set_options(ctx, kOptions);
    SSL_CTX_set_mode(ctx, kModes);
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);  // no resumption (ADR-0019 section 5)
    SSL_CTX_set_alpn_select_cb(ctx, &select_alpn, nullptr);
    SSL_CTX_set_default_passwd_cb(ctx, &no_passphrase);
    return SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) == 1 &&
           SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION) == 1 &&
           SSL_CTX_set_ciphersuites(ctx, kTls13Suites) == 1 &&
           SSL_CTX_set_cipher_list(ctx, kTls12Ciphers) == 1 &&
           SSL_CTX_set1_groups_list(ctx, kGroups) == 1 && SSL_CTX_set_num_tickets(ctx, 0) == 1;
}

/// EC-T03: a certificate that is not valid now would only produce failed handshakes later.
Result<void> check_validity(SSL_CTX* ctx, ILogger& logger, const std::string& path) {
    const X509* leaf = SSL_CTX_get0_certificate(ctx);
    if (leaf == nullptr) {
        return refuse(logger, "no certificate in", path, "tls: cannot read the certificate chain");
    }
    // X509_cmp_current_time: -1 if the time is in the past, 1 if in the future, 0 if unreadable.
    const int after = X509_cmp_current_time(X509_get0_notAfter(leaf));
    const int before = X509_cmp_current_time(X509_get0_notBefore(leaf));
    if (after == 0 || before == 0) {
        return refuse(
            logger, "unreadable validity in", path, "tls: cannot read the certificate chain");
    }
    if (after < 0) {
        return refuse(logger, "expired certificate", path, "tls: the certificate has expired");
    }
    if (before > 0) {
        return refuse(
            logger, "certificate not valid yet", path, "tls: the certificate is not valid yet");
    }
    return {};
}

/// SECURITY_DESIGN rule 7: a key file that "others" can open is a leaked key. The group may read
/// it (the Debian `ssl-cert` convention is mode 0640).
Result<void> check_key_file(ILogger& logger, const std::string& path) {
    std::error_code error;
    const fs::file_status status = fs::status(path, error);  // follows symlinks (certbot uses them)
    if (error || !fs::is_regular_file(status)) {
        return refuse(logger, "cannot read", path, "tls: cannot read the private key");
    }
    if ((status.permissions() & fs::perms::others_all) != fs::perms::none) {
        return refuse(logger,
                      "key file open to everyone",
                      path,
                      "tls: the private key file is readable by everyone");
    }
    return {};
}

Result<void> load_key(SSL_CTX* ctx, ILogger& logger, const std::string& path) {
    if (const Result<void> permitted = check_key_file(logger, path); !permitted) {
        return permitted;
    }
    const std::unique_ptr<BIO, decltype(&BIO_free_all)> file{BIO_new_file(path.c_str(), "r"),
                                                             &BIO_free_all};
    const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key{
        file ? PEM_read_bio_PrivateKey(file.get(), nullptr, &no_passphrase, nullptr) : nullptr,
        &EVP_PKEY_free};
    if (!key) {
        return refuse(logger, "cannot read", path, "tls: cannot read the private key");
    }
    if (X509_check_private_key(SSL_CTX_get0_certificate(ctx), key.get()) != 1) {
        return refuse(logger,
                      "key does not match the certificate",
                      path,
                      "tls: the private key does not match the certificate");
    }
    if (SSL_CTX_use_PrivateKey(ctx, key.get()) != 1) {
        return refuse(logger, "cannot use", path, "tls: cannot read the private key");
    }
    return {};
}

/// One complete context: the hardened profile, the certificate chain (valid now) and its key.
Result<UniqueSslCtx>
build_context(const std::string& chain_file, const std::string& key_file, ILogger& logger) {
    if (chain_file.empty() || key_file.empty()) {
        logger.log(LogLevel::Error,
                   "tls_config",
                   "tls: a certificate chain file and a private key file are required");
        return fail(ErrorCode::InvalidConfig, "tls: a certificate and a key file are required");
    }
    UniqueSslCtx ctx{SSL_CTX_new(TLS_server_method())};
    if (!ctx || !apply_profile(ctx.get())) {
        static_cast<void>(take_openssl_reason());
        return fail(ErrorCode::InvalidConfig, "tls: cannot apply the hardened profile");
    }
    if (SSL_CTX_use_certificate_chain_file(ctx.get(), chain_file.c_str()) != 1) {
        return refuse(logger,
                      "cannot use the certificate chain",
                      chain_file,
                      "tls: cannot read the certificate chain");
    }
    if (const Result<void> valid = check_validity(ctx.get(), logger, chain_file); !valid) {
        return std::unexpected{valid.error()};
    }
    if (const Result<void> keyed = load_key(ctx.get(), logger, key_file); !keyed) {
        return std::unexpected{keyed.error()};
    }
    return ctx;
}

/// The entry for `host` (already normalised): an exact pattern first, otherwise the most specific
/// wildcard (the longest suffix). Nothing if no entry fits.
const TlsSniContext* find_entry(const TlsMaterial& material, std::string_view host) {
    const TlsSniContext* wildcard = nullptr;
    for (const TlsSniContext& entry : material.entries) {
        if (!sni::matches(entry.pattern, host)) {
            continue;
        }
        if (!entry.pattern.starts_with("*.")) {
            return &entry;
        }
        if (wildcard == nullptr || entry.pattern.size() > wildcard->pattern.size()) {
            wildcard = &entry;
        }
    }
    return wildcard;
}

/// OpenSSL calls this while it reads the ClientHello, before it picks a certificate. We only ever
/// swap the SSL to another context of ours; every outcome (no name, a name we cannot use, a name
/// nobody has a certificate for) is "carry on with the default": a client is never refused for its
/// SNI (clients that send an address, or nothing, must still work). The name is peer-controlled:
/// it is length- and character-checked before it is compared, and it is never logged.
int select_certificate(SSL* ssl, int* /*alert*/, void* arg) {
    const auto* material = static_cast<const TlsMaterial*>(arg);
    const char* raw = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    if (raw == nullptr || material->entries.empty()) {
        return SSL_TLSEXT_ERR_OK;
    }
    const std::string_view wire{raw, ::strnlen(raw, sni::kMaxNameLength + 1)};
    if (const std::optional<std::string> host = sni::normalise_host(wire)) {
        if (const TlsSniContext* entry = find_entry(*material, *host)) {
            static_cast<void>(SSL_set_SSL_CTX(ssl, entry->ctx.get()));
        }
    }
    return SSL_TLSEXT_ERR_OK;
}

/// The entries of a configuration: names normalised and unique, at most kMaxSniEntries, every
/// certificate held to the rules of the default one.
Result<void> build_entries(const TlsConfig& config, ILogger& logger, TlsMaterial& material) {
    if (config.sni.size() > kMaxSniEntries) {
        logger.log(LogLevel::Error, "tls_config", "tls: too many SNI entries");
        return fail(ErrorCode::InvalidConfig, "tls: too many SNI entries");
    }
    std::unordered_set<std::string> seen;
    material.entries.reserve(config.sni.size());
    for (const TlsSniEntry& entry : config.sni) {
        auto pattern = sni::normalise_pattern(entry.name);
        if (!pattern) {
            logger.log(LogLevel::Error,
                       "tls_config",
                       "tls: an SNI name is not a host name or a *.wildcard");
            return fail(ErrorCode::InvalidConfig, "tls: an SNI name is not valid");
        }
        if (!seen.insert(*pattern).second) {
            logger.log(LogLevel::Error, "tls_config", "tls: the same SNI name is configured twice");
            return fail(ErrorCode::InvalidConfig, "tls: an SNI name is listed twice");
        }
        auto ctx = build_context(entry.certificate_chain_file, entry.private_key_file, logger);
        if (!ctx) {
            return std::unexpected{ctx.error()};
        }
        material.entries.push_back({std::move(*pattern), std::move(*ctx)});
    }
    return {};
}

Result<std::shared_ptr<const TlsMaterial>> build(const TlsConfig& config, ILogger& logger) {
    auto ctx = build_context(config.certificate_chain_file, config.private_key_file, logger);
    if (!ctx) {
        return std::unexpected{ctx.error()};
    }
    auto material = std::make_shared<TlsMaterial>();
    material->ctx = std::move(*ctx);
    if (const Result<void> entries = build_entries(config, logger, *material); !entries) {
        return std::unexpected{entries.error()};
    }
    if (!material->entries.empty()) {
        // The callback finds the entries through this pointer. The material never moves after this
        // point (it lives in a shared_ptr) and the SSL objects that use `ctx` are owned by a
        // TlsTransport that keeps the material alive.
        SSL_CTX_ctrl(material->ctx.get(), SSL_CTRL_SET_TLSEXT_SERVERNAME_ARG, 0, material.get());
        // OpenSSL's callback slot is a generic function pointer; this one cast is the API's shape.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const auto slot = reinterpret_cast<void (*)()>(&select_certificate);
        SSL_CTX_callback_ctrl(material->ctx.get(), SSL_CTRL_SET_TLSEXT_SERVERNAME_CB, slot);
    }
    return std::shared_ptr<const TlsMaterial>{std::move(material)};
}

}  // namespace

TlsServerContext::TlsServerContext(Key /*key*/,
                                   TlsConfig config,
                                   ILogger& logger,
                                   std::shared_ptr<const TlsMaterial> material) noexcept
    : config_{std::move(config)}, logger_{&logger}, current_{std::move(material)} {}

TlsServerContext::~TlsServerContext() = default;

Result<std::unique_ptr<TlsServerContext>> TlsServerContext::load(const TlsConfig& config,
                                                                 ILogger& logger) {
    try {
        auto material = build(config, logger);
        if (!material) {
            return std::unexpected{material.error()};
        }
        return std::make_unique<TlsServerContext>(Key{}, config, logger, std::move(*material));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "tls: out of memory");
    }
}

Result<void> TlsServerContext::reload() {
    try {
        auto material = build(config_, *logger_);
        if (!material) {
            logger_->log(LogLevel::Error,
                         "tls_reload_failed",
                         "tls: reload failed, keeping the certificate in use");
            return std::unexpected{material.error()};
        }
        {
            const std::lock_guard lock{mutex_};
            current_ = std::move(*material);
        }
        logger_->log(LogLevel::Info, "tls_reloaded", "tls: certificate reloaded");
        return {};
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "tls: out of memory");
    }
}

std::shared_ptr<const TlsMaterial> TlsServerContext::current() const {
    const std::lock_guard lock{mutex_};
    return current_;
}

}  // namespace wsrv

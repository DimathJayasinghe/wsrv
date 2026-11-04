// SPDX-License-Identifier: Apache-2.0
#pragma once

// Test support for TLS (ADR-0019 section 9): certificates are made in the test process with the
// OpenSSL API, so no file in the repository holds a key and no tool has to be downloaded. Tests
// only: this header is never included by production code.

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace wsrv::test {

struct PkeyFree {
    void operator()(EVP_PKEY* p) const noexcept {
        EVP_PKEY_free(p);
    }
};
struct X509Free {
    void operator()(X509* p) const noexcept {
        X509_free(p);
    }
};
struct BioFree {
    void operator()(BIO* p) const noexcept {
        BIO_free_all(p);
    }
};
struct SslFree {
    void operator()(SSL* p) const noexcept {
        SSL_free(p);
    }
};
struct SslCtxFree {
    void operator()(SSL_CTX* p) const noexcept {
        SSL_CTX_free(p);
    }
};
using PkeyPtr = std::unique_ptr<EVP_PKEY, PkeyFree>;
using X509Ptr = std::unique_ptr<X509, X509Free>;
using BioPtr = std::unique_ptr<BIO, BioFree>;
using SslPtr = std::unique_ptr<SSL, SslFree>;
using SslCtxPtr = std::unique_ptr<SSL_CTX, SslCtxFree>;

enum class KeyKind { EcP256, Rsa2048, Rsa1024 };

/// What a generated certificate looks like. Offsets are in seconds from now.
struct CertSpec {
    std::string common_name = "localhost";
    std::vector<std::string> dns_names{"localhost"};
    KeyKind key = KeyKind::EcP256;
    long not_before_offset_s = -3600;  // valid since an hour ago ...
    long not_after_offset_s = 86400;   // ... for one more day
    long serial = 1;
};

struct PemPair {
    std::string certificate;  // PEM
    std::string key;          // PEM, PKCS#8, unencrypted
};

inline std::runtime_error tls_fixture_error(const char* what) {
    ERR_clear_error();
    return std::runtime_error{what};
}

inline PkeyPtr make_key(KeyKind kind) {
    EVP_PKEY* key = nullptr;
    switch (kind) {
    case KeyKind::EcP256:
        key = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256");
        break;
    case KeyKind::Rsa2048:
        key = EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", std::size_t{2048});
        break;
    case KeyKind::Rsa1024:
        key = EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", std::size_t{1024});
        break;
    }
    if (key == nullptr) {
        throw tls_fixture_error("key generation failed");
    }
    return PkeyPtr{key};
}

inline std::string bio_to_string(BIO& bio) {
    char* data = nullptr;
    const long size = BIO_get_mem_data(&bio, &data);
    return size > 0 ? std::string{data, static_cast<std::size_t>(size)} : std::string{};
}

/// A self-signed certificate and its key.
inline PemPair make_pem(const CertSpec& spec) {
    const PkeyPtr key = make_key(spec.key);
    const X509Ptr cert{X509_new()};
    if (!cert) {
        throw tls_fixture_error("X509_new failed");
    }
    X509_set_version(cert.get(), 2);  // v3
    ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), spec.serial);
    X509_gmtime_adj(X509_getm_notBefore(cert.get()), spec.not_before_offset_s);
    X509_gmtime_adj(X509_getm_notAfter(cert.get()), spec.not_after_offset_s);
    X509_set_pubkey(cert.get(), key.get());
    X509_NAME* name = X509_get_subject_name(cert.get());
    X509_NAME_add_entry_by_txt(name,
                               "CN",
                               MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(spec.common_name.c_str()),
                               -1,
                               -1,
                               0);
    X509_set_issuer_name(cert.get(), name);  // self-signed
    if (!spec.dns_names.empty()) {
        std::string alt;
        for (const std::string& dns : spec.dns_names) {
            alt += (alt.empty() ? "DNS:" : ",DNS:") + dns;
        }
        X509V3_CTX context;
        X509V3_set_ctx_nodb(&context);
        X509V3_set_ctx(&context, cert.get(), cert.get(), nullptr, nullptr, 0);
        X509_EXTENSION* extension =
            X509V3_EXT_nconf_nid(nullptr, &context, NID_subject_alt_name, alt.c_str());
        if (extension == nullptr) {
            throw tls_fixture_error("subjectAltName failed");
        }
        X509_add_ext(cert.get(), extension, -1);
        X509_EXTENSION_free(extension);
    }
    if (X509_sign(cert.get(), key.get(), EVP_sha256()) == 0) {
        throw tls_fixture_error("X509_sign failed");
    }

    const BioPtr cert_bio{BIO_new(BIO_s_mem())};
    const BioPtr key_bio{BIO_new(BIO_s_mem())};
    if (PEM_write_bio_X509(cert_bio.get(), cert.get()) != 1 ||
        PEM_write_bio_PrivateKey(key_bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) !=
            1) {
        throw tls_fixture_error("PEM encoding failed");
    }
    return {bio_to_string(*cert_bio), bio_to_string(*key_bio)};
}

/// A directory that exists for the lifetime of the object.
class TempDir {
public:
    TempDir() {
        std::string pattern = (std::filesystem::temp_directory_path() / "wsrv-tls-XXXXXX").string();
        if (mkdtemp(pattern.data()) == nullptr) {
            throw std::runtime_error{"mkdtemp failed"};
        }
        path_ = pattern;
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    /// Write (or replace, in place) `name` with `content` and the given permissions.
    std::string
    write(const std::string& name, const std::string& content, std::filesystem::perms mode) const {
        const std::filesystem::path file = path_ / name;
        {
            std::ofstream out{file, std::ios::binary | std::ios::trunc};
            out << content;
        }
        std::filesystem::permissions(file, mode, std::filesystem::perm_options::replace);
        return file.string();
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

/// What the client of an in-memory handshake is willing to speak. Defaults: any TLS 1.2 or 1.3.
struct ClientOptions {
    int min_version = TLS1_2_VERSION;
    int max_version = 0;                 // 0 = no limit
    std::string cipher_list = {};        // TLS 1.2 and below; empty = OpenSSL default
    std::vector<std::string> alpn = {};  // empty = no ALPN extension
    bool allow_weak_security = false;    // security level 0: lets a client speak TLS 1.0, CBC, ...
    std::string server_name = {};        // SNI host name to send; empty = no SNI extension
};

struct HandshakeOutcome {
    bool server_done = false;
    bool client_done = false;
    int version = 0;           // negotiated, e.g. TLS1_3_VERSION
    std::string cipher;        // negotiated suite name
    std::string alpn;          // protocol the server chose
    std::string peer_cn;       // common name of the certificate the client received
    int tickets_received = 0;  // session tickets the client was handed
    [[nodiscard]] bool ok() const noexcept {
        return server_done && client_done;
    }
};

/// Run a complete handshake between a server SSL made from `server_ctx` and a client SSL made from
/// `options`, joined by a pair of memory BIOs (no sockets, fully deterministic). Never throws on a
/// failed handshake: that is the outcome the tests look at.
inline HandshakeOutcome handshake_in_memory(SSL_CTX* server_ctx,
                                            const ClientOptions& options = {}) {
    HandshakeOutcome outcome;
    int tickets = 0;

    const SslCtxPtr client_ctx{SSL_CTX_new(TLS_client_method())};
    if (!client_ctx) {
        throw tls_fixture_error("client context failed");
    }
    if (options.allow_weak_security) {
        SSL_CTX_set_security_level(client_ctx.get(), 0);
    }
    SSL_CTX_set_min_proto_version(client_ctx.get(), options.min_version);
    SSL_CTX_set_max_proto_version(client_ctx.get(), options.max_version);
    if (!options.cipher_list.empty()) {
        std::string list = options.cipher_list;
        if (options.allow_weak_security) {
            list += ":@SECLEVEL=0";
        }
        if (SSL_CTX_set_cipher_list(client_ctx.get(), list.c_str()) != 1) {
            throw tls_fixture_error("client cipher list failed");
        }
    }
    SSL_CTX_set_verify(client_ctx.get(), SSL_VERIFY_NONE, nullptr);  // the server is what is tested
    SSL_CTX_set_session_cache_mode(client_ctx.get(), SSL_SESS_CACHE_CLIENT);
    SSL_CTX_set_app_data(client_ctx.get(), &tickets);
    SSL_CTX_sess_set_new_cb(client_ctx.get(), [](SSL* ssl, SSL_SESSION* /*session*/) -> int {
        int* counter = static_cast<int*>(SSL_CTX_get_app_data(SSL_get_SSL_CTX(ssl)));
        ++*counter;
        return 0;  // we keep no reference
    });

    const SslPtr server{SSL_new(server_ctx)};
    const SslPtr client{SSL_new(client_ctx.get())};
    if (!server || !client) {
        throw tls_fixture_error("SSL_new failed");
    }
    if (!options.server_name.empty()) {
        // SSL_set_tlsext_host_name is a macro with a C-style cast; call SSL_ctrl directly.
        SSL_ctrl(client.get(),
                 SSL_CTRL_SET_TLSEXT_HOSTNAME,
                 TLSEXT_NAMETYPE_host_name,
                 const_cast<char*>(options.server_name.c_str()));
    }
    if (!options.alpn.empty()) {
        std::vector<unsigned char> wire;
        for (const std::string& protocol : options.alpn) {
            wire.push_back(static_cast<unsigned char>(protocol.size()));
            wire.insert(wire.end(), protocol.begin(), protocol.end());
        }
        SSL_set_alpn_protos(client.get(), wire.data(), static_cast<unsigned>(wire.size()));
    }
    BIO* server_end = nullptr;
    BIO* client_end = nullptr;
    if (BIO_new_bio_pair(&server_end, 0, &client_end, 0) != 1) {
        throw tls_fixture_error("BIO pair failed");
    }
    SSL_set_bio(server.get(), server_end, server_end);  // the SSL now owns the BIOs
    SSL_set_bio(client.get(), client_end, client_end);
    SSL_set_accept_state(server.get());
    SSL_set_connect_state(client.get());

    // Alternate the two sides until both are done or one fails for a reason other than "wait".
    const auto stuck = [](SSL* ssl, int result) {
        const int error = SSL_get_error(ssl, result);
        return error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE;
    };
    for (int round = 0; round < 64 && !(outcome.server_done && outcome.client_done); ++round) {
        if (!outcome.client_done) {
            const int r = SSL_do_handshake(client.get());
            outcome.client_done = r == 1;
            if (r != 1 && stuck(client.get(), r)) {
                break;
            }
        }
        if (!outcome.server_done) {
            const int r = SSL_do_handshake(server.get());
            outcome.server_done = r == 1;
            if (r != 1 && stuck(server.get(), r)) {
                break;
            }
        }
    }
    if (outcome.ok()) {
        outcome.version = SSL_version(client.get());
        outcome.cipher = SSL_CIPHER_get_name(SSL_get_current_cipher(client.get()));
        const unsigned char* chosen = nullptr;
        unsigned chosen_length = 0;
        SSL_get0_alpn_selected(client.get(), &chosen, &chosen_length);
        if (chosen != nullptr) {
            outcome.alpn.assign(reinterpret_cast<const char*>(chosen), chosen_length);
        }
        const X509Ptr peer{SSL_get1_peer_certificate(client.get())};
        if (peer) {
            std::array<char, 128> cn{};
            X509_NAME_get_text_by_NID(X509_get_subject_name(peer.get()),
                                      NID_commonName,
                                      cn.data(),
                                      static_cast<int>(cn.size()));
            outcome.peer_cn = cn.data();
        }
        // TLS 1.3 tickets travel after the handshake: let the server write, the client read.
        std::array<char, 8> data{};
        const char hello = 'x';
        if (SSL_write(server.get(), &hello, 1) == 1) {
            static_cast<void>(SSL_read(client.get(), data.data(), static_cast<int>(data.size())));
        }
        outcome.tickets_received = tickets;
    }
    ERR_clear_error();
    return outcome;
}

}  // namespace wsrv::test

// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/error.hpp"
#include "base/logger.hpp"

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace wsrv {

/// The OpenSSL side of a loaded certificate (defined in tls/tls_material.hpp, which only the tls
/// module and tests include). Keeping it opaque here keeps this header free of OpenSSL (ADR-0019).
struct TlsMaterial;

/// The most SNI entries one configuration may carry. Each entry holds a full OpenSSL context, so
/// the number is bounded (ADR-0019 section 7); the default certificate is not counted.
inline constexpr std::size_t kMaxSniEntries = 64;

/// A certificate for one host name: an exact name ("www.example.com") or one leading wildcard
/// label ("*.example.com", see tls/sni.hpp). Same files and same rules as the default certificate.
struct TlsSniEntry {
    std::string name;
    std::string certificate_chain_file;
    std::string private_key_file;
};

/// Where the server certificate and key live. PEM files; the key must not be passphrase-protected
/// (an operator who needs that terminates TLS in front of wsrv).
struct TlsConfig {
    std::string certificate_chain_file;  ///< the server certificate, then any intermediates
    std::string private_key_file;        ///< not readable by "others" (SECURITY_DESIGN rule 7)
    /// Optional per-name certificates, chosen by the SNI in the ClientHello. A client that sends no
    /// name, an unknown name or an unusable one gets the certificate above. At most kMaxSniEntries.
    std::vector<TlsSniEntry> sni;
};

/// The server-side TLS settings: one hardened profile (ADR-0019 section 5) plus one certificate,
/// replaceable at run time (section 6). `load` refuses anything it would not be willing to serve:
/// a missing or unreadable file, a key that does not match, an expired or not-yet-valid
/// certificate, a weak key, a key file open to everyone.
///
/// Thread-affinity: any-thread. `current()` and `reload()` may race; each connection takes a
/// snapshot once, at accept, and keeps it for its whole life, so a reload never changes a
/// handshake that is already running.
class TlsServerContext final {
    struct Key {
        explicit Key() = default;
    };

public:
    TlsServerContext(Key /*key*/,
                     TlsConfig config,
                     ILogger& logger,
                     std::shared_ptr<const TlsMaterial> material) noexcept;
    TlsServerContext(const TlsServerContext&) = delete;
    TlsServerContext& operator=(const TlsServerContext&) = delete;
    TlsServerContext(TlsServerContext&&) = delete;
    TlsServerContext& operator=(TlsServerContext&&) = delete;
    ~TlsServerContext();

    /// Read and validate the files. The reason of a refusal is logged (path and OpenSSL's reason,
    /// never key material); the Error carries a fixed text. `logger` must outlive the result.
    /// @return Error{InvalidConfig} for anything wrong with the files, Error{ResourceExhausted}.
    [[nodiscard]] static Result<std::unique_ptr<TlsServerContext>> load(const TlsConfig& config,
                                                                        ILogger& logger);

    /// Read the files again. All or nothing: on any failure the certificate in use is unchanged.
    [[nodiscard]] Result<void> reload();

    /// The certificate in use now. Never null.
    [[nodiscard]] std::shared_ptr<const TlsMaterial> current() const;

private:
    TlsConfig config_;
    ILogger* logger_;
    mutable std::mutex mutex_;  // guards current_ for the length of a pointer copy
    std::shared_ptr<const TlsMaterial> current_;
};

}  // namespace wsrv

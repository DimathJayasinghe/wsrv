// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <openssl/ssl.h>

#include <memory>
#include <string>
#include <vector>

namespace wsrv {

struct SslCtxDeleter {
    void operator()(SSL_CTX* ctx) const noexcept {
        SSL_CTX_free(ctx);
    }
};
using UniqueSslCtx = std::unique_ptr<SSL_CTX, SslCtxDeleter>;

struct SslDeleter {
    void operator()(SSL* ssl) const noexcept {
        SSL_free(ssl);
    }
};
using UniqueSsl = std::unique_ptr<SSL, SslDeleter>;

/// One SNI entry: a normalised pattern (tls/sni.hpp) and its own context, built with the same
/// hardened profile as the default.
struct TlsSniContext {
    std::string pattern;
    UniqueSslCtx ctx;
};

/// One loaded configuration: the default certificate with the hardened settings applied, plus the
/// SNI entries. Immutable once published; SSL objects made from `ctx` hold their own reference
/// (and, after SNI chose an entry, a reference to that entry's context), so the material may be
/// replaced (reload) while connections created from it are still running.
///
/// `ctx` holds a raw pointer to this object for the server-name callback, so a TlsMaterial must not
/// move after it is built and must outlive every handshake that uses `ctx`; TlsTransport keeps its
/// shared_ptr for exactly that long.
struct TlsMaterial {
    UniqueSslCtx ctx;
    std::vector<TlsSniContext> entries;
};

}  // namespace wsrv

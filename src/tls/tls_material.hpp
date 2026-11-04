// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <openssl/ssl.h>

#include <memory>

namespace wsrv {

struct SslCtxDeleter {
    void operator()(SSL_CTX* ctx) const noexcept {
        SSL_CTX_free(ctx);
    }
};
using UniqueSslCtx = std::unique_ptr<SSL_CTX, SslCtxDeleter>;

/// One loaded certificate with the hardened settings applied. Immutable once published; SSL
/// objects made from `ctx` hold their own reference, so the material may be replaced (reload)
/// while connections created from it are still running.
struct TlsMaterial {
    UniqueSslCtx ctx;
};

}  // namespace wsrv

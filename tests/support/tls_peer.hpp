// SPDX-License-Identifier: Apache-2.0
#pragma once

// A TLS client for tests: an OpenSSL SSL over two memory BIOs, so the test decides exactly which
// bytes travel when (one at a time, in bursts, cut short). It never touches a socket.

#include "support/tls_fixtures.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace wsrv::test {

class TlsTestClient {
public:
    /// `max_version` 0 = no limit (TLS 1.3); TLS1_2_VERSION forces 1.2 (renegotiation tests).
    explicit TlsTestClient(int max_version = 0) {
        ctx_.reset(SSL_CTX_new(TLS_client_method()));
        SSL_CTX_set_min_proto_version(ctx_.get(), TLS1_2_VERSION);
        SSL_CTX_set_max_proto_version(ctx_.get(), max_version);
        SSL_CTX_set_verify(ctx_.get(), SSL_VERIFY_NONE, nullptr);  // the server is what is tested
        ssl_.reset(SSL_new(ctx_.get()));
        BIO* in = BIO_new(BIO_s_mem());
        BIO* out = BIO_new(BIO_s_mem());
        BIO_set_mem_eof_return(in, -1);    // empty = "try again", not end of stream
        SSL_set_bio(ssl_.get(), in, out);  // the SSL owns both from here
        in_ = in;
        out_ = out;
        SSL_set_connect_state(ssl_.get());
    }

    /// One handshake step. @return true once the client side is complete.
    bool handshake() {
        if (!done_) {
            ERR_clear_error();
            done_ = SSL_do_handshake(ssl_.get()) == 1;
        }
        return done_;
    }

    /// Everything the client wants to send now (and forget it).
    std::vector<std::byte> take_output() {
        std::vector<std::byte> bytes;
        std::array<std::byte, 4096> chunk{};
        int n = 0;
        while ((n = BIO_read(out_, chunk.data(), static_cast<int>(chunk.size()))) > 0) {
            bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + n);
        }
        return bytes;
    }

    void feed(const std::vector<std::byte>& bytes) {
        if (!bytes.empty()) {
            BIO_write(in_, bytes.data(), static_cast<int>(bytes.size()));
        }
    }

    /// Encrypt `text` (queued in the output BIO until take_output()). @return bytes accepted.
    int write(const std::string& text) {
        return SSL_write(ssl_.get(), text.data(), static_cast<int>(text.size()));
    }

    /// Everything readable now. Sets closed() on close_notify and failed() on a TLS error.
    std::string read_all() {
        std::string text;
        std::array<char, 4096> chunk{};
        for (;;) {
            ERR_clear_error();
            const int n = SSL_read(ssl_.get(), chunk.data(), static_cast<int>(chunk.size()));
            if (n > 0) {
                text.append(chunk.data(), static_cast<std::size_t>(n));
                continue;
            }
            const int error = SSL_get_error(ssl_.get(), n);
            closed_ = closed_ || error == SSL_ERROR_ZERO_RETURN;
            failed_ = failed_ || error == SSL_ERROR_SSL || error == SSL_ERROR_SYSCALL;
            return text;
        }
    }

    /// TLS 1.3: ask the peer to change its keys too (the next write carries the request).
    bool key_update_request() {
        ERR_clear_error();
        return SSL_key_update(ssl_.get(), SSL_KEY_UPDATE_REQUESTED) == 1;
    }

    /// Send close_notify.
    void shutdown() {
        ERR_clear_error();
        SSL_shutdown(ssl_.get());
    }

    /// TLS 1.2 only: ask for a renegotiation. @return true if the request was started.
    bool renegotiate() {
        ERR_clear_error();
        return SSL_renegotiate(ssl_.get()) == 1 && SSL_do_handshake(ssl_.get()) <= 1;
    }

    [[nodiscard]] bool done() const noexcept {
        return done_;
    }
    [[nodiscard]] bool closed() const noexcept {
        return closed_;
    }
    [[nodiscard]] bool failed() const noexcept {
        return failed_;
    }
    [[nodiscard]] int version() const noexcept {
        return SSL_version(ssl_.get());
    }

private:
    SslCtxPtr ctx_;
    SslPtr ssl_;
    BIO* in_ = nullptr;   // owned by ssl_
    BIO* out_ = nullptr;  // owned by ssl_
    bool done_ = false;
    bool closed_ = false;
    bool failed_ = false;
};

}  // namespace wsrv::test

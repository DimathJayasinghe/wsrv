// SPDX-License-Identifier: Apache-2.0
#pragma once

// A blocking TLS test client over a real loopback socket: the TcpClient API (upgrade, frames, EOF)
// with an OpenSSL session in between. Not part of the product.

#include "support/tcp_client.hpp"
#include "support/tls_fixtures.hpp"

#include <array>
#include <string>

#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>

namespace wsrv::test {

struct TlsClientOptions {
    int max_version = 0;  // 0 = no limit; TLS1_2_VERSION forces 1.2
    std::string server_name = "localhost";
    std::vector<std::string> alpn = {};
};

class TlsClient final : public TcpClient {
public:
    explicit TlsClient(std::uint16_t port, const TlsClientOptions& options = {}) : TcpClient{port} {
        if (!valid()) {
            return;
        }
        const timeval limit{5, 0};  // a hung peer fails the test instead of hanging it
        ::setsockopt(raw_fd(), SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
        ::setsockopt(raw_fd(), SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
        ctx_.reset(SSL_CTX_new(TLS_client_method()));
        SSL_CTX_set_min_proto_version(ctx_.get(), TLS1_2_VERSION);
        SSL_CTX_set_max_proto_version(ctx_.get(), options.max_version);
        SSL_CTX_set_verify(ctx_.get(), SSL_VERIFY_NONE, nullptr);  // self-signed test certificates
        ssl_.reset(SSL_new(ctx_.get()));
        SSL_set_fd(ssl_.get(), raw_fd());
        // The SSL_set_tlsext_host_name macro hides an old-style cast; call SSL_ctrl directly.
        SSL_ctrl(ssl_.get(),
                 SSL_CTRL_SET_TLSEXT_HOSTNAME,
                 TLSEXT_NAMETYPE_host_name,
                 const_cast<char*>(options.server_name.c_str()));
        if (!options.alpn.empty()) {
            std::vector<unsigned char> wire;
            for (const std::string& name : options.alpn) {
                wire.push_back(static_cast<unsigned char>(name.size()));
                wire.insert(wire.end(), name.begin(), name.end());
            }
            SSL_set_alpn_protos(ssl_.get(), wire.data(), static_cast<unsigned>(wire.size()));
        }
        ERR_clear_error();
        connected_ = SSL_connect(ssl_.get()) == 1;
        ERR_clear_error();
    }

    /// True if the TLS handshake completed.
    [[nodiscard]] bool tls_connected() const {
        return connected_;
    }
    [[nodiscard]] int version() const {
        return ssl_ ? SSL_version(ssl_.get()) : 0;
    }
    [[nodiscard]] std::string alpn() const {
        const unsigned char* chosen = nullptr;
        unsigned length = 0;
        if (ssl_) {
            SSL_get0_alpn_selected(ssl_.get(), &chosen, &length);
        }
        return chosen == nullptr ? std::string{}
                                 : std::string{reinterpret_cast<const char*>(chosen), length};
    }
    /// Common name of the certificate the server presented.
    [[nodiscard]] std::string peer_common_name() const {
        const X509Ptr peer{ssl_ ? SSL_get1_peer_certificate(ssl_.get()) : nullptr};
        if (!peer) {
            return {};
        }
        std::array<char, 128> name{};
        X509_NAME_get_text_by_NID(X509_get_subject_name(peer.get()),
                                  NID_commonName,
                                  name.data(),
                                  static_cast<int>(name.size()));
        return name.data();
    }

    /// Send close_notify, as a well-behaved client does before it closes.
    void shutdown_tls() {
        if (ssl_) {
            SSL_shutdown(ssl_.get());
        }
    }

    using TcpClient::send;  // keep the string_view overload the override would hide
    bool send(ByteView bytes) const override {
        if (!connected_) {
            return false;
        }
        std::size_t done = 0;
        while (done < bytes.size()) {
            const int n =
                SSL_write(ssl_.get(), bytes.data() + done, static_cast<int>(bytes.size() - done));
            if (n <= 0) {
                return false;
            }
            done += static_cast<std::size_t>(n);
        }
        return true;
    }

    [[nodiscard]] bool silent_for(std::chrono::milliseconds period) const override {
        return !ready(period);
    }

    bool wait_for_eof() override {
        if (!connected_) {
            return TcpClient::wait_for_eof();  // the handshake failed: the TCP connection is the
                                               // news
        }
        std::array<char, 4096> chunk{};
        for (;;) {
            if (!ready(kTimeout)) {
                return false;
            }
            ERR_clear_error();
            const int n = SSL_read(ssl_.get(), chunk.data(), static_cast<int>(chunk.size()));
            if (n > 0) {
                continue;
            }
            const int error = SSL_get_error(ssl_.get(), n);
            if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
                return true;  // close_notify, a cut or an error: the stream has ended
            }
        }
    }

protected:
    bool fill() override {
        if (!connected_ || !ready(kTimeout)) {
            return false;
        }
        std::array<char, 4096> chunk{};
        ERR_clear_error();
        const int n = SSL_read(ssl_.get(), chunk.data(), static_cast<int>(chunk.size()));
        if (n <= 0) {
            return false;
        }
        append(chunk.data(), static_cast<std::size_t>(n));
        return true;
    }

private:
    /// Decrypted data already waiting inside OpenSSL counts as readable; the socket may be empty.
    [[nodiscard]] bool ready(std::chrono::milliseconds period) const {
        return (ssl_ && SSL_pending(ssl_.get()) > 0) || readable(period);
    }

    SslCtxPtr ctx_;
    SslPtr ssl_;
    bool connected_ = false;
};

}  // namespace wsrv::test

// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/error.hpp"
#include "net/transport.hpp"

#include <cstddef>
#include <memory>

#include "tls/server_context.hpp"

namespace wsrv {

/// TLS over another ITransport (a decorator, ADR-0019 section 2): the connection above it sees
/// plaintext, the inner transport (the TCP socket) carries ciphertext. FR-T2: nothing above this
/// class knows TLS exists.
///
/// OpenSSL is driven through two memory buffers instead of the socket: ciphertext from the inner
/// transport is put into one, ciphertext OpenSSL wants to send is taken out of the other. So the
/// transport never blocks, never touches a descriptor itself, and a test can feed it any byte
/// pattern. Both buffers are bounded (see kMaxPendingOutput in the source).
///
/// The handshake is not a separate step: read() and write() run it first and return
/// Error{WouldBlock} until it completes. Besides the ITransport contract:
///   read():  0 = the peer sent close_notify (clean end); Error{ConnectionClosed} = the stream was
///            cut without it; Error{TlsFailure} = not TLS, a failed handshake or a bad record. A
///            failed transport stays failed.
///   write(): accepts at most one TLS record per call (16 KiB) and returns Error{WouldBlock} while
///            too much ciphertext is waiting for the inner transport. write({}) only pushes out
///            waiting ciphertext and returns 0.
/// Thread-affinity: loop-thread-only.
class TlsTransport final : public ITransport {
    class State;
    struct Key {
        explicit Key() = default;
    };

public:
    TlsTransport(Key /*key*/, std::unique_ptr<State> state) noexcept;
    TlsTransport(const TlsTransport&) = delete;
    TlsTransport& operator=(const TlsTransport&) = delete;
    TlsTransport(TlsTransport&&) = delete;
    TlsTransport& operator=(TlsTransport&&) = delete;
    ~TlsTransport() override;

    /// A server-side TLS stream over `inner`, using the certificate snapshot `material` (take it
    /// with TlsServerContext::current() once per connection).
    /// @return Error{InvalidArgument} for a null argument, Error{ResourceExhausted}.
    [[nodiscard]] static Result<std::unique_ptr<TlsTransport>>
    create(std::unique_ptr<ITransport> inner, std::shared_ptr<const TlsMaterial> material);

    [[nodiscard]] Result<std::size_t> read(MutableByteView out) override;
    [[nodiscard]] Result<std::size_t> write(ByteView data) override;
    void shutdown_write() noexcept override;
    void close() noexcept override;
    [[nodiscard]] int fd() const noexcept override;
    [[nodiscard]] bool wants_write() const noexcept override;
    [[nodiscard]] bool buffered_input() const noexcept override;

private:
    std::unique_ptr<State> state_;
};

}  // namespace wsrv

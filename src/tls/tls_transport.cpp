// SPDX-License-Identifier: Apache-2.0
#include "tls/tls_transport.hpp"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <new>
#include <optional>
#include <string_view>
#include <utility>

#include "tls/tls_material.hpp"

namespace wsrv {

namespace {

// One TLS record carries at most 16 KiB of plaintext (RFC 8446 section 5.1, RFC 5246
// section 6.2.1).
constexpr std::size_t kRecordPlaintext = 16384;
// Ciphertext OpenSSL has produced that the inner transport has not taken. write() stops accepting
// when this is exceeded, so a peer that does not read cannot make us buffer without bound (T3).
constexpr std::size_t kMaxPendingOutput = std::size_t{64} * 1024;
// The same bound for the read path, where only the peer can make us produce output (answers to its
// KeyUpdate requests, alerts, handshake flights). Higher than the write bound because a busy
// writer legitimately sits near that one; a peer that gets past it is not reading at all, and the
// stream is cut instead of queueing without limit (T3).
constexpr std::size_t kMaxPendingOutputOnRead = 2 * kMaxPendingOutput;
// A single call to read() takes at most this many ciphertext chunks from the descriptor. Beyond
// it the call yields with buffered_input() true, so one flooding peer cannot hold the loop.
constexpr int kMaxFillsPerCall = 64;
constexpr std::size_t kScratchSize = 16384;

constexpr std::string_view kClosed = "tls: use of a closed transport";
constexpr std::string_view kHandshakeFailed = "tls: the handshake failed";
constexpr std::string_view kBadRecord = "tls: a record could not be processed";
constexpr std::string_view kNoProgress = "tls: no progress";
constexpr std::string_view kPeerNotReading = "tls: the peer does not read our output";
constexpr std::string_view kCutShort = "tls: the connection ended without close_notify";

/// What OpenSSL asked for after an operation that did not complete.
enum class Need : std::uint8_t { Input, Output, PeerClosed, Failure };

}  // namespace

/// Everything one connection owns. Defined here so that no OpenSSL type is visible in the header.
class TlsTransport::State {
public:
    State(std::unique_ptr<ITransport> inner,
          std::shared_ptr<const TlsMaterial> material,
          UniqueSsl ssl,
          BIO* from_peer,
          BIO* to_peer) noexcept
        : inner_{std::move(inner)}, material_{std::move(material)}, ssl_{std::move(ssl)},
          from_peer_{from_peer}, to_peer_{to_peer} {}

    Result<std::size_t> read(MutableByteView out);
    Result<std::size_t> write(ByteView data);
    void shutdown_write() noexcept;
    void close() noexcept;

    [[nodiscard]] int fd() const noexcept {
        return closed_ ? -1 : inner_->fd();
    }
    [[nodiscard]] bool wants_write() const noexcept {
        return !closed_ && output_blocked_;
    }
    [[nodiscard]] bool buffered_input() const noexcept {
        return !closed_ && !sticky_ &&
               (yielded_ || SSL_pending(ssl_.get()) > 0 || BIO_ctrl_pending(from_peer_) > 0);
    }

private:
    enum class Outcome : std::uint8_t { Done, PeerClosed };

    [[nodiscard]] std::optional<Error> usable() const;
    [[nodiscard]] Need classify(int result) const;
    Result<void> flush();
    Result<void> fill();
    Result<void> fail_stream(std::string_view context);
    Result<void> note_flush_error(const Result<void>& flushed);

    Result<void> bound_output();
    Result<void> serve_input(int& fills);
    template <typename Op>
    Result<Outcome> drive(Op&& op);
    Result<void> ensure_established();

    std::unique_ptr<ITransport> inner_;
    std::shared_ptr<const TlsMaterial>
        material_;  // keeps the certificate alive; SSL holds its own too
    UniqueSsl ssl_;
    BIO* from_peer_;  // ciphertext the inner transport delivered, waiting for OpenSSL (owned by
                      // ssl_)
    BIO* to_peer_;  // ciphertext OpenSSL produced, waiting for the inner transport (owned by ssl_)
    std::array<std::byte, kScratchSize> scratch_{};
    bool established_ = false;
    bool closed_ = false;
    bool peer_closed_ = false;     // close_notify received: a clean end of stream
    bool output_blocked_ = false;  // flush() hit WouldBlock with ciphertext still waiting
    bool yielded_ = false;         // read() stopped early with input possibly left
    bool sent_close_notify_ = false;
    std::optional<Error> sticky_;  // the stream failed; every later call reports this
};

Result<void> TlsTransport::State::note_flush_error(const Result<void>& flushed) {
    // A full socket is not an error here: the ciphertext stays queued and wants_write() says so.
    if (!flushed && flushed.error().code != ErrorCode::WouldBlock) {
        return std::unexpected{flushed.error()};
    }
    return {};
}

std::optional<Error> TlsTransport::State::usable() const {
    if (closed_) {
        return Error{ErrorCode::ConnectionClosed, kClosed};
    }
    return sticky_;
}

Need TlsTransport::State::classify(int result) const {
    switch (SSL_get_error(ssl_.get(), result)) {
    case SSL_ERROR_WANT_READ:
        return Need::Input;
    case SSL_ERROR_WANT_WRITE:
        return Need::Output;
    case SSL_ERROR_ZERO_RETURN:
        return Need::PeerClosed;
    default:
        return Need::Failure;
    }
}

/// Give the inner transport the ciphertext OpenSSL produced. Partial writes are normal: whatever
/// the inner transport did not take stays queued. @return Error{WouldBlock} if some remains.
Result<void> TlsTransport::State::flush() {
    while (true) {
        char* pending = nullptr;
        const long size = BIO_get_mem_data(to_peer_, &pending);
        if (size <= 0) {
            output_blocked_ = false;
            return {};
        }
        const Result<std::size_t> taken =
            inner_->write(as_bytes(std::string_view{pending, static_cast<std::size_t>(size)}));
        if (!taken || *taken == 0) {
            if (!taken && taken.error().code != ErrorCode::WouldBlock) {
                return std::unexpected{taken.error()};
            }
            output_blocked_ = true;
            return fail(ErrorCode::WouldBlock, "tls: the socket is full");
        }
        // Drop what was sent from the front of the buffer.
        for (std::size_t left = *taken; left > 0;) {
            const std::size_t part = std::min(left, scratch_.size());
            BIO_read(to_peer_, scratch_.data(), static_cast<int>(part));
            left -= part;
        }
    }
}

/// Take one chunk of ciphertext from the inner transport and give it to OpenSSL.
Result<void> TlsTransport::State::fill() {
    const Result<std::size_t> got = inner_->read(scratch_);
    if (!got) {
        return std::unexpected{got.error()};
    }
    if (*got == 0) {
        // The peer closed TCP. A clean TLS end is close_notify, which arrives as data, so this is
        // a cut (RFC 8446 section 6.1): an error here, not an end of stream.
        return fail(ErrorCode::ConnectionClosed, kCutShort);
    }
    if (BIO_write(from_peer_, scratch_.data(), static_cast<int>(*got)) != static_cast<int>(*got)) {
        return fail(ErrorCode::ResourceExhausted, "tls: out of memory");
    }
    return {};
}

/// The stream is dead: try to tell the peer (an alert may be queued), and remember why.
Result<void> TlsTransport::State::fail_stream(std::string_view context) {
    ERR_clear_error();  // OpenSSL's reason is not shown: an Error carries fixed text only
    static_cast<void>(flush().has_value());  // best effort
    sticky_ = Error{ErrorCode::TlsFailure, context};
    return std::unexpected{*sticky_};
}

/// Cut the stream if the peer made us queue more than the read-path bound and still takes nothing.
Result<void> TlsTransport::State::bound_output() {
    if (BIO_ctrl_pending(to_peer_) > kMaxPendingOutputOnRead) {
        return fail_stream(kPeerNotReading);
    }
    return {};
}

/// OpenSSL needs more ciphertext: first send what is waiting (it may be what the peer is waiting
/// for), then take one chunk from the descriptor. `fills` counts chunks in this call.
Result<void> TlsTransport::State::serve_input(int& fills) {
    if (const Result<void> pushed = note_flush_error(flush()); !pushed) {
        return pushed;
    }
    if (const Result<void> bounded = bound_output(); !bounded) {
        return bounded;
    }
    if (++fills > kMaxFillsPerCall) {
        yielded_ = true;  // data may remain in the kernel: tell the connection to come back
        return fail(ErrorCode::WouldBlock, "tls: yielding to other connections");
    }
    Result<void> pulled = fill();
    if (!pulled && pulled.error().code == ErrorCode::ConnectionClosed) {
        sticky_ = pulled.error();
    }
    return pulled;
}

/// Run `op` (an OpenSSL call returning > 0 on success) until it succeeds, feeding it input and
/// emptying its output as it asks. Returns when it succeeded, when the peer closed cleanly, or
/// with the error: WouldBlock when only more input from the descriptor can help.
template <typename Op>
Result<TlsTransport::State::Outcome> TlsTransport::State::drive(Op&& op) {
    int fills = 0;
    for (int round = 0; round < 4 * kMaxFillsPerCall; ++round) {
        ERR_clear_error();
        const int result = op();
        if (result > 0) {
            return Outcome::Done;
        }
        Result<void> served;
        switch (classify(result)) {
        case Need::Input:
            served = serve_input(fills);
            break;
        case Need::Output:
            served = flush();
            break;
        case Need::PeerClosed:
            return Outcome::PeerClosed;
        case Need::Failure:
            return std::unexpected{
                fail_stream(established_ ? kBadRecord : kHandshakeFailed).error()};
        }
        if (!served) {
            return std::unexpected{served.error()};
        }
    }
    return std::unexpected{fail_stream(kNoProgress).error()};
}

Result<void> TlsTransport::State::ensure_established() {
    if (established_) {
        return {};
    }
    const Result<Outcome> done = drive([&] { return SSL_do_handshake(ssl_.get()); });
    if (!done) {
        return std::unexpected{done.error()};
    }
    if (*done == Outcome::PeerClosed) {
        peer_closed_ = true;  // closed during the handshake: nothing more will come
        return fail(ErrorCode::ConnectionClosed, kCutShort);
    }
    established_ = true;
    static_cast<void>(note_flush_error(flush()).has_value());  // the server's last flight
    return {};
}

Result<std::size_t> TlsTransport::State::read(MutableByteView out) {
    if (const std::optional<Error> unusable = usable()) {
        return std::unexpected{*unusable};
    }
    if (out.empty()) {
        return std::size_t{0};  // never look like EOF
    }
    yielded_ = false;
    if (const Result<void> ready = ensure_established(); !ready) {
        return std::unexpected{ready.error()};
    }
    if (peer_closed_) {
        return std::size_t{0};
    }
    std::size_t got = 0;
    const Result<Outcome> done =
        drive([&] { return SSL_read_ex(ssl_.get(), out.data(), out.size(), &got); });
    if (!done) {
        return std::unexpected{done.error()};
    }
    // KeyUpdate and alerts may have queued output; a blocked socket is reported by wants_write().
    static_cast<void>(note_flush_error(flush()).has_value());
    if (const Result<void> bounded = bound_output(); !bounded) {
        return std::unexpected{bounded.error()};
    }
    if (*done == Outcome::PeerClosed) {
        peer_closed_ = true;
        return std::size_t{0};
    }
    return got;
}

Result<std::size_t> TlsTransport::State::write(ByteView data) {
    if (const std::optional<Error> unusable = usable()) {
        return std::unexpected{*unusable};
    }
    if (const Result<void> ready = ensure_established(); !ready) {
        return std::unexpected{ready.error()};
    }
    if (const Result<void> pushed = note_flush_error(flush()); !pushed) {
        return std::unexpected{pushed.error()};
    }
    if (data.empty()) {
        return std::size_t{0};  // only pushes out what is waiting
    }
    if (BIO_ctrl_pending(to_peer_) > kMaxPendingOutput) {
        output_blocked_ = true;
        return fail(ErrorCode::WouldBlock, "tls: too much output is waiting");
    }
    std::size_t written = 0;
    const std::size_t chunk = std::min(data.size(), kRecordPlaintext);
    ERR_clear_error();
    if (SSL_write_ex(ssl_.get(), data.data(), chunk, &written) != 1) {
        return std::unexpected{fail_stream(kBadRecord).error()};
    }
    if (const Result<void> pushed = note_flush_error(flush()); !pushed) {
        return std::unexpected{pushed.error()};
    }
    return written;
}

void TlsTransport::State::shutdown_write() noexcept {
    if (closed_) {
        return;
    }
    if (established_ && !sticky_ && !sent_close_notify_) {
        sent_close_notify_ = true;
        ERR_clear_error();
        SSL_shutdown(ssl_.get());  // queues close_notify; a TCP FIN follows it
        ERR_clear_error();
        static_cast<void>(flush().has_value());  // best effort
    }
    inner_->shutdown_write();
}

void TlsTransport::State::close() noexcept {
    if (closed_) {
        return;
    }
    closed_ = true;
    output_blocked_ = false;
    ssl_.reset();  // frees the two buffers with it
    from_peer_ = nullptr;
    to_peer_ = nullptr;
    inner_->close();
}

// ---- TlsTransport -------------------------------------------------------------------------------

TlsTransport::TlsTransport(Key /*key*/, std::unique_ptr<State> state) noexcept
    : state_{std::move(state)} {}

TlsTransport::~TlsTransport() = default;

Result<std::unique_ptr<TlsTransport>>
TlsTransport::create(std::unique_ptr<ITransport> inner,
                     std::shared_ptr<const TlsMaterial> material) {
    if (!inner || !material || !material->ctx) {
        return fail(ErrorCode::InvalidArgument, "tls: a transport and a certificate are required");
    }
    UniqueSsl ssl{SSL_new(material->ctx.get())};
    BIO* from_peer = BIO_new(BIO_s_mem());
    BIO* to_peer = BIO_new(BIO_s_mem());
    if (!ssl || from_peer == nullptr || to_peer == nullptr) {
        BIO_free(from_peer);
        BIO_free(to_peer);
        ERR_clear_error();
        return fail(ErrorCode::ResourceExhausted, "tls: cannot create a TLS stream");
    }
    BIO_set_mem_eof_return(from_peer, -1);       // empty means "no input yet", not "end of stream"
    SSL_set_bio(ssl.get(), from_peer, to_peer);  // the SSL owns both from here
    SSL_set_accept_state(ssl.get());
    try {
        auto state = std::make_unique<State>(
            std::move(inner), std::move(material), std::move(ssl), from_peer, to_peer);
        return std::make_unique<TlsTransport>(Key{}, std::move(state));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "tls: out of memory");
    }
}

Result<std::size_t> TlsTransport::read(MutableByteView out) {
    return state_->read(out);
}

Result<std::size_t> TlsTransport::write(ByteView data) {
    return state_->write(data);
}

void TlsTransport::shutdown_write() noexcept {
    state_->shutdown_write();
}

void TlsTransport::close() noexcept {
    state_->close();
}

int TlsTransport::fd() const noexcept {
    return state_->fd();
}

bool TlsTransport::wants_write() const noexcept {
    return state_->wants_write();
}

bool TlsTransport::buffered_input() const noexcept {
    return state_->buffered_input();
}

}  // namespace wsrv

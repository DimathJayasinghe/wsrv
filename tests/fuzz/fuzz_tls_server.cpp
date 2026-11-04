// SPDX-License-Identifier: Apache-2.0
// libFuzzer target for the server side of TLS (ADR-0019 section 9). The bytes are what a hostile
// client sends to a TLS port, fed to a real TlsTransport (a real OpenSSL server) in slices whose
// size the input chooses. Properties, on any input:
//   1. no crash, no leak, no out-of-bounds access (ASan/UBSan);
//   2. nothing arrives as application data: a stream of arbitrary bytes cannot complete a
//   handshake,
//      because that needs a valid Finished message (MAC), so read() must never return bytes;
//   3. the server's reply stays small: a bounded answer (an alert or one handshake flight), never
//      amplification;
//   4. a failed stream stays failed (the same code on the next read).
#include "base/assert.hpp"
#include "base/bytes.hpp"
#include "net/transport.hpp"
#include "support/tls_fixtures.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>

#include "tls/server_context.hpp"
#include "tls/tls_transport.hpp"

namespace {

/// Bytes in, bytes out, no kernel. The out-vector is shared so the harness can count the reply.
class PipeTransport final : public wsrv::ITransport {
public:
    explicit PipeTransport(std::size_t* written_total) noexcept : written_total_{written_total} {}
    void feed(wsrv::ByteView bytes) {
        incoming_.insert(incoming_.end(), bytes.begin(), bytes.end());
    }
    void feed_eof() noexcept {
        eof_ = true;
    }
    [[nodiscard]] wsrv::Result<std::size_t> read(wsrv::MutableByteView out) override {
        if (incoming_.empty()) {
            return eof_ ? wsrv::Result<std::size_t>{0}
                        : wsrv::fail(wsrv::ErrorCode::WouldBlock, "pipe empty");
        }
        const std::size_t n = std::min(out.size(), incoming_.size());
        std::copy_n(incoming_.begin(), n, out.begin());
        incoming_.erase(incoming_.begin(), incoming_.begin() + static_cast<std::ptrdiff_t>(n));
        return n;
    }
    [[nodiscard]] wsrv::Result<std::size_t> write(wsrv::ByteView data) override {
        *written_total_ += data.size();
        return data.size();
    }
    void shutdown_write() noexcept override {}
    void close() noexcept override {}
    [[nodiscard]] int fd() const noexcept override {
        return 3;
    }

private:
    std::deque<std::byte> incoming_;
    bool eof_ = false;
    std::size_t* written_total_;
};

/// One certificate for the whole run (generating one per input would dominate the time).
struct Server {
    wsrv::test::TempDir dir;
    wsrv::NullLogger logger;
    std::unique_ptr<wsrv::TlsServerContext> context;

    Server() {
        const wsrv::test::PemPair pem = wsrv::test::make_pem({});
        namespace fs = std::filesystem;
        wsrv::TlsConfig config;
        config.certificate_chain_file =
            dir.write("chain.pem", pem.certificate, fs::perms::owner_read | fs::perms::owner_write);
        config.private_key_file =
            dir.write("key.pem", pem.key, fs::perms::owner_read | fs::perms::owner_write);
        auto loaded = wsrv::TlsServerContext::load(config, logger);
        WSRV_ASSERT(loaded.has_value());
        context = std::move(*loaded);
    }
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    static const Server server;  // NOLINT(cert-err58-cpp): a harness; built once for speed
    if (size == 0) {
        return 0;
    }
    const std::size_t slice = std::size_t{data[0]} + 1;  // the first byte picks the slice size
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): bytes as std::byte, same size.
    const wsrv::ByteView input{reinterpret_cast<const std::byte*>(data) + 1, size - 1};

    std::size_t reply = 0;
    auto pipe = std::make_unique<PipeTransport>(&reply);
    PipeTransport* feed = pipe.get();
    auto created = wsrv::TlsTransport::create(std::move(pipe), server.context->current());
    WSRV_ASSERT(created.has_value());
    const std::unique_ptr<wsrv::TlsTransport> tls = std::move(*created);

    std::array<std::byte, 4096> buffer{};
    std::optional<wsrv::ErrorCode> failed;
    for (std::size_t at = 0; at < input.size() && !failed; at += slice) {
        feed->feed(input.subspan(at, std::min(slice, input.size() - at)));
        const wsrv::Result<std::size_t> got = tls->read(buffer);
        if (got) {
            WSRV_ASSERT(*got == 0);  // property 2 (0 would be a close_notify, also impossible here)
        } else if (got.error().code != wsrv::ErrorCode::WouldBlock) {
            failed = got.error().code;
        }
    }
    if (failed) {
        const wsrv::Result<std::size_t> again = tls->read(buffer);
        WSRV_ASSERT(!again.has_value() && again.error().code == *failed);  // property 4
    }
    feed->feed_eof();
    static_cast<void>(tls->read(buffer).has_value());
    WSRV_ASSERT(reply <= std::size_t{256} * 1024);  // property 3
    tls->close();
    return 0;
}

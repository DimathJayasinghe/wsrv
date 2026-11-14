// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "net/transport_wrapper.hpp"
#include "server/config.hpp"
#include "server/server.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace wsrv {

/// An invalid configuration, or a startup problem that is the operator's to fix (port in use,
/// no permission to bind). The only exception the server module throws, and only from
/// ServerBuilder::build() (ADR-0006, CODING_STANDARDS section 4).
class ConfigError : public std::runtime_error {
public:
    /// @param code what kind of problem it is, for callers that must react to it (a host
    /// embedding the server maps AddressInUse differently from a bad setting).
    explicit ConfigError(const std::string& message, ErrorCode code = ErrorCode::InvalidConfig)
        : std::runtime_error{message}, code_{code} {}

    [[nodiscard]] ErrorCode code() const noexcept {
        return code_;
    }

private:
    ErrorCode code_;
};

/// permessage-deflate as the operator configures it (RFC 7692, ADR-0020). Off unless it is passed
/// to ServerBuilder::permessage_deflate().
struct DeflateOptions {
    /// What is offered to clients: no_context_takeover for the server's compressor and the
    /// largest windows the server agrees to (it answers with the smaller of this and the offer).
    DeflatePolicy policy;
    /// zlib compression level, 1..9: CPU against ratio.
    int level = 6;
    /// zlib memLevel, 1..9: per-connection memory against ratio (docs/OPERATIONS.md has the table).
    int memory_level = 6;
    /// Outbound messages shorter than this are sent uncompressed (never worth the CPU).
    std::size_t min_compress_size = 128;
    /// An inflated message may be at most this many times its compressed size (plus a small
    /// allowance): the decompression-bomb limit, SECURITY_DESIGN T6. 1..kMaxConfiguredInflateRatio.
    std::uint32_t max_inflate_ratio = 100;
};

/// The composition root (Builder): collects configuration and collaborators, validates once, and
/// produces a running-ready Server. Nothing else in the module decides which concrete classes are
/// used.
///
///   ServerBuilder builder;
///   builder.listen("0.0.0.0", 9001).handler_factory(factory);
///   builder.config().session.idle_timeout = std::chrono::seconds{120};
///   auto server = builder.build();   // throws ConfigError
///   server->install_signal_handlers();
///   server->run();
///
/// Injected collaborators are borrowed and must outlive the Server. With `config().threads > 1`
/// the handler factory, the logger and the clock are used from several loop threads at once and
/// must be thread-safe (ADR-0014).
/// Thread-affinity: build on the thread that will run the server.
class ServerBuilder {
public:
    /// An IPv4 or IPv6 address literal (no names: DNS would block the loop) and a port (0 = let
    /// the kernel choose; see Server::local_endpoint()).
    ServerBuilder& listen(std::string_view ip, std::uint16_t port);
    /// Serve /metrics, /healthz and /readyz on this address (ADR-0023): plain HTTP, no
    /// authentication, so loopback unless the network around it is trusted. Port 0 = any free port
    /// (see Server::admin_endpoint()). Off unless called.
    ServerBuilder& admin(std::string_view ip, std::uint16_t port);
    ServerBuilder& handler_factory(IHandlerFactory& factory) noexcept;
    ServerBuilder& logger(ILogger& logger) noexcept;
    ServerBuilder& clock(const IClock& clock) noexcept;
    ServerBuilder& syscalls(ISyscalls& sys) noexcept;
    /// Turn the asynchronous handshake decision on (ADR-0018): every valid upgrade waits for
    /// `decider` (see ServerConfig::handshake_decision_timeout). Without it, upgrades are accepted
    /// at once. Borrowed; must be thread-safe and outlive the Server.
    ServerBuilder& handshake_decider(IHandshakeDecider& decider) noexcept;

    /// Wrap every accepted connection (TLS: pass a TlsTransportWrapper from the composition root).
    /// Without it connections are plain TCP. Borrowed; must be thread-safe and outlive the Server.
    ServerBuilder& transport_wrapper(ITransportWrapper& wrapper) noexcept;

    /// Turn permessage-deflate on (ADR-0020): the handshake offers it and each connection that
    /// agrees compresses and inflates with zlib. Sets the handshake policy and the session codecs
    /// together, so the half-configured state cannot happen, and replaces whatever
    /// `config().handshake.permessage_deflate` and `config().session.deflate` held. The options
    /// are checked by build(), which throws ConfigError. Fail-closed: a build without zlib
    /// (WSRV_ENABLE_DEFLATE=OFF) refuses to start instead of silently serving uncompressed.
    ServerBuilder& permessage_deflate(const DeflateOptions& options);

    /// Everything else (limits, timeouts, Origin policy, subprotocols, delivery mode...).
    [[nodiscard]] ServerConfig& config() noexcept {
        return config_;
    }

    /// @throws ConfigError with the offending field or the startup failure in the message.
    [[nodiscard]] std::unique_ptr<Server> build() const;

private:
    ServerConfig config_;
    ServerDeps deps_;
    std::optional<DeflateOptions> deflate_;  ///< applied by build(), after config() was copied
    std::string ip_ = "127.0.0.1";           ///< loopback unless the operator opts into exposure
    std::uint16_t port_ = 0;
    std::optional<std::pair<std::string, std::uint16_t>> admin_;  ///< ip and port, when enabled
};

}  // namespace wsrv

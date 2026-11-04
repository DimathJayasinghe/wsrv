// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "net/transport_wrapper.hpp"
#include "server/config.hpp"
#include "server/server.hpp"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

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

    /// Everything else (limits, timeouts, Origin policy, subprotocols, delivery mode...).
    [[nodiscard]] ServerConfig& config() noexcept {
        return config_;
    }

    /// @throws ConfigError with the offending field or the startup failure in the message.
    [[nodiscard]] std::unique_ptr<Server> build() const;

private:
    ServerConfig config_;
    ServerDeps deps_;
    std::string ip_ = "127.0.0.1";  ///< loopback unless the operator opts into exposure
    std::uint16_t port_ = 0;
};

}  // namespace wsrv

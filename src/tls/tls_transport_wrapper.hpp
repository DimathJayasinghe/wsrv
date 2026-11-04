// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "net/transport_wrapper.hpp"
#include "tls/server_context.hpp"

#include <memory>

namespace wsrv {

/// Wraps every accepted connection in a server-side TlsTransport that uses the certificate in
/// force when the connection arrives (a later reload affects only later connections).
/// Thread-affinity: any-thread (TlsServerContext::current() is).
class TlsTransportWrapper final : public ITransportWrapper {
public:
    /// `context` is borrowed and must outlive the wrapper and every server using it.
    explicit TlsTransportWrapper(TlsServerContext& context) noexcept : context_{&context} {}

    [[nodiscard]] std::unique_ptr<ITransport> wrap(std::unique_ptr<ITransport> plain) override;

private:
    TlsServerContext* context_;
};

}  // namespace wsrv

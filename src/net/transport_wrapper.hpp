// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "net/transport.hpp"

#include <memory>

namespace wsrv {

/// Puts something around each accepted connection's transport (TLS is the one user, ADR-0019
/// section 4). The server knows only this interface, so it needs no TLS code and no OpenSSL; the
/// composition root decides which wrapper, if any, to inject.
///
/// Thread-affinity: any-thread. Every loop thread calls wrap() for the connections it accepts.
class ITransportWrapper {
public:
    ITransportWrapper() = default;
    ITransportWrapper(const ITransportWrapper&) = delete;
    ITransportWrapper& operator=(const ITransportWrapper&) = delete;
    ITransportWrapper(ITransportWrapper&&) = delete;
    ITransportWrapper& operator=(ITransportWrapper&&) = delete;
    virtual ~ITransportWrapper() = default;

    /// @return the transport the connection should use (normally one that owns `plain`), or null
    /// if it cannot be made (out of memory): the server then drops the connection. `plain` is
    /// closed by whoever owns it when this returns, so a failure leaks nothing.
    [[nodiscard]] virtual std::unique_ptr<ITransport> wrap(std::unique_ptr<ITransport> plain) = 0;
};

}  // namespace wsrv

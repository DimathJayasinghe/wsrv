// SPDX-License-Identifier: Apache-2.0
#include "tls/tls_transport_wrapper.hpp"

#include "tls/tls_transport.hpp"

#include <utility>

namespace wsrv {

std::unique_ptr<ITransport> TlsTransportWrapper::wrap(std::unique_ptr<ITransport> plain) {
    auto tls = TlsTransport::create(std::move(plain), context_->current());
    if (!tls) {
        return nullptr;  // `plain` was consumed by create(); its destructor closed the socket
    }
    return std::move(*tls);
}

}  // namespace wsrv

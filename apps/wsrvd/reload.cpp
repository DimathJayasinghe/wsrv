// SPDX-License-Identifier: Apache-2.0
#include "wsrvd/reload.hpp"

#include "base/error.hpp"

namespace wsrv::wsrvd {

ReloadOutcome reload_tls_on_hangup(EmbeddedServer& server) {
    const Result<void> done = server.reload_tls();
    if (done) {
        return {true, "wsrvd: SIGHUP: tls certificate reloaded"};
    }
    if (done.error().code == ErrorCode::InvalidArgument) {
        return {false, "wsrvd: SIGHUP: tls is not enabled, nothing to reload"};
    }
    std::string line = "wsrvd: SIGHUP: tls reload failed, keeping the old certificate: ";
    const std::string detail = server.tls_detail();
    line += detail.empty() ? std::string{done.error().context} : detail;
    return {false, std::move(line)};
}

}  // namespace wsrv::wsrvd

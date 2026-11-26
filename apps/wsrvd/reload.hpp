// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "capi/embedded_server.hpp"

#include <string>

namespace wsrv::wsrvd {

/// What happened on SIGHUP, for the daemon's log.
struct ReloadOutcome {
    bool reloaded = false;  ///< the new certificate is in use
    /// TLS is off, so there was nothing to reload. (A reload that worked or failed was already
    /// logged by the TLS context, with the file and the reason: ADR-0037.)
    bool skipped = false;
    std::string line;  ///< one line for people, without a trailing newline
};

/// SIGHUP means "re-read your configuration files". The only reloadable thing today is the TLS
/// certificate and key (ADR-0019 section 6). A failed reload keeps the old certificate and says
/// which file was the problem; a daemon without TLS says there was nothing to do. It runs on the
/// daemon's own loop thread (never a WebSocket I/O thread) and does a few small file reads.
[[nodiscard]] ReloadOutcome reload_tls_on_hangup(EmbeddedServer& server);

}  // namespace wsrv::wsrvd

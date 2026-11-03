// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/error.hpp"
#include "net/syscalls.hpp"

#include <string>

namespace wsrv::wsrvd {

/// Read the shared IPC token from a file (ADR-0017 section 4). A token passed as a command-line
/// argument would show in `ps`, so it lives in a file instead, and the file must keep it private:
///  - it is a regular file (symlinks are followed, as container secrets are symlinks);
///  - owned by the daemon's user (or root);
///  - no permission bits for group or others (mode 0600 or stricter);
///  - one to kMaxTokenSize bytes; ONE trailing newline is dropped (`echo secret > file`).
///
/// The check and the read are two steps (stat, then open), so a hostile directory could swap the
/// file in between; the directory holding the token must be the operator's own, as for any secret.
/// @return Error{InvalidConfig} for a missing, unsafe or empty file; Error{Io} if reading fails.
[[nodiscard]] Result<std::string> read_token_file(ISyscalls& sys, const std::string& path);

}  // namespace wsrv::wsrvd

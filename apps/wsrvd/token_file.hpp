// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/error.hpp"
#include "base/random.hpp"
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

/// Create `path` holding a new generated token (apps/wsrvd/token_format.hpp) and one newline, for
/// `wsrvd --generate-token=FILE` (ADR-0038 section 3).
///  - the file is created exclusively (O_EXCL, mode 0600, whatever the umask): an existing file is
///    never overwritten (a token is a deployment's secret) and a symlink is never followed;
///  - the random bytes come from `random` and nothing weaker: if it fails no file is made;
///  - a write that fails or stops short leaves no file behind (a file that looks like a token and
///    is not one is worse than none).
/// The token is never printed or returned: it exists in the file and nowhere else.
/// @return Error{InvalidConfig} if `path` exists or its directory does not; Error{RandomUnavailable}
/// if there is no random source; Error{Io} for any other failure.
[[nodiscard]] Result<void>
generate_token_file(ISyscalls& sys, IRandom& random, const std::string& path);

}  // namespace wsrv::wsrvd

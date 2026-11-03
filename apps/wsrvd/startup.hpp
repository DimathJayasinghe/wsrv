// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/error.hpp"
#include "net/syscalls.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "wsrvd/config.hpp"

namespace wsrv::wsrvd {

/// A config file is a few lines; anything bigger is a mistake (or an attack on startup memory).
inline constexpr std::size_t kMaxConfigFileSize = std::size_t{64} << 10U;

/// Everything the daemon needs before it opens a single socket, validated.
struct Startup {
    DaemonConfig config;
    std::string token;  ///< empty = no token configured
    bool help = false;  ///< --help was given: print usage() and exit, config is incomplete
};

/// Turn the command line into a Startup (ADR-0017). Arguments are applied in order, so a flag after
/// `--config=FILE` overrides the file:
///   --help                 print usage
///   --config=FILE          read key = value lines from FILE
///   --key=value            any config key; dashes mean underscores
/// The token is read from `ipc_token_file` (never from an argument), with the permission checks of
/// read_token_file. Everything is checked before anything starts.
/// @return Error{InvalidConfig} for any bad argument, file or setting; Error{Io} if a file cannot
/// be read.
[[nodiscard]] Result<Startup> prepare(std::span<const std::string_view> args, ISyscalls& sys);

/// The text printed for --help and after a usage error.
[[nodiscard]] std::string_view usage() noexcept;

}  // namespace wsrv::wsrvd

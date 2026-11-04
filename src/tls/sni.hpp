// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

/// Host-name rules for choosing a certificate by SNI (ADR-0019 section 7; RFC 6066 section 3).
/// Pure functions: no TLS library, so the rules are tested alone and the OpenSSL callback stays a
/// thin caller. The name in a ClientHello is peer-controlled, so it is checked (character set,
/// length) before it is compared with anything.
namespace wsrv::sni {

inline constexpr std::size_t kMaxNameLength = 253;  ///< longest DNS name in text form
inline constexpr std::size_t kMaxLabelLength = 63;

/// A configured pattern: a DNS name, or "*." followed by a name with at least two labels (one
/// wildcard, leftmost only, never over a whole top-level domain). Letters are lower-cased.
/// @return the normalised pattern, or nothing if it is malformed.
[[nodiscard]] std::optional<std::string> normalise_pattern(std::string_view pattern);

/// A name received from a client: a plain DNS name (no wildcard, no trailing dot, no control or
/// space characters, at most 253 characters), lower-cased.
/// @return the normalised name, or nothing if it is not usable (the caller then serves the default
/// certificate; a hostile name is never an error and never matches an entry).
[[nodiscard]] std::optional<std::string> normalise_host(std::string_view host);

/// Both arguments normalised. An exact pattern matches only itself; "*.suffix" matches exactly one
/// label in front of ".suffix" (RFC 6125 section 6.4.3: not "suffix", not "a.b.suffix").
[[nodiscard]] bool matches(std::string_view pattern, std::string_view host);

}  // namespace wsrv::sni

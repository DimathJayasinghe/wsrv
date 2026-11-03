// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <string_view>

namespace wsrv {

/// Coarse class of a failure; decides how the server reacts (docs/ARCHITECTURE.md section 8).
enum class ErrorCategory : std::uint8_t { Protocol, Limit, Io, Tls, Config, Internal };

/// Specific failure reasons. Extended milestone by milestone; never renumbered or reused.
enum class ErrorCode : std::uint8_t {
    InvalidArgument,    ///< Caller bug or impossible request (Internal).
    OutOfRange,         ///< Index or size outside valid bounds (Internal).
    Overflow,           ///< Arithmetic would wrap around (Limit).
    BufferFull,         ///< A bounded buffer cannot take more data (Limit).
    InvalidBase64,      ///< Input is not strict, canonical Base64 (Protocol).
    InvalidUtf8,        ///< Input is not well-formed UTF-8 (Protocol).
    RandomUnavailable,  ///< The system random source failed (Internal).
    ResourceExhausted,  ///< An allocation failed; the operation did nothing (Limit).
    Closing,            ///< The session is closing or closed; the request was refused (Internal).
    Backpressure,       ///< An outbound queue is full; retry after it drains (Limit).
    InvalidConfig,      ///< A configuration value is out of range or missing (Config).
    WouldBlock,         ///< Not ready yet (EAGAIN): wait for readiness and retry (Io).
    ConnectionClosed,   ///< The peer is gone: EPIPE, ECONNRESET, ECONNABORTED, ETIMEDOUT... (Io).
    FdLimit,            ///< Out of descriptors or kernel buffers: EMFILE, ENFILE, ENOBUFS (Limit).
    AddressInUse,       ///< bind() failed because the address is taken (Io).
    Io,                 ///< Any other operating-system failure (Io).
    MessageInProgress,  ///< A streamed message is open; no other data message may start (Internal).
    WrongProcess,       ///< Called from a forked child of the process that created it (Internal).
    InvalidFrame,       ///< A peer's IPC frame is malformed (Protocol).
};

/// A failure. `context` MUST be a string literal (static storage): it is never built from peer
/// data, so errors cannot allocate, dangle, or smuggle attacker-controlled text into logs.
struct Error {
    ErrorCode code;
    std::string_view context;

    friend constexpr bool operator==(const Error&, const Error&) = default;
};

/// @return The category of `code`. Thread-affinity: any-thread.
[[nodiscard]] constexpr ErrorCategory category(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::InvalidBase64:
    case ErrorCode::InvalidUtf8:
    case ErrorCode::InvalidFrame:
        return ErrorCategory::Protocol;
    case ErrorCode::Overflow:
    case ErrorCode::BufferFull:
    case ErrorCode::ResourceExhausted:
    case ErrorCode::Backpressure:
    case ErrorCode::FdLimit:
        return ErrorCategory::Limit;
    case ErrorCode::WouldBlock:
    case ErrorCode::ConnectionClosed:
    case ErrorCode::AddressInUse:
    case ErrorCode::Io:
        return ErrorCategory::Io;
    case ErrorCode::InvalidArgument:
    case ErrorCode::OutOfRange:
    case ErrorCode::RandomUnavailable:
    case ErrorCode::Closing:
    case ErrorCode::MessageInProgress:
    case ErrorCode::WrongProcess:
        return ErrorCategory::Internal;
    case ErrorCode::InvalidConfig:
        return ErrorCategory::Config;
    }
    return ErrorCategory::Internal;  // unreachable for valid enumerators; fail safe
}

/// @return A stable, human-readable identifier such as "BufferFull". Thread-affinity: any-thread.
[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;

/// Either a value of type T or an Error (ADR-0006). Functions returning it are `[[nodiscard]]`.
template <class T>
using Result = std::expected<T, Error>;

/// Build the failure branch of a Result: `return fail(ErrorCode::BufferFull, "outbound queue");`
[[nodiscard]] constexpr std::unexpected<Error> fail(ErrorCode code,
                                                    std::string_view context = {}) noexcept {
    return std::unexpected<Error>{Error{code, context}};
}

}  // namespace wsrv

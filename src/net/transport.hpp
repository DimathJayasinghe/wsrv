// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/bytes.hpp"
#include "base/error.hpp"

#include <cstddef>

namespace wsrv {

/// A byte stream to one peer, non-blocking. The connection layer talks only to this interface, so
/// TLS (M8) is a decorator that wraps another ITransport (docs/ARCHITECTURE.md section 3.5).
///
/// read():  success with n > 0 bytes; success with 0 means the peer has finished sending (EOF);
///          Error{WouldBlock} means "nothing now, wait for readiness";
///          Error{ConnectionClosed} / other errors mean the stream is dead.
/// write(): success with the number of bytes accepted (possibly fewer than offered: a partial
///          write is normal); Error{WouldBlock} when the kernel buffer is full;
///          Error{ConnectionClosed} when the peer is gone (never a signal).
/// Thread-affinity: loop-thread-only.
class ITransport {
public:
    ITransport() = default;
    ITransport(const ITransport&) = delete;
    ITransport& operator=(const ITransport&) = delete;
    ITransport(ITransport&&) = delete;
    ITransport& operator=(ITransport&&) = delete;
    virtual ~ITransport() = default;

    [[nodiscard]] virtual Result<std::size_t> read(MutableByteView out) = 0;
    [[nodiscard]] virtual Result<std::size_t> write(ByteView data) = 0;
    /// Tell the peer we will send nothing more (TCP FIN); reading still works.
    virtual void shutdown_write() noexcept = 0;
    /// Release the descriptor. Idempotent.
    virtual void close() noexcept = 0;
    /// The descriptor to register with the poller, or -1 once closed.
    [[nodiscard]] virtual int fd() const noexcept = 0;

    /// True while the transport holds bytes it must send but the descriptor refused them (a TLS
    /// handshake flight). The connection then asks the poller for "writable" and calls read()
    /// again when it comes, which lets the transport continue. Edge-triggered epoll reports a
    /// change once: without this hint a handshake that needs to write while the connection only
    /// listens would never be woken. Plain transports never need it.
    [[nodiscard]] virtual bool wants_write() const noexcept {
        return false;
    }

    /// True while the transport holds input it already took from the descriptor but has not handed
    /// out (TLS: decrypted data or whole records not yet processed). The kernel will not report
    /// that data again, so a connection that stopped reading early (read budget, back-pressure)
    /// must schedule the next read itself. Plain transports never buffer.
    [[nodiscard]] virtual bool buffered_input() const noexcept {
        return false;
    }
};

}  // namespace wsrv

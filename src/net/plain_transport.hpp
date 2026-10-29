// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "net/fd.hpp"
#include "net/syscalls.hpp"
#include "net/transport.hpp"

namespace wsrv {

/// ITransport over a plain non-blocking TCP socket. Every send uses MSG_NOSIGNAL, so a write to a
/// closed peer is an Error{ConnectionClosed}, never a SIGPIPE that kills the process (EC-N03).
/// Thread-affinity: loop-thread-only.
class PlainTransport final : public ITransport {
public:
    PlainTransport(Fd fd, ISyscalls& sys) noexcept : fd_{std::move(fd)}, sys_{&sys} {}

    [[nodiscard]] Result<std::size_t> read(MutableByteView out) override;
    [[nodiscard]] Result<std::size_t> write(ByteView data) override;
    void shutdown_write() noexcept override;
    void close() noexcept override;
    [[nodiscard]] int fd() const noexcept override {
        return fd_.get();
    }

private:
    Fd fd_;
    ISyscalls* sys_;
};

}  // namespace wsrv

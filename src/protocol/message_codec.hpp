// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/bytes.hpp"
#include "base/error.hpp"
#include "protocol/permessage_deflate.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace wsrv {

/// Progress of one inflate call.
struct InflateStep {
    std::size_t consumed = 0;  ///< input bytes taken from `in`
    std::size_t produced = 0;  ///< bytes written to `out`
    bool done = false;         ///< finish() only: the message is complete and fully drained
};

/// The receive half of permessage-deflate (RFC 7692 section 7.2.2): turns the compressed payload
/// of one message into the original bytes, a bounded piece at a time. It never allocates sized
/// from the input: the caller owns `out` and so decides the peak memory (ADR-0020 section 4). The
/// limits (message size, ratio) are the caller's: it sees every produced byte and stops asking.
///
/// Protocol of a message: call inflate() with the payload of each frame, and again with the
/// unconsumed rest of the input while `produced == out.size()` or `consumed < in.size()` (more
/// output may be waiting); after the last frame call finish() until it reports done. The
/// instance is then ready for the next message (and was reset if the connection agreed to
/// no_context_takeover).
///
/// Failure: ErrorCode::InvalidCompressedData for bytes that are not valid DEFLATE (the caller
/// closes with 1007), ErrorCode::ResourceExhausted if memory ran out. After a failure the
/// instance must be dropped.
///
/// Thread-affinity: loop-thread-only (one instance per connection).
class IMessageInflater {
public:
    IMessageInflater() = default;
    IMessageInflater(const IMessageInflater&) = delete;
    IMessageInflater& operator=(const IMessageInflater&) = delete;
    IMessageInflater(IMessageInflater&&) = delete;
    IMessageInflater& operator=(IMessageInflater&&) = delete;
    virtual ~IMessageInflater() = default;

    [[nodiscard]] virtual Result<InflateStep> inflate(ByteView in, MutableByteView out) = 0;

    /// Supply the four bytes the sender removed (00 00 ff ff, section 7.2.1) and drain what they
    /// release. `done` is set when no output is pending and the message ended at a DEFLATE block
    /// boundary; a message that does not is InvalidCompressedData.
    [[nodiscard]] virtual Result<InflateStep> finish(MutableByteView out) = 0;
};

/// The send half (RFC 7692 section 7.2.1): compresses one whole message. Fragmenting is done
/// afterwards by the scheduler, so the context stays simple (FRAGMENTATION.md section 2.4).
///
/// Thread-affinity: loop-thread-only (one instance per connection).
class IMessageDeflater {
public:
    IMessageDeflater() = default;
    IMessageDeflater(const IMessageDeflater&) = delete;
    IMessageDeflater& operator=(const IMessageDeflater&) = delete;
    IMessageDeflater(IMessageDeflater&&) = delete;
    IMessageDeflater& operator=(IMessageDeflater&&) = delete;
    virtual ~IMessageDeflater() = default;

    /// Append the compressed form of `message` to `out`, without the trailing 00 00 ff ff. The
    /// output is at most a few bytes larger than the input (deflate's worst case), which callers
    /// check to decide whether sending it is worthwhile (EC-D07). Failure:
    /// ErrorCode::ResourceExhausted.
    [[nodiscard]] virtual Result<void> deflate(ByteView message, std::vector<std::byte>& out) = 0;
};

/// The codecs of one connection.
struct DeflateCodecs {
    std::unique_ptr<IMessageInflater> inflater;
    std::unique_ptr<IMessageDeflater> deflater;
};

/// Builds the codecs for the parameters agreed in the handshake. The composition root chooses the
/// implementation (extensions/ with zlib, or a fake in tests); protocol/ never names zlib.
///
/// Thread-affinity: any-thread (immutable after construction); create() runs on the loop thread.
class IDeflateCodecFactory {
public:
    IDeflateCodecFactory() = default;
    IDeflateCodecFactory(const IDeflateCodecFactory&) = delete;
    IDeflateCodecFactory& operator=(const IDeflateCodecFactory&) = delete;
    IDeflateCodecFactory(IDeflateCodecFactory&&) = delete;
    IDeflateCodecFactory& operator=(IDeflateCodecFactory&&) = delete;
    virtual ~IDeflateCodecFactory() = default;

    [[nodiscard]] virtual Result<DeflateCodecs> create(const DeflateParams& params) const = 0;
};

}  // namespace wsrv

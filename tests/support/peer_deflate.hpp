// SPDX-License-Identifier: Apache-2.0
// The compressor and decompressor of a scripted WebSocket peer, built on the real zlib codecs: what
// a client does with permessage-deflate (RFC 7692) when a test talks to the server. Not part of
// the product.
#pragma once

#include "base/assert.hpp"
#include "base/bytes.hpp"
#include "extensions/zlib_codec.hpp"
#include "protocol/message_codec.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wsrv::test {

/// The compressor of a scripted client: turns a message into the payload of its first (RSV1)
/// frame, without the trailing 00 00 ff ff (section 7.2.1).
class PeerCompressor {
public:
    explicit PeerCompressor(bool no_context_takeover = false) {
        DeflateParams params;
        // The factory builds deflaters from the "server" fields: that is the direction we need.
        params.server_no_context_takeover = no_context_takeover;
        auto codecs = ZlibCodecFactory{ZlibSettings{}}.create(params);
        WSRV_ASSERT(codecs.has_value());
        codecs_ = std::move(*codecs);
    }

    std::string compress(std::string_view message) {
        std::vector<std::byte> out;
        const Result<void> done = codecs_.deflater->deflate(as_bytes(message), out);
        WSRV_ASSERT(done.has_value());
        return std::string{as_chars(out)};
    }

private:
    DeflateCodecs codecs_;
};

/// The decompressor of a scripted client (context takeover on, which also reads messages that were
/// compressed with no_context_takeover).
class PeerDecompressor {
public:
    PeerDecompressor() {
        auto codecs = ZlibCodecFactory{ZlibSettings{}}.create(DeflateParams{});
        WSRV_ASSERT(codecs.has_value());
        codecs_ = std::move(*codecs);
    }

    /// The message carried by `compressed` (the payload of an RSV1 frame), or "<invalid>".
    std::string decompress(std::string_view compressed) {
        std::vector<std::byte> out;
        std::vector<std::byte> buffer(4096);
        ByteView rest = as_bytes(compressed);
        while (!rest.empty()) {
            auto step = codecs_.inflater->inflate(rest, buffer);
            if (!step) {
                return "<invalid>";
            }
            out.insert(out.end(),
                       buffer.begin(),
                       buffer.begin() + static_cast<std::ptrdiff_t>(step->produced));
            rest = rest.subspan(step->consumed);
            if (rest.empty() && step->produced < buffer.size()) {
                break;
            }
        }
        for (;;) {
            auto step = codecs_.inflater->finish(buffer);
            if (!step) {
                return "<invalid>";
            }
            out.insert(out.end(),
                       buffer.begin(),
                       buffer.begin() + static_cast<std::ptrdiff_t>(step->produced));
            if (step->done) {
                return std::string{as_chars(out)};
            }
        }
    }

private:
    DeflateCodecs codecs_;
};

}  // namespace wsrv::test

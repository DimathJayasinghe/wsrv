// SPDX-License-Identifier: Apache-2.0
// Codecs that copy their input. They let server tests exercise the permessage-deflate wiring (what
// was agreed, who may send RSV1) without zlib; real compression is tested in extensions/ and in the
// end-to-end tests.
#pragma once

#include "protocol/message_codec.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace wsrv::test {

class IdentityInflater final : public IMessageInflater {
public:
    Result<InflateStep> inflate(ByteView in, MutableByteView out) override {
        const std::size_t count = std::min(in.size(), out.size());
        std::copy_n(in.begin(), count, out.begin());
        InflateStep step;
        step.consumed = count;
        step.produced = count;
        return step;
    }

    Result<InflateStep> finish(MutableByteView /*out*/) override {
        InflateStep step;
        step.done = true;
        return step;
    }
};

class IdentityDeflater final : public IMessageDeflater {
public:
    Result<void> deflate(ByteView message, std::vector<std::byte>& out) override {
        out.insert(out.end(), message.begin(), message.end());
        return {};
    }
};

/// Remembers the parameters of its last create(): what the connection built the codecs from.
class IdentityCodecFactory final : public IDeflateCodecFactory {
public:
    Result<DeflateCodecs> create(const DeflateParams& params) const override {
        last_params = params;
        ++created;
        DeflateCodecs codecs;
        codecs.inflater = std::make_unique<IdentityInflater>();
        codecs.deflater = std::make_unique<IdentityDeflater>();
        return codecs;
    }

    mutable std::optional<DeflateParams> last_params;
    mutable int created = 0;
};

}  // namespace wsrv::test

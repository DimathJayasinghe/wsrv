// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "protocol/message_codec.hpp"

namespace wsrv {

/// Tuning of the zlib compressor (ADR-0020 section 3).
struct ZlibSettings {
    int level = 6;         ///< 1..9
    int memory_level = 6;  ///< 1..9; zlib's own default is 8
};

[[nodiscard]] bool valid(const ZlibSettings& settings) noexcept;

/// IDeflateCodecFactory over system zlib. The z_stream state (zconf.h: inflate about 2^15 bytes plus a
/// few KiB; deflate 2^(windowBits+2) + 2^(memLevel+9) plus a few KiB, so about 48 KiB + a few KiB at
/// window 12 / memLevel 6) is allocated by the first message that needs it and freed with the
/// connection, so a connection that never sees a compressed message costs nothing.
class ZlibCodecFactory final : public IDeflateCodecFactory {
public:
    /// @pre valid(settings)
    explicit ZlibCodecFactory(ZlibSettings settings) noexcept;

    [[nodiscard]] Result<DeflateCodecs> create(const DeflateParams& params) const override;

private:
    ZlibSettings settings_;
};

}  // namespace wsrv

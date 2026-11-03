// SPDX-License-Identifier: Apache-2.0
// libFuzzer target for the sidecar IPC decoder (ADR-0017). Properties:
//  1. chunking never changes the result (whole buffer, byte at a time, data-derived chunks);
//  2. the format is canonical: re-encoding every decoded message gives back exactly the bytes it
//  was
//     decoded from (so a parser/encoder disagreement is a crash, not a latent bug);
//  3. the only failures are the documented ones, and a frame above the limit never gets past the
//     header. Input layout: byte 0 selects the frame limit, the rest is the byte stream.
#include "base/assert.hpp"
#include "base/byte_buffer.hpp"
#include "base/bytes.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ipc/frame_decoder.hpp"

namespace {

struct Outcome {
    std::vector<std::byte> reencoded;  // every decoded message, encoded again, concatenated
    std::optional<wsrv::ErrorCode> failure;

    friend bool operator==(const Outcome&, const Outcome&) = default;
};

std::size_t limit_from(std::uint8_t selector) {
    return std::size_t{8} << (selector & 0x0FU);  // 8 .. 262144
}

Outcome run(wsrv::ByteView data, std::size_t limit, std::span<const std::uint8_t> sizes) {
    wsrv::ipc::FrameDecoder decoder{limit};
    Outcome out;
    std::size_t pos = 0;
    std::size_t index = 0;
    while (pos < data.size()) {
        std::size_t take = data.size() - pos;
        if (!sizes.empty()) {
            take = std::min(take, std::size_t{1} + sizes[index++ % sizes.size()] % 16U);
        }
        const std::vector<std::byte> piece(data.begin() + static_cast<std::ptrdiff_t>(pos),
                                           data.begin() + static_cast<std::ptrdiff_t>(pos + take));
        wsrv::ByteView input{piece};
        pos += take;
        for (;;) {
            const auto message = decoder.next(input);
            if (!message) {
                out.failure = message.error().code;
                return out;
            }
            if (!message->has_value()) {
                break;
            }
            wsrv::ByteBuffer again{std::size_t{1} << 20};
            WSRV_ASSERT(wsrv::ipc::encode(**message, limit, again).has_value());
            const wsrv::ByteView view = again.view();
            out.reencoded.insert(out.reencoded.end(), view.begin(), view.end());
        }
    }
    return out;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    const std::size_t limit = limit_from(data[0]);
    const std::span<const std::uint8_t> raw{data + 1, size - 1};
    const wsrv::ByteView bytes = std::as_bytes(raw);

    const Outcome whole = run(bytes, limit, {});
    const std::uint8_t one = 0;
    WSRV_ASSERT(whole == run(bytes, limit, std::span<const std::uint8_t>{&one, 1}));
    WSRV_ASSERT(whole == run(bytes, limit, raw));

    // Canonical form: what was decoded is a prefix of the input, byte for byte.
    WSRV_ASSERT(whole.reencoded.size() <= bytes.size());
    WSRV_ASSERT(std::equal(whole.reencoded.begin(), whole.reencoded.end(), bytes.begin()));

    const bool failure_is_expected = !whole.failure.has_value() ||
                                     *whole.failure == wsrv::ErrorCode::InvalidFrame ||
                                     *whole.failure == wsrv::ErrorCode::BufferFull;
    WSRV_ASSERT(failure_is_expected);
    return 0;
}

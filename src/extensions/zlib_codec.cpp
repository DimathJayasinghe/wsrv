// SPDX-License-Identifier: Apache-2.0
#include "extensions/zlib_codec.hpp"

#define ZLIB_CONST  // z_stream::next_in becomes const Bytef*, so no const_cast is needed
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>

namespace wsrv {
namespace {

// One zlib call never gets more than this many bytes (avail_in / avail_out are 32-bit); callers
// loop with the rest.
constexpr std::size_t kMaxZlibChunk = std::size_t{1} << 30U;
// The compressor's output grows in steps of this size, never sized from the message length.
constexpr std::size_t kDeflateOutputStep = 16U * 1024U;
// RFC 7692 section 7.2.1: a sync flush ends with an empty stored block; its last four bytes are
// removed before sending and added back before inflating (section 7.2.2).
constexpr std::array<std::byte, 4> kTail{
    std::byte{0x00}, std::byte{0x00}, std::byte{0xff}, std::byte{0xff}};
// zlib's inflate reports "stopped between blocks" in bit 7 of data_type (zlib.h, inflate()).
constexpr int kAtBlockBoundary = 128;
// RFC 7692 section 7.1.2.2: our inflater uses the full window, which decodes any smaller one.
constexpr int kInflateWindowBits = 15;

// std::byte and unsigned char alias any object and have the same size, so reading and writing
// through the converted pointers is well defined (same audited conversion as base/bytes.hpp).
const Bytef* zlib_bytes(const std::byte* data) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): audited, see comment above.
    return reinterpret_cast<const Bytef*>(data);
}
Bytef* zlib_bytes(std::byte* data) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): audited, see comment above.
    return reinterpret_cast<Bytef*>(data);
}

bool in_window_range(int bits) noexcept {
    return bits >= kMinDeflateWindowBits && bits <= kMaxDeflateWindowBits;
}

class ZlibInflater final : public IMessageInflater {
public:
    explicit ZlibInflater(bool no_context_takeover) noexcept
        : no_context_takeover_(no_context_takeover) {}

    ~ZlibInflater() override {
        if (initialised_) {
            inflateEnd(&stream_);
        }
    }

    ZlibInflater(const ZlibInflater&) = delete;
    ZlibInflater& operator=(const ZlibInflater&) = delete;
    ZlibInflater(ZlibInflater&&) = delete;
    ZlibInflater& operator=(ZlibInflater&&) = delete;

    Result<InflateStep> inflate(ByteView in, MutableByteView out) override {
        if (auto ready = ensure_initialised(); !ready) {
            return std::unexpected{ready.error()};
        }
        return run(in, out);
    }

    Result<InflateStep> finish(MutableByteView out) override {
        if (auto ready = ensure_initialised(); !ready) {
            return std::unexpected{ready.error()};
        }
        InflateStep step;
        if (stream_ended_) {
            // The sender used a final block (section 7.2.3.4); there is nothing left to feed.
            end_message();
            step.done = true;
            return step;
        }
        const ByteView rest = ByteView{kTail}.subspan(tail_fed_);
        auto result = run(rest, out);
        if (!result) {
            return result;
        }
        tail_fed_ += result->consumed;
        step.produced = result->produced;  // `consumed` counts our own bytes: not reported
        if (stream_ended_) {
            end_message();
            step.done = true;
            return step;
        }
        if (tail_fed_ == kTail.size() && step.produced < out.size()) {
            // All input used and no output waiting: the message must have ended between blocks.
            if ((stream_.data_type & kAtBlockBoundary) == 0) {
                return fail(ErrorCode::InvalidCompressedData,
                            "deflate: message ends inside a block");
            }
            end_message();
            step.done = true;
        }
        return step;
    }

private:
    Result<void> ensure_initialised() {
        if (initialised_) {
            return {};
        }
        const int status = inflateInit2(&stream_, -kInflateWindowBits);  // negative: raw DEFLATE
        if (status == Z_MEM_ERROR) {
            return fail(ErrorCode::ResourceExhausted, "deflate: cannot allocate inflate state");
        }
        if (status != Z_OK) {
            return fail(ErrorCode::InvalidArgument, "deflate: inflateInit2 failed");
        }
        initialised_ = true;
        return {};
    }

    Result<InflateStep> run(ByteView in, MutableByteView out) {
        if (stream_ended_ && !in.empty()) {
            return fail(ErrorCode::InvalidCompressedData, "deflate: data after the final block");
        }
        const std::size_t in_size = std::min(in.size(), kMaxZlibChunk);
        const std::size_t out_size = std::min(out.size(), kMaxZlibChunk);
        stream_.next_in = zlib_bytes(in.data());
        stream_.avail_in = static_cast<uInt>(in_size);
        stream_.next_out = zlib_bytes(out.data());
        stream_.avail_out = static_cast<uInt>(out_size);
        const int status = ::inflate(&stream_, Z_SYNC_FLUSH);
        switch (status) {
        case Z_OK:
        case Z_BUF_ERROR:  // no progress was possible: not an error by itself
            break;
        case Z_STREAM_END:
            stream_ended_ = true;
            break;
        case Z_MEM_ERROR:
            return fail(ErrorCode::ResourceExhausted, "deflate: out of memory while inflating");
        case Z_DATA_ERROR:
        case Z_NEED_DICT:
            return fail(ErrorCode::InvalidCompressedData, "deflate: invalid compressed data");
        default:
            return fail(ErrorCode::InvalidArgument, "deflate: unexpected inflate status");
        }
        InflateStep step;
        step.consumed = in_size - stream_.avail_in;
        step.produced = out_size - stream_.avail_out;
        return step;
    }

    void end_message() noexcept {
        if (stream_ended_ || no_context_takeover_) {
            // Window and block state start afresh; the allocation is reused.
            inflateReset(&stream_);
        }
        stream_ended_ = false;
        tail_fed_ = 0;
    }

    z_stream stream_{};
    bool no_context_takeover_;
    bool initialised_ = false;
    bool stream_ended_ = false;
    std::size_t tail_fed_ = 0;
};

class ZlibDeflater final : public IMessageDeflater {
public:
    ZlibDeflater(int window_bits, bool no_context_takeover, ZlibSettings settings) noexcept
        : window_bits_(window_bits), no_context_takeover_(no_context_takeover),
          settings_(settings) {}

    ~ZlibDeflater() override {
        if (initialised_) {
            deflateEnd(&stream_);
        }
    }

    ZlibDeflater(const ZlibDeflater&) = delete;
    ZlibDeflater& operator=(const ZlibDeflater&) = delete;
    ZlibDeflater(ZlibDeflater&&) = delete;
    ZlibDeflater& operator=(ZlibDeflater&&) = delete;

    Result<void> deflate(ByteView message, std::vector<std::byte>& out) override {
        if (message.size() > kMaxZlibChunk) {
            return fail(ErrorCode::OutOfRange, "deflate: message too large to compress");
        }
        if (message.empty()) {
            // RFC 7692 section 7.2.3.6: an empty message is one empty stored block, 0x00 once the
            // tail is removed. zlib cannot make it: a second sync flush with no new input is a
            // no-op that emits nothing, which would be an invalid (empty) payload.
            try {
                out.push_back(std::byte{0x00});
            } catch (const std::bad_alloc&) {
                return fail(ErrorCode::ResourceExhausted,
                            "deflate: out of memory while compressing");
            }
            return {};
        }
        if (auto ready = ensure_initialised(); !ready) {
            return ready;
        }
        const std::size_t start = out.size();
        stream_.next_in = zlib_bytes(message.data());
        stream_.avail_in = static_cast<uInt>(message.size());
        try {
            for (;;) {
                const std::size_t used = out.size();
                out.resize(used + kDeflateOutputStep);
                stream_.next_out = zlib_bytes(out.data() + used);
                stream_.avail_out = static_cast<uInt>(kDeflateOutputStep);
                const int status = ::deflate(&stream_, Z_SYNC_FLUSH);
                out.resize(used + kDeflateOutputStep - stream_.avail_out);
                if (status != Z_OK && status != Z_BUF_ERROR) {
                    out.resize(start);
                    return fail(ErrorCode::InvalidArgument, "deflate: unexpected deflate status");
                }
                if (stream_.avail_out != 0) {
                    break;  // the flush is complete when zlib leaves output space unused
                }
            }
        } catch (const std::bad_alloc&) {
            out.resize(start);
            return fail(ErrorCode::ResourceExhausted, "deflate: out of memory while compressing");
        }
        const std::size_t produced = out.size() - start;
        if (produced < kTail.size() ||
            !std::equal(kTail.begin(),
                        kTail.end(),
                        out.end() - static_cast<std::ptrdiff_t>(kTail.size()))) {
            out.resize(start);
            return fail(ErrorCode::InvalidArgument, "deflate: sync flush marker missing");
        }
        out.resize(out.size() - kTail.size());  // section 7.2.1
        if (no_context_takeover_) {
            deflateReset(&stream_);
        }
        return {};
    }

private:
    Result<void> ensure_initialised() {
        if (initialised_) {
            return {};
        }
        const int status = deflateInit2(&stream_,
                                        settings_.level,
                                        Z_DEFLATED,
                                        -window_bits_,  // negative: raw DEFLATE, no zlib header
                                        settings_.memory_level,
                                        Z_DEFAULT_STRATEGY);
        if (status == Z_MEM_ERROR) {
            return fail(ErrorCode::ResourceExhausted, "deflate: cannot allocate deflate state");
        }
        if (status != Z_OK) {
            return fail(ErrorCode::InvalidArgument, "deflate: deflateInit2 failed");
        }
        initialised_ = true;
        return {};
    }

    z_stream stream_{};
    int window_bits_;
    bool no_context_takeover_;
    ZlibSettings settings_;
    bool initialised_ = false;
};

}  // namespace

bool valid(const ZlibSettings& settings) noexcept {
    return settings.level >= 1 && settings.level <= 9 && settings.memory_level >= 1 &&
           settings.memory_level <= 9;
}

ZlibCodecFactory::ZlibCodecFactory(ZlibSettings settings) noexcept : settings_(settings) {}

Result<DeflateCodecs> ZlibCodecFactory::create(const DeflateParams& params) const {
    if (!valid(settings_) || !in_window_range(params.server_window_bits)) {
        return fail(ErrorCode::InvalidConfig, "deflate: unsupported window or level");
    }
    try {
        DeflateCodecs codecs;
        codecs.inflater = std::make_unique<ZlibInflater>(params.client_no_context_takeover);
        codecs.deflater = std::make_unique<ZlibDeflater>(
            params.server_window_bits, params.server_no_context_takeover, settings_);
        return codecs;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "deflate: out of memory");
    }
}

}  // namespace wsrv

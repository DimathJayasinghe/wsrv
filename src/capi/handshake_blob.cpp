// SPDX-License-Identifier: Apache-2.0
#include "capi/handshake_blob.hpp"

#include <cstdint>
#include <limits>

namespace wsrv {

namespace {

void put_u32(std::vector<std::byte>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::byte>((value >> static_cast<unsigned>(shift)) & 0xFFU));
    }
}

void put_u16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>(value >> 8U));
    out.push_back(static_cast<std::byte>(value & 0xFFU));
}

void put_string(std::vector<std::byte>& out, std::string_view text) {
    put_u32(out,
            static_cast<std::uint32_t>(text.size()));  // bounded by the caps in HandshakeRequest
    const ByteView bytes = as_bytes(text);
    out.insert(out.end(), bytes.begin(), bytes.end());
}

/// A cursor that never reads past the end: every read says whether it fit.
class Reader {
public:
    explicit Reader(ByteView bytes) noexcept : bytes_{bytes} {}

    [[nodiscard]] bool u16(std::uint16_t& out) noexcept {
        if (bytes_.size() - at_ < 2) {
            return false;
        }
        out = static_cast<std::uint16_t>((std::to_integer<unsigned>(bytes_[at_]) << 8U) |
                                         std::to_integer<unsigned>(bytes_[at_ + 1]));
        at_ += 2;
        return true;
    }

    [[nodiscard]] bool text(std::string_view& out) noexcept {
        if (bytes_.size() - at_ < 4) {
            return false;
        }
        std::size_t length = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            length = (length << 8U) | std::to_integer<std::size_t>(bytes_[at_ + i]);
        }
        at_ += 4;
        if (length > bytes_.size() - at_) {
            return false;  // the declared length is a claim, not a fact
        }
        out = as_chars(bytes_.subspan(at_, length));
        at_ += length;
        return true;
    }

    [[nodiscard]] bool at_end() const noexcept {
        return at_ == bytes_.size();
    }

private:
    ByteView bytes_;
    std::size_t at_ = 0;
};

[[nodiscard]] bool read_fixed(Reader& in, HandshakeView& view) {
    return in.text(view.peer) && in.text(view.path) && in.text(view.query) &&
           in.text(view.origin) && in.text(view.host) && in.text(view.subprotocol);
}

[[nodiscard]] bool read_lists(Reader& in, HandshakeView& view) {
    std::uint16_t count = 0;
    if (!in.u16(count)) {
        return false;
    }
    for (std::uint16_t i = 0; i < count; ++i) {
        std::string_view item;
        if (!in.text(item)) {
            return false;
        }
        view.offered.push_back(item);
    }
    if (!in.u16(count)) {
        return false;
    }
    for (std::uint16_t i = 0; i < count; ++i) {
        std::string_view name;
        std::string_view value;
        if (!in.text(name) || !in.text(value)) {
            return false;
        }
        view.headers.emplace_back(name, value);
    }
    return true;
}

}  // namespace

std::vector<std::byte> encode_handshake(std::string_view peer, const HandshakeRequest& request) {
    std::vector<std::byte> out;
    for (const std::string_view field : {peer,
                                         std::string_view{request.path},
                                         std::string_view{request.query},
                                         std::string_view{request.origin},
                                         std::string_view{request.host},
                                         std::string_view{request.subprotocol}}) {
        put_string(out, field);
    }
    put_u16(out, static_cast<std::uint16_t>(request.offered_subprotocols.size()));
    for (const std::string& offered : request.offered_subprotocols) {
        put_string(out, offered);
    }
    put_u16(out, static_cast<std::uint16_t>(request.forwarded.size()));
    for (const HttpHeader& header : request.forwarded) {
        put_string(out, header.name);
        put_string(out, header.value);
    }
    return out;
}

std::optional<HandshakeView> decode_handshake(ByteView blob) {
    Reader in{blob};
    HandshakeView view;
    if (!read_fixed(in, view) || !read_lists(in, view) || !in.at_end()) {
        return std::nullopt;
    }
    return view;
}

}  // namespace wsrv

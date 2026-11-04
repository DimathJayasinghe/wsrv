// SPDX-License-Identifier: Apache-2.0
#include "wsrvd/handshake.hpp"

#include "wsrv/wsrv.h"

#include <utility>
#include <variant>

#include "wsrvd/token.hpp"

namespace wsrv::wsrvd {

namespace {

[[nodiscard]] ByteView as_bytes(const std::string& text) noexcept {
    // std::string stores chars; ByteView views the same memory as bytes.
    return ByteView{reinterpret_cast<const std::byte*>(text.data()), text.size()};  // NOLINT
}

[[nodiscard]] Handshake::Outcome refused() {
    return Handshake::Outcome{
        .accepted = false,
        .reply = ipc::Status{.request = 0,
                             .status = static_cast<std::uint8_t>(WSRV_ERR_INVALID_ARGUMENT)}};
}

}  // namespace

Handshake::Handshake(std::string token, std::uint32_t max_frame, bool asks_questions)
    : token_{std::move(token)}, max_frame_{max_frame}, asks_questions_{asks_questions} {}

Handshake::Outcome Handshake::on_first_message(const ipc::Message& first) const {
    const auto* hello = std::get_if<ipc::Hello>(&first);
    if (hello == nullptr || hello->major != ipc::kVersionMajor) {
        return refused();
    }
    if (asks_questions_ && hello->minor < ipc::kHandshakeHookMinor) {
        return refused();  // the same answer as every other refusal: nothing to probe apart
    }
    if (!token_.empty() && !constant_time_equal(hello->token, as_bytes(token_))) {
        return refused();
    }
    return Outcome{.accepted = true,
                   .reply = ipc::HelloOk{.major = ipc::kVersionMajor,
                                         .minor = ipc::kVersionMinor,
                                         .max_frame = max_frame_}};
}

}  // namespace wsrv::wsrvd

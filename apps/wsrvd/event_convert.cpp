// SPDX-License-Identifier: Apache-2.0
#include "wsrvd/event_convert.hpp"

#include "capi/event_queue.hpp"

namespace wsrv::wsrvd {

// The queue reserves room for a close reason of this size; the codec accepts exactly this much.
// If they ever differ, a legal Close could be refused by the encoder and kill the IPC link.
static_assert(ipc::kMaxCloseReason == EventQueue::kReservedCloseBytes,
              "the IPC close reason limit must match what the event queue reserves");

namespace {

[[nodiscard]] ipc::EventKind kind_of(HostEventType type) noexcept {
    switch (type) {
    case HostEventType::Open:
        return ipc::EventKind::Open;
    case HostEventType::Text:
        return ipc::EventKind::Text;
    case HostEventType::Binary:
        return ipc::EventKind::Binary;
    case HostEventType::Close:
        return ipc::EventKind::Close;
    case HostEventType::Error:
    case HostEventType::Handshake:  // wsrvd does not enable the hook yet (ADR-0018 slice 3)
        return ipc::EventKind::Error;
    }
    return ipc::EventKind::Error;  // unreachable for a valid enum; the encoder judges the rest
}

}  // namespace

ipc::Message to_ipc_event(const HostEvent& event) {
    ipc::Event out;
    out.kind = kind_of(event.type);
    out.conn = event.conn;
    out.close_code = event.type == HostEventType::Close ? event.close_code : std::uint16_t{0};
    out.clean = event.type == HostEventType::Close && event.clean;
    out.data = ByteView{event.data};
    return out;
}

std::size_t encoded_event_size(const HostEvent& event) noexcept {
    return ipc::kFrameHeaderSize + kEventBodyOverhead + event.data.size();
}

}  // namespace wsrv::wsrvd

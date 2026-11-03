// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "capi/host_event.hpp"
#include "ipc/message.hpp"

#include <cstddef>

namespace wsrv::wsrvd {

/// Bytes an EVENT body has besides the payload: u8 kind, u64 connection, u16 close code, u8 clean.
inline constexpr std::size_t kEventBodyOverhead = 12;

/// A view of `event` as an IPC message. The result points into `event.data`, so `event` must
/// outlive it. The core's event shapes and the codec's rules agree (see event_convert_test.cpp).
/// Thread-affinity: any-thread (pure).
[[nodiscard]] ipc::Message to_ipc_event(const HostEvent& event);

/// The exact size of the EVENT frame for `event`, header included. The pump uses it to decide
/// whether the frame can ever fit the configured limit and the write buffer.
[[nodiscard]] std::size_t encoded_event_size(const HostEvent& event) noexcept;

}  // namespace wsrv::wsrvd

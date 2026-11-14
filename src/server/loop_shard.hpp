// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/clock.hpp"
#include "base/error.hpp"
#include "base/logger.hpp"
#include "base/metrics.hpp"
#include "net/endpoint.hpp"
#include "net/event_loop.hpp"
#include "net/fd.hpp"
#include "net/syscalls.hpp"
#include "net/transport_wrapper.hpp"
#include "protocol/handshake.hpp"
#include "server/config.hpp"
#include "server/connection.hpp"
#include "server/connection_table.hpp"
#include "server/handler.hpp"
#include "server/handshake_decider.hpp"
#include "server/ip_limits.hpp"
#include "server/loop_host.hpp"
#include "server/pending_gate.hpp"
#include "server/session_handle.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

namespace wsrv {

/// What every shard of one server shares. All of it is immutable or thread-safe (ADR-0014
/// section 2): the loops never share anything else.
struct ShardShared {
    const ServerConfig& config;
    const HandshakeNegotiator& negotiator;  // const after construction
    IHandlerFactory& factory;               // create() may be called from several loops at once
    ISyscalls& sys;
    const IClock& clock;
    ILogger& logger;                       // must be thread-safe once threads > 1
    std::atomic<std::size_t>& live;        // connections alive in all shards (the global cap)
    IHandshakeDecider* decider = nullptr;  // null = valid upgrades are accepted at once
    PendingGate* pending = nullptr;        // the bound on upgrades waiting for the decider
    ITransportWrapper* wrapper = nullptr;  // null = plain TCP; else wraps each accepted transport
    IpLimits* ip_limits = nullptr;         // null = no per-IP limits; else its slot is given back
    MetricBanks& banks;                    // each shard claims one bank for its counters
};

/// Everything that belongs to one loop thread: the loop, the connections it serves and what they
/// need to run (host, shared read buffer, handle link). Nothing here is shared between shards;
/// a connection never touches another shard.
///
/// Destruction order is part of the design (see ADR-0013 and server.hpp): everything a connection
/// touches while it is being destroyed is declared before the table, so the table (and its
/// connections) is destroyed first. The link is closed first of all, so a foreign thread holding
/// a SessionHandle gets Closing instead of reaching freed memory.
///
/// Thread-affinity: create, adopt, begin_shutdown, abort_all, the destructor: the shard's loop
/// thread (the thread that created it). loop().post/stop: any thread.
class LoopShard {
public:
    /// @param on_drained called on this shard's loop thread, at most once, when a shutdown was
    /// begun and the last connection is gone.
    /// @return Error from the loop (descriptors, memory) or Error{ResourceExhausted}.
    [[nodiscard]] static Result<std::unique_ptr<LoopShard>>
    create(const ShardShared& shared, std::function<void()> on_drained);

    LoopShard(const LoopShard&) = delete;
    LoopShard& operator=(const LoopShard&) = delete;
    LoopShard(LoopShard&&) = delete;
    LoopShard& operator=(LoopShard&&) = delete;
    ~LoopShard();

    [[nodiscard]] EventLoop& loop() noexcept {
        return *loop_;
    }
    /// This loop's counters (ADR-0023); the loop thread writes, any thread may read.
    [[nodiscard]] CounterBank& metrics() noexcept {
        return bank_;
    }

    /// Serve an accepted client. The caller already counted it in `shared.live`; if it cannot be
    /// served (out of memory) the count is given back and the descriptor closed.
    void adopt(Fd client, const Endpoint& peer, ConnectionId id);

    /// Graceful shutdown (FR-S5): every connection gets Close 1001. Reports "drained" when none
    /// is left (immediately if there was none).
    void begin_shutdown();
    /// The grace period is over: close every transport now.
    void abort_all();

    /// Construction goes through create() (passkey, so make_unique can call it).
    class Key {
        friend class LoopShard;
        Key() = default;
    };
    LoopShard(Key /*unused*/,
              const ShardShared& shared,
              std::unique_ptr<EventLoop> loop,
              std::function<void()> on_drained);

private:
    void on_removed(const Endpoint& peer);
    void give_back(const Endpoint& peer);
    void report_drained();

    const ShardShared shared_;
    CounterBank& bank_;  // claimed from shared_.banks; owned by the server, which outlives us
    std::function<void()> on_drained_;
    std::unique_ptr<EventLoop> loop_;
    std::vector<std::byte> scratch_;
    std::unique_ptr<LoopHost> host_;
    std::shared_ptr<LoopLink> link_;
    std::unique_ptr<ConnectionContext> context_;
    std::unique_ptr<ConnectionTable> table_;  // last: destroyed first

    bool shutting_down_ = false;
    bool drained_reported_ = false;
};

}  // namespace wsrv

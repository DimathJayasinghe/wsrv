// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/clock.hpp"
#include "base/error.hpp"
#include "base/logger.hpp"
#include "net/acceptor.hpp"
#include "net/endpoint.hpp"
#include "net/event_loop.hpp"
#include "net/signal_watcher.hpp"
#include "net/syscalls.hpp"
#include "net/tcp_listener.hpp"
#include "protocol/handshake.hpp"
#include "server/config.hpp"
#include "server/handler.hpp"
#include "server/loop_shard.hpp"
#include "server/worker_thread.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace wsrv {

/// Collaborators injected at the composition root. Null means "use the real one" (Linux system
/// calls, the steady clock, no logging). `factory` is required.
/// With `threads > 1` the factory and the logger are called from several loop threads at once and
/// must be thread-safe (ADR-0014).
struct ServerDeps {
    IHandlerFactory* factory = nullptr;
    ISyscalls* sys = nullptr;
    const IClock* clock = nullptr;
    ILogger* logger = nullptr;
};

/// A WebSocket server (Facade): accepts connections, runs them on one or more loops, shuts them
/// down. `ServerConfig::threads` loops run; loop 0 runs on the thread that calls run(), owns the
/// listener and also serves connections, the others run on threads the server starts inside run()
/// and stops before it returns. The acceptor hands each new connection to the next loop in turn
/// (ADR-0014). With one thread nothing is handed over.
///
/// Lifecycle: create() on the thread that will call run() (loop 0 belongs to that thread),
/// optionally install_signal_handlers(), then run(), which returns after a shutdown or stop().
/// request_shutdown() and stop() may be called from any thread while run() is active:
///  - request_shutdown(): graceful (FR-S5, EC-C18). Stop accepting, send Close 1001 to every
///    client, wait up to `shutdown_grace`, then drop the rest. A second request means "now".
///  - stop(): end run() at once, without closing handshakes.
/// Destroy the Server on the loop-0 thread, after run() has returned (the loop checks its thread).
///
/// Thread-affinity: create/run/install_signal_handlers/destructor on the loop-0 thread;
/// request_shutdown and stop are any-thread.
class Server {
public:
    /// @param config must have passed validate(); the HandshakeConfig is validated here.
    /// @return Error{InvalidConfig} (bad config), Error{AddressInUse} / Error{Io} (bind failed,
    /// EC-N20), Error{InvalidArgument} (no handler factory).
    [[nodiscard]] static Result<std::unique_ptr<Server>> create(ServerConfig config,
                                                                const ServerDeps& deps);

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;
    ~Server();

    /// Start the other loops, run loop 0 until stop() or the end of a shutdown, then stop and
    /// join the other loops. @return the first loop's fatal error, or the error that kept a loop
    /// thread from starting. Call at most once.
    [[nodiscard]] Result<void> run();

    /// Turn SIGINT and SIGTERM into a graceful shutdown (a second one forces it). Blocks the
    /// signals for the calling thread: call it on the loop-0 thread before run(), so the loop
    /// threads started by run() inherit the mask.
    [[nodiscard]] Result<void> install_signal_handlers();

    [[nodiscard]] Result<void> request_shutdown();
    void stop() noexcept;

    /// The address actually bound (the real port when 0 was configured).
    [[nodiscard]] Endpoint local_endpoint() const;
    /// Connections refused because max_connections was reached. Any-thread.
    [[nodiscard]] std::uint64_t rejected_connections() const noexcept {
        return rejected_.load();
    }

    /// Construction goes through create() (passkey, so make_unique can call it).
    class Key {
        friend class Server;
        Key() = default;
    };
    Server(Key /*unused*/, ServerConfig config, const ServerDeps& deps);

private:
    [[nodiscard]] Result<void> init();
    [[nodiscard]] Result<void> start_workers();
    void stop_workers();
    void on_accept(Fd client, const Endpoint& peer);
    void hand_off(std::size_t worker, Fd client, const Endpoint& peer, ConnectionId id);
    void begin_shutdown();
    void force_shutdown();
    void on_shard_drained();
    void on_signal(int signo);

    ServerConfig config_;
    IHandlerFactory& factory_;
    LinuxSyscalls own_sys_;
    SteadyClock own_clock_;
    NullLogger own_logger_;
    ISyscalls& sys_;
    const IClock& clock_;
    ILogger& logger_;

    // Non-null once init() succeeded (create() returns only then).
    // Destruction runs in reverse declaration order. The acceptor and the signal watcher come
    // last: they are destroyed first and stop feeding new work. Then the worker threads (their
    // shards die on their own threads), then loop 0's shard, which everything above may use.
    std::atomic<std::size_t> live_{0};  // connections alive in every shard: the global cap
    std::unique_ptr<HandshakeNegotiator> negotiator_;
    std::unique_ptr<ShardShared> shared_;
    std::unique_ptr<LoopShard> main_;  // shard 0, on the thread that created the server
    std::vector<std::unique_ptr<WorkerThread>> workers_;  // shards 1..threads-1, started by run()
    std::unique_ptr<TcpListener> listener_;
    std::unique_ptr<Acceptor> acceptor_;
    std::unique_ptr<SignalWatcher> signals_;

    // Loop-0-thread state.
    ConnectionId last_id_ = 0;
    std::size_t next_shard_ = 0;
    bool shutting_down_ = false;
    std::size_t shards_to_drain_ = 0;  // set by begin_shutdown() before any shard is told
    std::optional<TimerHandle> grace_timer_;

    std::atomic<std::size_t> drained_{0};
    std::atomic<std::uint64_t> rejected_{0};
};

}  // namespace wsrv

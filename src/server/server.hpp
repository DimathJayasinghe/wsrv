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
#include "server/connection.hpp"
#include "server/connection_table.hpp"
#include "server/handler.hpp"
#include "server/loop_host.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace wsrv {

/// Collaborators injected at the composition root. Null means "use the real one" (Linux system
/// calls, the steady clock, no logging). `factory` is required.
struct ServerDeps {
    IHandlerFactory* factory = nullptr;
    ISyscalls* sys = nullptr;
    const IClock* clock = nullptr;
    ILogger* logger = nullptr;
};

/// A single-loop WebSocket server (Facade): accepts connections, runs them, shuts them down.
///
/// Lifecycle: create() on the thread that will call run() (the loop belongs to that thread),
/// optionally install_signal_handlers(), then run(), which returns after a shutdown.
/// request_shutdown() and stop() may be called from any thread:
///  - request_shutdown(): graceful (FR-S5, EC-C18). Stop accepting, send Close 1001 to every
///    client, wait up to `shutdown_grace`, then drop the rest. A second request means "now".
///  - stop(): end run() at once, without closing handshakes.
/// Destroy the Server on the loop thread, after run() has returned (the loop checks its thread).
///
/// Thread-affinity: create/run/install_signal_handlers/destructor on the loop thread;
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

    /// Run until stop() or the end of a shutdown. @return the loop's fatal error, if any.
    [[nodiscard]] Result<void> run();

    /// Turn SIGINT and SIGTERM into a graceful shutdown (a second one forces it). Blocks the
    /// signals for the calling thread: call it on the loop thread before any other thread starts.
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
    void on_accept(Fd client, const Endpoint& peer);
    void begin_shutdown();
    void force_shutdown();
    void on_connection_removed();
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
    // Destruction runs in reverse declaration order. Everything a connection touches while it is
    // being destroyed (host, negotiator, scratch, context, loop) must be declared BEFORE the
    // table, so the table (and its connections) is destroyed first. The acceptor and the signal
    // watcher come last: they are destroyed first and stop feeding new work.
    std::unique_ptr<EventLoop> loop_;
    std::unique_ptr<TcpListener> listener_;
    std::unique_ptr<LoopHost> host_;
    std::unique_ptr<HandshakeNegotiator> negotiator_;
    std::vector<std::byte> scratch_;
    std::unique_ptr<ConnectionContext> context_;
    std::unique_ptr<ConnectionTable> table_;
    std::unique_ptr<Acceptor> acceptor_;
    std::unique_ptr<SignalWatcher> signals_;

    ConnectionId last_id_ = 0;
    bool shutting_down_ = false;
    std::optional<TimerHandle> grace_timer_;
    std::atomic<std::uint64_t> rejected_{0};
};

}  // namespace wsrv

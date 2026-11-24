// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/error.hpp"
#include "net/syscalls.hpp"
#include "server/server.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <sys/types.h>

#include "capi/embedded_config.hpp"
#include "capi/host_bridge.hpp"

namespace wsrv {

/// A WebSocket server living inside a host process (ADR-0016): it runs the event loops on its own
/// thread and hands the host everything through poll() and a few commands by connection id. This
/// is the complete embedded-mode behaviour in C++; the C functions in wsrv.h only translate to it.
///
/// Lifecycle: create() holds configuration and the event queue; start() spawns the I/O thread,
/// which builds the Server, runs it and destroys it (the loops check their thread), and returns
/// only when the server is listening or has failed (bind errors, unusable settings: start_error()
/// has the reason). stop() asks for a graceful shutdown (idempotent, any thread); the destructor
/// stops, waits for the grace period and joins. Events stay readable after the server ended, so
/// the host still sees every Close.
///
/// It installs no signal handlers (the host owns signals). Every call from a forked child of the
/// creating process fails with Error{WrongProcess}: threads do not survive fork(), so a server
/// must be created after the fork.
/// Thread-affinity: poll() one consumer thread at a time; everything else any thread.
class EmbeddedServer {
public:
    /// @return Error{InvalidConfig} (event queue limits), Error from the kernel.
    [[nodiscard]] static Result<std::unique_ptr<EmbeddedServer>>
    create(const EmbeddedConfig& config);

    /// Construction goes through create() (passkey, so make_unique can call it).
    class Key {
        friend class EmbeddedServer;
        Key() = default;
    };
    EmbeddedServer(Key /*unused*/, EmbeddedConfig config);

    EmbeddedServer(const EmbeddedServer&) = delete;
    EmbeddedServer& operator=(const EmbeddedServer&) = delete;
    EmbeddedServer(EmbeddedServer&&) = delete;
    EmbeddedServer& operator=(EmbeddedServer&&) = delete;
    ~EmbeddedServer();

    /// Start listening. @return Error{InvalidArgument} if already started, Error{AddressInUse},
    /// Error{InvalidConfig}, or another startup error; start_error() then explains it.
    [[nodiscard]] Result<void> start();

    /// Begin a graceful shutdown. Fine before start, after the end, and more than once.
    [[nodiscard]] Result<void> stop();

    /// The port actually bound (useful with listen_port 0). 0 before a successful start.
    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_.load();
    }
    /// The admin listener's bound port (ADR-0023); 0 when it is off or before a successful start.
    [[nodiscard]] std::uint16_t admin_port() const noexcept {
        return admin_port_.load();
    }
    /// The publish listener's bound port (ADR-0028); 0 when it is off or before a successful start.
    [[nodiscard]] std::uint16_t publish_port() const noexcept {
        return publish_port_.load();
    }
    /// True from a successful start until the loops have stopped.
    [[nodiscard]] bool running() const noexcept {
        return running_.load();
    }
    /// Why start() failed; empty otherwise. Text for people and logs, not for programs.
    [[nodiscard]] const std::string& start_error() const noexcept {
        return start_error_;
    }

    /// Wait up to `timeout` for the next event (0 = only look). Returns nothing on timeout. Before
    /// start, after nothing is left, or from a forked child, it returns nothing.
    [[nodiscard]] std::optional<HostEvent> poll(std::chrono::milliseconds timeout);
    /// A descriptor that becomes readable when events wait, for the host's own event loop; it may
    /// lag poll() by one loop iteration (EventQueue, ADR-0035).
    [[nodiscard]] int event_fd() const noexcept {
        return bridge_->events().event_fd();
    }

    /// Error{WrongProcess} when called from a forked child of the creating process.
    [[nodiscard]] Result<void> check_process() const;

    /// Commands by connection id; see HostBridge. Error{Closing} also covers "not started".
    [[nodiscard]] Result<void> send_text(ConnectionId id, std::string_view text);
    [[nodiscard]] Result<void> send_binary(ConnectionId id, ByteView data);
    [[nodiscard]] Result<void> close(ConnectionId id, CloseCode code, std::string_view reason);

    /// The broker by connection id (ADR-0027); see HostBridge. Error{InvalidArgument} unless the
    /// config key `broker` is true. Any thread.
    [[nodiscard]] Result<void> subscribe(ConnectionId id, std::string_view topic);
    [[nodiscard]] Result<void> unsubscribe(ConnectionId id, std::string_view topic);
    [[nodiscard]] Result<PublishReport>
    publish_text(std::string_view topic, std::string_view text, ConnectionId except = 0);
    [[nodiscard]] Result<PublishReport>
    publish_binary(std::string_view topic, ByteView data, ConnectionId except = 0);
    [[nodiscard]] Result<std::size_t> topic_subscribers(std::string_view topic) const;
    /// Remove every subscription of `id`; does nothing from a forked child or without a broker.
    void drop_subscriptions(ConnectionId id) noexcept;

    /// Answer a HostEventType::Handshake (only with handshake_hook on); see HostBridge.
    [[nodiscard]] Result<void> accept_handshake(ConnectionId id,
                                                std::optional<std::string_view> subprotocol);
    [[nodiscard]] Result<void> reject_handshake(ConnectionId id, int status);
    /// Re-read the certificate and key files (tls_cert / tls_key) and use them for connections
    /// accepted from now on; connections already open keep the old certificate. A reload that fails
    /// (file missing, expired, key mismatch) changes nothing and the server keeps serving.
    /// Call it after start() succeeded; any thread (the daemon calls it on SIGHUP).
    /// @return Error{InvalidArgument} if TLS is not enabled, Error{InvalidConfig} or the like if
    /// the new files are unusable; tls_detail() then says which file and why.
    [[nodiscard]] Result<void> reload_tls();

    /// The reason the last TLS load or reload failed, for people and logs; empty if none.
    [[nodiscard]] std::string tls_detail() const;

    /// See HostBridge::set_handshake_intake. A call from a forked child does nothing.
    void set_handshake_intake(bool open);

private:
    /// The TLS context and wrapper (defined in the .cpp, so this header needs no TLS include and a
    /// build without TLS compiles the same header).
    struct TlsState;

    struct Started {
        Result<std::uint16_t> port;
        std::string detail;
        std::uint16_t admin_port = 0;    ///< 0 = the admin listener is off
        std::uint16_t publish_port = 0;  ///< 0 = the publish listener is off
    };

    void io_thread_main(std::promise<Started>& ready);
    /// Load the certificate when TLS was asked for. Runs in start(), before the thread exists, so
    /// a bad setup is reported by start() and nothing is listening yet.
    [[nodiscard]] Result<void> prepare_tls();
    /// Check the publish settings and build its token and action (ADR-0028). Same timing as TLS.
    [[nodiscard]] Result<void> prepare_publish();

    const pid_t owner_pid_;
    const EmbeddedConfig config_;
    LinuxSyscalls sys_;
    std::unique_ptr<HostBridge> bridge_;
    std::unique_ptr<TlsState> tls_;  // null = plain TCP; set in start(), before the I/O thread

    std::atomic<bool> started_{false};
    std::atomic<bool> running_{false};
    std::atomic<std::uint16_t> port_{0};
    std::atomic<std::uint16_t> admin_port_{0};
    std::atomic<std::uint16_t> publish_port_{0};
    PublishSources publish_sources_;  // the token and the action; lives as long as the server
    std::string start_error_;
    // A pointer so that a forked child can abandon it: it names a thread that exists only in the
    // parent, so the child must neither join nor destroy it (destroying a joinable thread aborts).
    std::unique_ptr<std::thread> thread_;

    std::mutex server_mutex_;  // guards server_ between the I/O thread and stop(); one call long
    Server* server_ = nullptr;
};

}  // namespace wsrv

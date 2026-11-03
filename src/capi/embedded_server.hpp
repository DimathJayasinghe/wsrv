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
    /// A descriptor that is readable while events wait, for the host's own event loop.
    [[nodiscard]] int event_fd() const noexcept {
        return bridge_->events().event_fd();
    }

    /// Error{WrongProcess} when called from a forked child of the creating process.
    [[nodiscard]] Result<void> check_process() const;

    /// Commands by connection id; see HostBridge. Error{Closing} also covers "not started".
    [[nodiscard]] Result<void> send_text(ConnectionId id, std::string_view text);
    [[nodiscard]] Result<void> send_binary(ConnectionId id, ByteView data);
    [[nodiscard]] Result<void> close(ConnectionId id, CloseCode code, std::string_view reason);

    /// Answer a HostEventType::Handshake (only with handshake_hook on); see HostBridge.
    [[nodiscard]] Result<void> accept_handshake(ConnectionId id,
                                                std::optional<std::string_view> subprotocol);
    [[nodiscard]] Result<void> reject_handshake(ConnectionId id, int status);

private:
    struct Started {
        Result<std::uint16_t> port;
        std::string detail;
    };

    void io_thread_main(std::promise<Started>& ready);

    const pid_t owner_pid_;
    const EmbeddedConfig config_;
    LinuxSyscalls sys_;
    std::unique_ptr<HostBridge> bridge_;

    std::atomic<bool> started_{false};
    std::atomic<bool> running_{false};
    std::atomic<std::uint16_t> port_{0};
    std::string start_error_;
    // A pointer so that a forked child can abandon it: it names a thread that exists only in the
    // parent, so the child must neither join nor destroy it (destroying a joinable thread aborts).
    std::unique_ptr<std::thread> thread_;

    std::mutex server_mutex_;  // guards server_ between the I/O thread and stop(); one call long
    Server* server_ = nullptr;
};

}  // namespace wsrv

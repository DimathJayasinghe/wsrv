// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/bytes.hpp"
#include "net/endpoint.hpp"
#include "net/event_loop.hpp"
#include "net/transport.hpp"
#include "protocol/handshake_driver.hpp"
#include "protocol/session.hpp"
#include "server/config.hpp"
#include "server/connection_host.hpp"
#include "server/handler.hpp"

#include <memory>
#include <optional>
#include <string>

namespace wsrv {

/// Read-only wiring shared by every connection of a server (a parameter object).
struct ConnectionContext {
    IConnectionHost& host;
    const ServerConfig& config;
    const HandshakeNegotiator& negotiator;
    IHandlerFactory& factory;
    /// Per-loop read buffer, reused by every connection: a connection holds no inbound buffer of
    /// its own, so an idle one costs almost nothing (NFR-P1). The protocol layer consumes each
    /// chunk completely (it keeps its own small partial-frame state), so sharing is safe.
    MutableByteView scratch;
};

/// One client, from accept to close: the glue between a transport, the handshake, the
/// ProtocolSession and the application's handler (ARCHITECTURE sections 6 and 7). It contains no
/// protocol rules and no policy beyond what ServerConfig says.
///
///   Handshaking --101--> Established --Closed event / transport failure--> Finished
///        \--refusal / timeout / EOF-------------------------------------------^
///
/// Rules this class enforces (ADR-0013, docs/PROTOCOL_EDGE_CASES.md):
///  - Reads are edge-triggered: read until WouldBlock, but at most ReadBudget per event; when the
///    budget runs out the registration is re-armed so the kernel reports the rest next iteration
///    (EC-N07, EC-N08).
///  - Output is written at once; if the kernel is full, Write interest is armed until it drains
///    (EC-N02) and a stall timer closes a peer that accepts nothing (EC-N09, T3).
///  - Handler callbacks run inside one exception barrier: a throw closes with 1011 (FR-S6).
///  - on_close is delivered exactly once, and only if on_open ran.
///  - The object never destroys itself: finishing releases the descriptor and calls
///    IConnectionHost::retire(); the owner deletes it at a safe point.
///
/// Thread-affinity: loop-thread-only.
class Connection final : public IEventHandler, public Session {
public:
    Connection(ConnectionId id,
               const Endpoint& peer,
               std::unique_ptr<ITransport> transport,
               const ConnectionContext& context);
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) = delete;
    Connection& operator=(Connection&&) = delete;
    ~Connection() override;

    /// Register with the loop and start the handshake timer. On failure the connection is finished
    /// (already retired) and the caller just drops it.
    [[nodiscard]] Result<void> start();

    void on_event(PollEvents events) noexcept override;

    /// Graceful shutdown (FR-S5): an established connection gets Close 1001; one still in the
    /// handshake is dropped.
    void begin_shutdown();
    /// Shutdown deadline passed: close the transport now, without waiting for the peer.
    void abort();

    [[nodiscard]] bool finished() const noexcept {
        return phase_ == Phase::Finished;
    }

    // Session
    [[nodiscard]] ConnectionId id() const noexcept override {
        return id_;
    }
    [[nodiscard]] const Endpoint& peer() const noexcept override {
        return peer_;
    }
    [[nodiscard]] Result<void> send_text(std::string_view text) override;
    [[nodiscard]] Result<void> send_binary(ByteView data) override;
    void close(CloseCode code, std::string_view reason) override;

private:
    enum class Phase : std::uint8_t { Handshaking, Established, Finished };

    // reading
    void on_readable();
    void on_bytes(MutableByteView chunk);
    void on_handshake_bytes(MutableByteView chunk);
    void establish(HandshakeOutcome& outcome, MutableByteView leftover);
    void feed_session(MutableByteView chunk);
    void dispatch(const SessionEvent& event);
    void deliver_part(const SessionEvent& event);
    void on_peer_eof();
    [[nodiscard]] ProtocolSession& established_session();

    // writing
    void settle();
    [[nodiscard]] bool drain_events();
    enum class WriteStep : std::uint8_t { Done, Progress, Stopped };
    [[nodiscard]] WriteStep write_once();
    void flush();
    [[nodiscard]] ByteView next_output();
    void consume_output(std::size_t count);
    void set_write_blocked(bool blocked);
    void note_write_progress();
    [[nodiscard]] Result<void> apply_send_policy(Result<void> result);

    // ending
    void transport_failed(const Error& error);
    void drop_transport();
    void finish(const std::optional<CloseInfo>& info);
    void release() noexcept;

    // timers
    [[nodiscard]] bool
    arm(TimerHandle& slot, std::chrono::milliseconds delay, void (Connection::*action)());
    void disarm(TimerHandle& slot) noexcept;
    void on_handshake_timeout();
    void on_tick();
    void on_write_stall();

    // handler barrier
    template <class Call>
    void guarded(Call&& call);

    void log(LogLevel level, std::string_view message) const;

    ConnectionId id_;
    Endpoint peer_;
    std::unique_ptr<ITransport> transport_;
    ConnectionContext ctx_;
    Phase phase_ = Phase::Handshaking;

    HandshakeDriver driver_;
    std::string head_out_;  ///< the handshake response (101 or a refusal) still being written
    std::size_t head_sent_ = 0;
    bool close_after_head_ = false;  ///< a refusal: close once it is written

    std::optional<ProtocolSession> session_;
    std::unique_ptr<IWebSocketHandler> handler_;
    bool opened_ = false;  ///< on_open ran, so on_close is owed

    WatchHandle watch_{};
    bool watching_ = false;
    PollEvents interest_ = PollEvents::Read;
    bool write_blocked_ = false;
    bool transport_dead_ = false;  ///< a read/write failed: never touch the transport again
    bool released_ = false;        ///< release() ran: timers, watch and transport are gone
    TimerHandle handshake_timer_{};
    TimerHandle tick_timer_{};
    TimerHandle stall_timer_{};
};

}  // namespace wsrv

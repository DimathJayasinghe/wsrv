// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/bytes.hpp"
#include "base/metrics.hpp"
#include "net/endpoint.hpp"
#include "net/event_loop.hpp"
#include "net/transport.hpp"
#include "protocol/handshake_driver.hpp"
#include "protocol/session.hpp"
#include "server/config.hpp"
#include "server/connection_host.hpp"
#include "server/handler.hpp"
#include "server/handshake_decider.hpp"
#include "server/handshake_verdict.hpp"
#include "server/ip_limits.hpp"
#include "server/pending_gate.hpp"
#include "server/session_handle.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

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
    /// How a SessionHandle reaches this loop; null (tests, single-use fakes) gives inert handles.
    std::shared_ptr<LoopLink> link;
    /// The asynchronous handshake decision (ADR-0018). Both null = the hook is off and a valid
    /// upgrade is accepted at once. A decider without a gate is a wiring mistake and fails closed.
    IHandshakeDecider* decider = nullptr;
    PendingGate* pending = nullptr;
    /// Where events are counted (ADR-0023): this loop's bank. Null (tests) counts nothing.
    IMetrics* metrics = nullptr;
    /// The per-IP table (ADR-0021), used here only to admit the client address a trusted proxy
    /// names (ADR-0029). Null = no per-IP limits.
    IpLimits* ip_limits = nullptr;
};

/// One client, from accept to close: the glue between a transport, the handshake, the
/// ProtocolSession and the application's handler (ARCHITECTURE sections 6 and 7). It contains no
/// protocol rules and no policy beyond what ServerConfig says.
///
///   Handshaking --101--> Established --Closed event / transport failure--> Finished
///        \--refusal / timeout / EOF-------------------------------------------^
///
/// With a handshake decider (ADR-0018) a valid upgrade first waits:
///
///   Handshaking --valid--> AwaitingDecision --decide(accept)--> Established
///                               \--decide(reject) / timeout / decider full / shutdown--> 4xx/5xx,
///                               close
///                               \--peer gone / abort-------------------------------------->
///                               Finished
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
class Connection final : public IEventHandler, public Session, private IMessageSink {
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
    /// Run a command from outside a handler callback (a SessionHandle's posted task), then write
    /// what it queued. Ignored once the connection is finished.
    void apply(const SessionCommand& command);
    /// The application's answer to a waiting upgrade (ADR-0018). Acts only in AwaitingDecision;
    /// late, repeated and misdirected answers are dropped without effect.
    void decide(const HandshakeVerdict& verdict);
    /// Shutdown deadline passed: close the transport now, without waiting for the peer.
    void abort();

    [[nodiscard]] bool finished() const noexcept {
        return phase_ == Phase::Finished;
    }

    // Session
    [[nodiscard]] ConnectionId id() const noexcept override {
        return id_;
    }
    /// The client address: the TCP peer, or, behind a trusted proxy, the address its header named
    /// once the upgrade request was read (ADR-0029). Port 0 in the second case.
    [[nodiscard]] const Endpoint& peer() const noexcept override {
        return client_;
    }
    /// The address of the socket itself: what the accept-time slots were taken under.
    [[nodiscard]] const Endpoint& tcp_peer() const noexcept {
        return peer_;
    }
    [[nodiscard]] Result<void> send_text(std::string_view text) override;
    [[nodiscard]] Result<void> send_binary(ByteView data) override;
    [[nodiscard]] Result<MessageWriter> begin_text() override;
    [[nodiscard]] Result<MessageWriter> begin_binary() override;
    void pause_reads() override;
    void resume_reads() override;
    void close(CloseCode code, std::string_view reason) override;
    [[nodiscard]] SessionHandle handle() const override {
        return SessionHandle{ctx_.link, id_};
    }

private:
    enum class Phase : std::uint8_t { Handshaking, AwaitingDecision, Established, Finished };

    // reading
    void on_readable(bool peer_closing);
    void on_read_failed(const Error& error);
    void on_bytes(MutableByteView chunk);
    void on_handshake_bytes(MutableByteView chunk);
    [[nodiscard]] bool resolve_client();
    void establish(HandshakeOutcome& outcome, MutableByteView leftover);
    void await_decision(HandshakeOutcome& outcome, ByteView leftover);
    void on_waiting_event(PollEvents events);
    /// Drop the waiting upgrade; tell the decider if it still thinks it owes an answer.
    void end_wait() noexcept;
    void accept_pending(const HandshakeVerdict& verdict);
    /// Answer the upgrade with `status` and close once it is written (no body, Connection: close).
    void refuse(int status);
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
    [[nodiscard]] WriteStep push_held_output();
    void flush();
    [[nodiscard]] ByteView next_output();
    void consume_output(std::size_t count);
    void set_write_blocked(bool blocked);
    void note_write_progress();
    [[nodiscard]] Result<void> apply_send_policy(Result<void> result);

    void apply_interest();
    void notify_writable();

    // streaming writer (IMessageSink): what a MessageWriter calls
    [[nodiscard]] Result<MessageWriter> begin_message(Opcode type);
    [[nodiscard]] Result<void> stream_write(ByteView bytes) override;
    [[nodiscard]] Result<void> stream_finish() override;
    void stream_abandon() noexcept override;

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
    void on_decision_timeout();
    void on_tick();
    void on_write_stall();
    void on_reread();
    void schedule_reread();

    // handler barrier
    template <class Call>
    void guarded(Call&& call);

    void log(LogLevel level, std::string_view message) const;
    void count(Metric metric, std::uint64_t amount = 1) const noexcept;

    ConnectionId id_;
    Endpoint peer_;    ///< the TCP peer
    Endpoint client_;  ///< what peer() reports: peer_, or the client a trusted proxy named
    /// The per-IP slot this connection took itself (a client behind a trusted proxy, ADR-0029);
    /// given back by release(). Empty when the accept path took the slot, or none was needed.
    std::optional<Endpoint> client_slot_;
    std::unique_ptr<ITransport> transport_;
    ConnectionContext ctx_;
    Phase phase_ = Phase::Handshaking;

    HandshakeDriver driver_;
    std::string head_out_;  ///< the handshake response (101 or a refusal) still being written
    std::size_t head_sent_ = 0;
    bool close_after_head_ = false;  ///< a refusal: close once it is written

    /// An upgrade that passed every rule and waits for the application. It owns everything needed
    /// to finish the job later: the rendered 101, the key (to render it again with another
    /// subprotocol), the bytes that arrived behind the head (the read buffer is shared and reused,
    /// so they are copied; at most one read), and its place among the waiting upgrades.
    struct PendingUpgrade {
        HandshakeOutcome outcome;
        std::vector<std::byte> leftover;
        std::optional<PendingSlot> slot;
        bool asked = false;  ///< the decider took the request and has not answered or been told off
    };
    std::optional<PendingUpgrade> pending_;

    std::optional<ProtocolSession> session_;
    std::unique_ptr<IWebSocketHandler> handler_;
    bool opened_ = false;  ///< on_open ran, so on_close is owed

    WatchHandle watch_{};
    bool watching_ = false;
    PollEvents interest_ = PollEvents::Read;
    bool write_blocked_ = false;
    bool reads_paused_ = false;   ///< pause_reads() is in effect: Read interest is dropped
    bool writable_owed_ = false;  ///< a write was refused with Backpressure: on_writable is due
    std::shared_ptr<StreamLink>
        stream_link_;              ///< created by the first writer; cleared by release()
    bool transport_dead_ = false;  ///< a read/write failed: never touch the transport again
    bool released_ = false;        ///< release() ran: timers, watch and transport are gone
    TimerHandle handshake_timer_{};
    TimerHandle tick_timer_{};
    TimerHandle stall_timer_{};
    TimerHandle reread_timer_{};  ///< a read owed because the transport buffers input (TLS)
};

}  // namespace wsrv

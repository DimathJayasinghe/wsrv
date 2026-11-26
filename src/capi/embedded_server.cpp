// SPDX-License-Identifier: Apache-2.0
#include "capi/embedded_server.hpp"

#include <algorithm>
#include <exception>
#include <new>
#include <optional>
#include <string>
#include <utility>

#include <poll.h>
#include <unistd.h>

#if defined(WSRV_HAS_TLS)
#include "tls/server_context.hpp"
#include "tls/tls_transport_wrapper.hpp"
#endif

namespace wsrv {

#if defined(WSRV_HAS_TLS)

namespace {

/// Keeps the first error line the TLS context logged since the last clear() (which file, and why),
/// so start() and reload_tls() can hand it to the host, which needs more than "the certificate
/// could not be read". The first line is the specific one; the context follows it with a generic
/// summary
/// ("reload failed, keeping ..."). Every record also goes on to the server's logger, so the
/// operator sees the same lines on stderr (ADR-0037).
/// Thread-safe: reload runs on any thread.
class DetailLogger final : public ILogger {
public:
    /// Every record also goes to `downstream`, the server's own logger (ADR-0037).
    explicit DetailLogger(ILogger& downstream) noexcept : downstream_{downstream} {}

    using ILogger::log;
    void log(const LogRecord& record) override {
        downstream_.log(record);
        if (record.level < LogLevel::Error) {
            return;
        }
        const std::string_view message = record.message;
        constexpr std::size_t kMaxDetail = 512;  // a path plus OpenSSL's reason fits easily
        const std::lock_guard lock{mutex_};
        if (last_.empty()) {
            last_.assign(message.substr(0, kMaxDetail));
        }
    }
    [[nodiscard]] bool enabled(LogLevel level) const noexcept override {
        return downstream_.enabled(level) || level >= LogLevel::Error;
    }
    void clear() {
        const std::lock_guard lock{mutex_};
        last_.clear();
    }
    [[nodiscard]] std::string last() const {
        const std::lock_guard lock{mutex_};
        return last_;
    }

private:
    ILogger& downstream_;
    mutable std::mutex mutex_;
    std::string last_;
};

}  // namespace

struct EmbeddedServer::TlsState {
    explicit TlsState(ILogger& downstream) noexcept : logger{downstream} {}
    DetailLogger logger;
    std::unique_ptr<TlsServerContext> context;
    std::unique_ptr<TlsTransportWrapper> wrapper;
};

#else

struct EmbeddedServer::TlsState {};  // never created: asking for TLS fails at start()

#endif

Result<std::unique_ptr<EmbeddedServer>> EmbeddedServer::create(const EmbeddedConfig& config) {
    try {
        auto server = std::make_unique<EmbeddedServer>(Key{}, config);
        auto bridge = HostBridge::make(
            server->sys_,
            config.queue(),
            config.broker_enabled() ? std::make_shared<Broker>(config.broker_config()) : nullptr);
        if (!bridge) {
            return std::unexpected{bridge.error()};
        }
        server->bridge_ = std::move(*bridge);
        return server;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "embedded server: out of memory");
    }
}

namespace {

/// The log's destination is the stderr of the moment the server is created, as a private copy
/// numbered 3 or above. Writing to "descriptor 2" later would be wrong when a host closes its stdio
/// after create() (a daemon does) and a socket of ours, or of the host's, takes the number 2: the
/// log lines would go into a client connection (ADR-0037). A host whose stderr is closed gets no
/// logging at all (the copy fails and the logger has nowhere to write).
Fd copy_of_stderr(ISyscalls& sys) {
    const SysResult copy = sys.dup_cloexec(STDERR_FILENO, 3);
    return Fd{copy.value >= 0 ? static_cast<int>(copy.value) : -1};
}

}  // namespace

EmbeddedServer::EmbeddedServer(Key /*unused*/, EmbeddedConfig config)
    : owner_pid_{::getpid()}, config_{std::move(config)}, log_fd_{copy_of_stderr(sys_)},
      logger_{sys_, wall_, clock_, log_fd_.get(), config_.log_level()} {}

EmbeddedServer::~EmbeddedServer() {
    if (!thread_) {
        return;
    }
    if (!check_process().has_value()) {
        // Forked child: the I/O thread exists only in the parent. Joining it is undefined, so
        // abandon the handle (a deliberate, tiny leak in a process that should exec or _exit).
        // NOLINTNEXTLINE(bugprone-unused-return-value): abandoning the handle is the point.
        thread_.release();
        return;
    }
    static_cast<void>(stop().has_value());  // graceful; the I/O thread ends when drained
    thread_->join();
}

Result<void> EmbeddedServer::check_process() const {
    if (fork_guard_.forked(owner_pid_)) {
        return fail(ErrorCode::WrongProcess, "embedded server: used after fork(); create it after");
    }
    return {};
}

// ---- lifecycle ---------------------------------------------------------------------------------

Result<void> EmbeddedServer::start() {
    if (const Result<void> same = check_process(); !same) {
        return same;
    }
    if (started_.exchange(true)) {
        return fail(ErrorCode::InvalidArgument, "embedded server: already started");
    }
    if (const Result<void> publish = prepare_publish(); !publish) {
        started_.store(false);
        return publish;
    }
    if (const Result<void> secured = prepare_tls(); !secured) {
        started_.store(false);  // nothing was started: the host may fix the settings and retry
        return secured;
    }
    std::promise<Started> ready;
    std::future<Started> outcome = ready.get_future();
    try {
        thread_ = std::make_unique<std::thread>([this, &ready] { io_thread_main(ready); });
    } catch (const std::system_error&) {
        started_.store(false);
        return fail(ErrorCode::ResourceExhausted, "embedded server: cannot start a thread");
    } catch (const std::bad_alloc&) {
        started_.store(false);
        return fail(ErrorCode::ResourceExhausted, "embedded server: cannot start a thread");
    }
    Started result = outcome.get();  // the I/O thread answers once, before it runs the loops
    if (!result.port) {
        start_error_ = std::move(result.detail);
        thread_->join();  // it has already ended
        thread_.reset();
        return std::unexpected{result.port.error()};
    }
    port_.store(*result.port);
    admin_port_.store(result.admin_port);
    publish_port_.store(result.publish_port);
    announce_listening(*result.port);
    return {};
}

/// ADR-0037: the line that says the server is up and where. Never fails start().
void EmbeddedServer::announce_listening(std::uint16_t port) noexcept {
    try {
        logger_.log(LogLevel::Info,
                    "listening",
                    "server: websocket listening on port " + std::to_string(port));
        // NOLINTNEXTLINE(bugprone-empty-catch): an optional line, not an error
    } catch (const std::exception&) {
    }
}

/// The publish endpoint's token and action (ADR-0028). The action is the bridge's broker publish:
/// it is called on loop 0 with a message that is already authenticated and bounded. Runs in
/// start(), before the thread exists, so a missing broker or token is reported by start().
Result<void> EmbeddedServer::prepare_publish() {
    if (const Result<void> valid = config_.check_publish(); !valid) {
        start_error_ = "publish_port needs broker=true and a publish token";
        return valid;
    }
    if (!config_.publish_requested()) {
        return {};
    }
    HostBridge* const bridge = bridge_.get();
    try {
        publish_sources_.token = config_.publish_token();
        publish_sources_.publish = [bridge](const PublishMessage& message) {
            if (message.binary) {
                return bridge->publish_binary(message.topic, message.payload, message.except);
            }
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): bytes seen as text
            const std::string_view text{reinterpret_cast<const char*>(message.payload.data()),
                                        message.payload.size()};
            return bridge->publish_text(message.topic, text, message.except);
        };
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "embedded server: out of memory");
    }
    return {};
}

Result<void> EmbeddedServer::prepare_tls() {
    if (!config_.tls_requested()) {
        return {};
    }
    // Asking for TLS and not getting it must never turn into serving plain text.
    if (config_.tls_cert().empty() || config_.tls_key().empty()) {
        if (config_.tls_cert().empty() && config_.tls_key().empty()) {
            start_error_ =
                "tls_sni is set but the default certificate is not: set tls_cert and tls_key";
        } else {
            start_error_ = config_.tls_cert().empty() ? "tls_key is set but tls_cert is not"
                                                      : "tls_cert is set but tls_key is not";
        }
        return fail(ErrorCode::InvalidConfig, "embedded server: incomplete TLS settings");
    }
#if defined(WSRV_HAS_TLS)
    try {
        auto state = std::make_unique<TlsState>(logger_);
        TlsConfig tls{config_.tls_cert(), config_.tls_key(), {}};
        tls.sni.reserve(config_.tls_sni().size());
        for (const TlsSniSetting& setting : config_.tls_sni()) {
            tls.sni.push_back({setting.name, setting.certificate, setting.key});
        }
        auto context = TlsServerContext::load(tls, state->logger);
        if (!context) {
            const std::string detail = state->logger.last();
            start_error_ = detail.empty() ? std::string{context.error().context} : detail;
            return std::unexpected{context.error()};
        }
        state->context = std::move(*context);
        state->wrapper = std::make_unique<TlsTransportWrapper>(*state->context);
        tls_ = std::move(state);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "embedded server: out of memory");
    }
    return {};
#else
    start_error_ = "tls_cert and tls_key are set, but this build has no TLS (WSRV_ENABLE_TLS=OFF)";
    return fail(ErrorCode::InvalidConfig, "embedded server: this build has no TLS");
#endif
}

Result<void> EmbeddedServer::reload_tls() {
    if (const Result<void> same = check_process(); !same) {
        return same;
    }
#if defined(WSRV_HAS_TLS)
    if (tls_) {
        tls_->logger.clear();
        return tls_->context->reload();
    }
#endif
    return fail(ErrorCode::InvalidArgument, "embedded server: TLS is not enabled");
}

std::string EmbeddedServer::tls_detail() const {
#if defined(WSRV_HAS_TLS)
    if (tls_) {
        return tls_->logger.last();
    }
#endif
    return {};
}

/// Runs on the I/O thread for the whole life of the server: the loops belong to the thread that
/// builds them, so build, run and destroy all happen here (ADR-0013).
void EmbeddedServer::io_thread_main(std::promise<Started>& ready) {
    std::unique_ptr<Server> server;
    try {
        ServerBuilder builder;
        config_.apply_to(builder);
        if (config_.publish_requested()) {
            builder.publish(config_.publish_host(), config_.publish_port(), publish_sources_);
        }
        builder.handler_factory(*bridge_);
        builder.logger(logger_);
        if (config_.handshake_hook()) {
            builder.handshake_decider(*bridge_);
        }
#if defined(WSRV_HAS_TLS)
        if (tls_) {
            builder.transport_wrapper(*tls_->wrapper);
        }
#endif
        server = builder.build();
        bridge_->use_broker_metrics(server->shared_metrics());  // the broker's counters (ADR-0024)
    } catch (const ConfigError& error) {
        // The code says what kind of failure it is; the message says which field or address.
        ready.set_value(Started{fail(error.code(), "embedded server: start"), error.what()});
        return;
    } catch (const std::bad_alloc&) {
        ready.set_value(Started{
            fail(ErrorCode::ResourceExhausted, "embedded server: out of memory"), "out of memory"});
        return;
    }
    {
        const std::lock_guard lock{server_mutex_};
        server_ = server.get();
    }
    running_.store(true);
    const std::optional<Endpoint> admin = server->admin_endpoint();
    const std::optional<Endpoint> publish = server->publish_endpoint();
    ready.set_value(Started{server->local_endpoint().port,
                            {},
                            admin ? admin->port : std::uint16_t{0},
                            publish ? publish->port : std::uint16_t{0}});
    static_cast<void>(server->run().has_value());  // a loop failure ends it too; events remain
    {
        const std::lock_guard lock{server_mutex_};
        server_ = nullptr;  // stop() must not touch a server that is about to be destroyed
    }
    running_.store(false);
    server.reset();  // closes what is left; each connection reports its Close
}

Result<void> EmbeddedServer::stop() {
    if (const Result<void> same = check_process(); !same) {
        return same;
    }
    const std::lock_guard lock{server_mutex_};
    if (server_ == nullptr) {
        return {};  // not started, or already over: nothing to stop
    }
    const Result<void> asked = server_->request_shutdown();
    if (!asked && asked.error().code == ErrorCode::BufferFull) {
        server_->stop();  // the loop cannot even take the request: end it at once
    }
    return {};
}

// ---- events and commands -----------------------------------------------------------------------

std::optional<HostEvent> EmbeddedServer::poll(std::chrono::milliseconds timeout) {
    if (!check_process() || !bridge_) {
        return std::nullopt;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (std::optional<HostEvent> event = bridge_->poll()) {
            return event;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            return std::nullopt;
        }
        pollfd waiting{bridge_->events().event_fd(), POLLIN, 0};
        ::poll(&waiting, 1, static_cast<int>(std::min<std::int64_t>(left.count(), 1000)));
    }
}

Result<void> EmbeddedServer::send_text(ConnectionId id, std::string_view text) {
    if (const Result<void> same = check_process(); !same) {
        return same;
    }
    return bridge_->send_text(id, text);
}

Result<void> EmbeddedServer::send_binary(ConnectionId id, ByteView data) {
    if (const Result<void> same = check_process(); !same) {
        return same;
    }
    return bridge_->send_binary(id, data);
}

Result<void> EmbeddedServer::close(ConnectionId id, CloseCode code, std::string_view reason) {
    if (const Result<void> same = check_process(); !same) {
        return same;
    }
    return bridge_->close(id, code, reason);
}

Result<void> EmbeddedServer::subscribe(ConnectionId id, std::string_view topic) {
    if (const Result<void> same = check_process(); !same) {
        return same;
    }
    return bridge_->subscribe(id, topic);
}

Result<void> EmbeddedServer::unsubscribe(ConnectionId id, std::string_view topic) {
    if (const Result<void> same = check_process(); !same) {
        return same;
    }
    return bridge_->unsubscribe(id, topic);
}

Result<PublishReport>
EmbeddedServer::publish_text(std::string_view topic, std::string_view text, ConnectionId except) {
    if (const Result<void> same = check_process(); !same) {
        return std::unexpected{same.error()};
    }
    return bridge_->publish_text(topic, text, except);
}

Result<PublishReport>
EmbeddedServer::publish_binary(std::string_view topic, ByteView data, ConnectionId except) {
    if (const Result<void> same = check_process(); !same) {
        return std::unexpected{same.error()};
    }
    return bridge_->publish_binary(topic, data, except);
}

Result<std::size_t> EmbeddedServer::topic_subscribers(std::string_view topic) const {
    if (const Result<void> same = check_process(); !same) {
        return std::unexpected{same.error()};
    }
    return bridge_->topic_subscribers(topic);
}

void EmbeddedServer::drop_subscriptions(ConnectionId id) noexcept {
    if (check_process()) {
        bridge_->drop_subscriptions(id);
    }
}

Result<void> EmbeddedServer::accept_handshake(ConnectionId id,
                                              std::optional<std::string_view> subprotocol) {
    if (const Result<void> same = check_process(); !same) {
        return same;
    }
    return bridge_->accept_handshake(id, subprotocol);
}

Result<void> EmbeddedServer::reject_handshake(ConnectionId id, int status) {
    if (const Result<void> same = check_process(); !same) {
        return same;
    }
    return bridge_->reject_handshake(id, status);
}

void EmbeddedServer::set_handshake_intake(bool open) {
    if (check_process()) {
        bridge_->set_handshake_intake(open);
    }
}

}  // namespace wsrv

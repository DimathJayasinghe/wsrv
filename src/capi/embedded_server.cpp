// SPDX-License-Identifier: Apache-2.0
#include "capi/embedded_server.hpp"

#include <algorithm>
#include <new>
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
/// so start() and reload_tls() can hand it to the host: the library never prints on its own, and
/// an operator needs more than "the certificate could not be read". The first line is the specific
/// one; the context follows it with a generic summary ("reload failed, keeping ...").
/// Thread-safe: reload runs on any thread.
class DetailLogger final : public ILogger {
public:
    void log(LogLevel level, std::string_view message) override {
        if (level < LogLevel::Error) {
            return;
        }
        constexpr std::size_t kMaxDetail = 512;  // a path plus OpenSSL's reason fits easily
        const std::lock_guard lock{mutex_};
        if (last_.empty()) {
            last_.assign(message.substr(0, kMaxDetail));
        }
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
    mutable std::mutex mutex_;
    std::string last_;
};

}  // namespace

struct EmbeddedServer::TlsState {
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
        auto bridge = HostBridge::make(server->sys_, config.queue());
        if (!bridge) {
            return std::unexpected{bridge.error()};
        }
        server->bridge_ = std::move(*bridge);
        return server;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "embedded server: out of memory");
    }
}

EmbeddedServer::EmbeddedServer(Key /*unused*/, EmbeddedConfig config)
    : owner_pid_{::getpid()}, config_{std::move(config)} {}

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
    if (::getpid() != owner_pid_) {
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
        auto state = std::make_unique<TlsState>();
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
        builder.handler_factory(*bridge_);
        if (config_.handshake_hook()) {
            builder.handshake_decider(*bridge_);
        }
#if defined(WSRV_HAS_TLS)
        if (tls_) {
            builder.transport_wrapper(*tls_->wrapper);
        }
#endif
        server = builder.build();
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
    ready.set_value(Started{server->local_endpoint().port, {}});
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

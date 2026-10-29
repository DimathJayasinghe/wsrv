// SPDX-License-Identifier: Apache-2.0
#include "server/server.hpp"

#include "base/assert.hpp"
#include "net/plain_transport.hpp"

#include <array>
#include <csignal>
#include <exception>
#include <stdexcept>
#include <utility>

namespace wsrv {

namespace {

/// One read buffer shared by all connections of the loop (see ConnectionContext::scratch).
constexpr std::size_t kScratchSize = std::size_t{16} * 1024;

/// Slack on top of max_connections for the server's own registrations (listener, signals).
constexpr std::size_t kLoopSlack = 16;

/// A connection holds at most a handshake or tick timer plus a write-stall timer.
constexpr std::size_t kTimersPerConnection = 3;

[[nodiscard]] EventLoopConfig loop_config_for(std::size_t max_connections) {
    EventLoopConfig loop;
    loop.max_watches = max_connections + kLoopSlack;
    loop.max_deferred = max_connections + kLoopSlack;  // one retire per connection
    loop.max_timers = kTimersPerConnection * max_connections + kLoopSlack;
    return loop;
}

}  // namespace

Server::Server(Key /*unused*/, ServerConfig config, const ServerDeps& deps)
    : config_{std::move(config)}, factory_{*deps.factory},
      sys_{deps.sys != nullptr ? *deps.sys : static_cast<ISyscalls&>(own_sys_)},
      clock_{deps.clock != nullptr ? *deps.clock : static_cast<const IClock&>(own_clock_)},
      logger_{deps.logger != nullptr ? *deps.logger : static_cast<ILogger&>(own_logger_)} {}

Server::~Server() {
    // Cut handles off before the loop and the table go: a foreign thread still holding one then
    // gets Closing instead of touching freed memory (ADR-0014 section 4).
    if (link_) {
        link_->close();
    }
}

Result<std::unique_ptr<Server>> Server::create(ServerConfig config, const ServerDeps& deps) {
    if (deps.factory == nullptr) {
        return fail(ErrorCode::InvalidArgument, "server: a handler factory is required");
    }
    if (const Result<void> valid = validate(config); !valid) {
        return std::unexpected{valid.error()};
    }
    std::unique_ptr<Server> server;
    try {
        server = std::make_unique<Server>(Key{}, std::move(config), deps);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "server: out of memory");
    }
    if (const Result<void> ready = server->init(); !ready) {
        return std::unexpected{ready.error()};
    }
    return server;
}

/// The fallible half of construction, in dependency order: loop, listener (bind errors surface
/// here, EC-N20), table, host, negotiator, connection context, acceptor.
Result<void> Server::init() {
    auto loop = EventLoop::create(sys_, clock_, loop_config_for(config_.max_connections));
    if (!loop) {
        return std::unexpected{loop.error()};
    }
    loop_ = std::move(*loop);

    auto listener = TcpListener::open(sys_, ListenerConfig{config_.listen});
    if (!listener) {
        return std::unexpected{listener.error()};
    }
    listener_ = std::make_unique<TcpListener>(std::move(*listener));

    table_ = std::make_unique<ConnectionTable>(*loop_, [this] { on_connection_removed(); });
    host_ = std::make_unique<LoopHost>(*loop_, clock_, logger_, *table_);
    try {
        negotiator_ = std::make_unique<HandshakeNegotiator>(config_.handshake);
    } catch (const std::invalid_argument&) {
        return fail(ErrorCode::InvalidConfig, "server: invalid handshake configuration");
    }
    scratch_.resize(kScratchSize);
    link_ = std::make_shared<LoopLink>(*loop_, *table_);
    context_ = std::make_unique<ConnectionContext>(ConnectionContext{
        *host_, config_, *negotiator_, factory_, MutableByteView{scratch_}, link_});

    acceptor_ = std::make_unique<Acceptor>(
        *loop_,
        *listener_,
        sys_,
        [this](Fd client, Endpoint peer) { on_accept(std::move(client), peer); },
        AcceptorConfig{});
    return acceptor_->start();
}

Result<void> Server::run() {
    return loop_->run();
}

Result<void> Server::install_signal_handlers() {
    const std::array<int, 2> signals{SIGINT, SIGTERM};
    auto watcher =
        SignalWatcher::create(*loop_, sys_, signals, [this](int signo) { on_signal(signo); });
    if (!watcher) {
        return std::unexpected{watcher.error()};
    }
    signals_ = std::move(*watcher);
    return {};
}

Result<void> Server::request_shutdown() {
    return loop_->post([this] { begin_shutdown(); });
}

void Server::stop() noexcept {
    loop_->stop();
}

Endpoint Server::local_endpoint() const {
    return listener_->local_endpoint();
}

// ---- accepting ----------------------------------------------------------------------------------

/// New descriptor from the acceptor. Over the cap the client is closed at once (the Fd destructor)
/// without reading a byte: refusing costs one close (FR-S1, T4). Runs inside the acceptor's
/// callback, which must not throw, so allocation failures end here.
void Server::on_accept(Fd client, const Endpoint& peer) {
    if (table_->size() >= config_.max_connections) {
        const std::uint64_t total = rejected_.fetch_add(1) + 1;
        if ((total & (total - 1)) == 0) {  // log the 1st, 2nd, 4th, 8th...: rate-limited
            logger_.log(LogLevel::Warn, "server: connection limit reached, refusing clients");
        }
        return;
    }
    try {
        auto transport = std::make_unique<PlainTransport>(std::move(client), sys_);
        Connection& connection = table_->insert(
            std::make_unique<Connection>(++last_id_, peer, std::move(transport), *context_));
        static_cast<void>(connection.start().has_value());  // on failure it is already retired
    } catch (const std::exception&) {
        logger_.log(LogLevel::Error, "server: out of memory accepting a connection");
    }
}

// ---- shutdown -----------------------------------------------------------------------------------

void Server::on_signal(int /*signo*/) {
    logger_.log(LogLevel::Info, "server: shutdown signal received");
    begin_shutdown();  // the second signal lands in force_shutdown()
}

/// EC-C18: stop accepting, send 1001 to everyone, give them `shutdown_grace`, then drop the rest.
void Server::begin_shutdown() {
    if (shutting_down_) {
        force_shutdown();
        return;
    }
    shutting_down_ = true;
    acceptor_->stop();
    table_->for_each([](Connection& connection) { connection.begin_shutdown(); });
    if (table_->empty()) {
        loop_->stop();
        return;
    }
    const Result<TimerHandle> grace =
        loop_->run_after(config_.shutdown_grace, [this] { force_shutdown(); });
    if (grace) {
        grace_timer_ = *grace;
    } else {
        force_shutdown();  // no timer, no way to wait: do not hang
    }
}

void Server::force_shutdown() {
    table_->for_each([](Connection& connection) { connection.abort(); });
    loop_->stop();
}

void Server::on_connection_removed() {
    if (shutting_down_ && table_->empty()) {
        if (grace_timer_) {
            static_cast<void>(loop_->cancel(*grace_timer_));
            grace_timer_.reset();
        }
        loop_->stop();
    }
}

}  // namespace wsrv

// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/error.hpp"
#include "net/acceptor.hpp"
#include "net/endpoint.hpp"
#include "net/event_loop.hpp"
#include "net/syscalls.hpp"
#include "net/tcp_listener.hpp"
#include "server/admin_http.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <vector>

namespace wsrv {

struct AdminConfig {
    Endpoint listen;
    std::size_t max_connections = 8;                  ///< served at once; the rest are closed
    std::chrono::milliseconds request_timeout{5000};  ///< a request must be complete by then
};

/// The admin listener (ADR-0023 section 3): `/metrics`, `/healthz`, `/readyz` over plain HTTP, on
/// the loop that owns it (loop 0), so it needs no thread and no lock. Everything about it is
/// bounded: at most `max_connections` clients (more are accepted and closed at once), a request
/// head of at most kMaxAdminHeadBytes, a deadline per client, one non-blocking send per answer
/// (a client that cannot take it in one call is dropped, never buffered for), and one request per
/// connection. It never reads a request body and never keeps a connection alive.
///
/// Thread-affinity: loop-thread-only (the thread that runs `loop`), including the destructor.
class AdminEndpoint {
public:
    /// @return Error from the listener (AddressInUse, permission...) or the acceptor.
    [[nodiscard]] static Result<std::unique_ptr<AdminEndpoint>>
    create(EventLoop& loop, ISyscalls& sys, const AdminConfig& config, AdminSources sources);
    AdminEndpoint(const AdminEndpoint&) = delete;
    AdminEndpoint& operator=(const AdminEndpoint&) = delete;
    AdminEndpoint(AdminEndpoint&&) = delete;
    AdminEndpoint& operator=(AdminEndpoint&&) = delete;
    ~AdminEndpoint();

    /// The address actually bound (the real port when 0 was configured).
    [[nodiscard]] Endpoint local_endpoint() const {
        return listener_->local_endpoint();
    }

    /// Passkey for std::make_unique; use create().
    class Key {
        friend class AdminEndpoint;
        Key() = default;
    };
    AdminEndpoint(Key /*passkey*/,
                  EventLoop& loop,
                  ISyscalls& sys,
                  const AdminConfig& config,
                  AdminSources sources);

private:
    class Client;
    [[nodiscard]] Result<void> open();
    void on_accept(Fd client);
    void sweep();
    void retire(Client& client);

    EventLoop& loop_;
    ISyscalls& sys_;
    const AdminConfig config_;
    const AdminSources sources_;
    std::unique_ptr<TcpListener> listener_;
    std::unique_ptr<Acceptor> acceptor_;
    std::vector<std::unique_ptr<Client>> clients_;  // at most max_connections live + not yet swept
};

}  // namespace wsrv

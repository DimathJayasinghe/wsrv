// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/error.hpp"
#include "base/metrics.hpp"
#include "net/acceptor.hpp"
#include "net/endpoint.hpp"
#include "net/event_loop.hpp"
#include "net/syscalls.hpp"
#include "net/tcp_listener.hpp"
#include "server/publish_http.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

namespace wsrv {

struct PublishConfig {
    Endpoint listen;
    std::size_t max_connections = 16;                 ///< served at once; the rest are closed
    std::chrono::milliseconds request_timeout{5000};  ///< head AND body must be complete by then
    std::size_t max_body_bytes = std::size_t{64} * 1024;  ///< a longer declared body is 413
};

/// What the endpoint works with, all borrowed and valid for its whole life (kept in one struct so
/// that create() stays within the parameter limit of the coding standard).
struct PublishDeps {
    EventLoop& loop;
    ISyscalls& sys;
    const PublishSources& sources;    ///< the token and the action
    IMetrics& metrics;                ///< where the answers are counted
    std::function<bool()> accepting;  ///< false while the server shuts down: the answer is 503
};

/// The publish listener (ADR-0028 section 3): `POST /publish` with a bearer token, on the loop
/// that owns it (loop 0), so it needs no thread and no lock. The order of checks is the security
/// property (amendment 2, item 4): the head is parsed and interpreted, then the token is compared
/// in constant time, then the declared length is checked against `max_body_bytes`, and only then
/// is a body buffer reserved; an unauthenticated peer can never make the daemon reserve memory.
/// Everything is bounded: at most `max_connections` clients (more are accepted and closed), a head
/// of at most kMaxPublishHeadBytes, one deadline for the whole request, one non-blocking send per
/// answer, one request per connection (bytes after the declared body are never read).
///
/// Thread-affinity: loop-thread-only (the thread that runs `loop`), including the destructor.
class PublishEndpoint {
public:
    /// @return Error{InvalidConfig} (token shorter than kMinPublishTokenBytes, no publish action),
    /// or an error from the listener (AddressInUse...) or the acceptor.
    [[nodiscard]] static Result<std::unique_ptr<PublishEndpoint>>
    create(PublishDeps deps, const PublishConfig& config);
    PublishEndpoint(const PublishEndpoint&) = delete;
    PublishEndpoint& operator=(const PublishEndpoint&) = delete;
    PublishEndpoint(PublishEndpoint&&) = delete;
    PublishEndpoint& operator=(PublishEndpoint&&) = delete;
    ~PublishEndpoint();

    /// The address actually bound (the real port when 0 was configured).
    [[nodiscard]] Endpoint local_endpoint() const {
        return listener_->local_endpoint();
    }

    /// Passkey for std::make_unique; use create().
    class Key {
        friend class PublishEndpoint;
        Key() = default;
    };
    PublishEndpoint(Key /*passkey*/, PublishDeps deps, const PublishConfig& config);

private:
    class Client;
    [[nodiscard]] Result<void> open();
    void on_accept(Fd client);
    void sweep();
    void retire(Client& client);

    EventLoop& loop_;
    ISyscalls& sys_;
    const PublishConfig config_;
    const PublishSources& sources_;
    IMetrics& metrics_;
    const std::function<bool()> accepting_;
    std::unique_ptr<TcpListener> listener_;
    std::unique_ptr<Acceptor> acceptor_;
    std::vector<std::unique_ptr<Client>> clients_;  // at most max_connections live + not yet swept
};

}  // namespace wsrv

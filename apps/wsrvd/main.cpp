// SPDX-License-Identifier: Apache-2.0
// wsrvd: the WebSocket sidecar daemon (ADR-0017). This file is the composition root: it is the one
// place that creates the concrete loop, signal handling and IPC server and wires them together.
#include "base/clock.hpp"
#include "net/event_loop.hpp"
#include "net/signal_watcher.hpp"
#include "net/syscalls.hpp"

#include <array>
#include <csignal>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "capi/embedded_server.hpp"
#include "wsrvd/embedded_backend.hpp"
#include "wsrvd/ipc_server.hpp"
#include "wsrvd/startup.hpp"

namespace {

int fail_with(std::string_view what, const wsrv::Error& error) {
    std::cerr << "wsrvd: " << what << ": " << error.context << '\n';
    return 1;
}

/// Open everything and serve until SIGINT/SIGTERM. Returns the process exit code.
int serve(const wsrv::wsrvd::Startup& startup, wsrv::ISyscalls& sys) {
    const wsrv::SteadyClock clock;
    auto loop = wsrv::EventLoop::create(sys, clock);
    if (!loop) {
        return fail_with("cannot create the event loop", loop.error());
    }
    // Signals are blocked and read from a descriptor on THIS thread, before any other thread
    // exists: threads inherit the signal mask, so the order matters (net/signal_watcher.hpp).
    const std::array<int, 2> signals{SIGINT, SIGTERM};
    auto watcher = wsrv::SignalWatcher::create(**loop, sys, signals, [&loop](int /*signo*/) {
        (*loop)->stop();  // graceful: the loop returns, destructors run in reverse order
    });
    if (!watcher) {
        return fail_with("cannot watch signals", watcher.error());
    }

    // The WebSocket side. Created (not started) first: the IPC server needs its event descriptor.
    auto embedded = wsrv::EmbeddedServer::create(startup.config.embedded());
    if (!embedded) {
        return fail_with("cannot create the WebSocket server", embedded.error());
    }
    wsrv::wsrvd::EmbeddedBackend backend{**embedded};

    wsrv::wsrvd::IpcServerConfig ipc;
    ipc.socket_path = startup.config.socket_path();
    ipc.token = startup.token;
    ipc.max_frame = startup.config.max_frame();
    ipc.hello_timeout = startup.config.hello_timeout();
    ipc.stall_timeout = startup.config.stall_timeout();
    ipc.restart_grace = startup.config.embedded().server().shutdown_grace;
    auto server = wsrv::wsrvd::IpcServer::create(**loop, sys, backend, std::move(ipc));
    if (!server) {
        return fail_with("cannot open the IPC socket", server.error());
    }
    std::cerr << "wsrvd: ipc socket " << startup.config.socket_path() << " ready\n";

    // Only now spawn the WebSocket threads: the signal mask is already set (see above), the IPC
    // socket is listening, and an application can connect before the first browser does.
    if (const wsrv::Result<void> started = (*embedded)->start(); !started) {
        std::cerr << "wsrvd: cannot start the WebSocket server: " << (*embedded)->start_error()
                  << '\n';
        server->reset();
        return 1;
    }
    std::cerr << "wsrvd: websocket server listening on port " << (*embedded)->port() << '\n';

    const wsrv::Result<void> ran = (*loop)->run();
    // Shutdown order matters: the IPC server uses the backend's descriptor and the loop, so it
    // goes first (and removes the socket file); then the WebSocket server drains and joins.
    server->reset();
    embedded->reset();
    return ran ? 0 : fail_with("event loop failed", ran.error());
}

int run(const std::vector<std::string_view>& args) {
    wsrv::LinuxSyscalls sys;
    const auto startup = wsrv::wsrvd::prepare(args, sys);
    if (!startup) {
        const int code = fail_with("cannot start", startup.error());
        std::cerr << '\n' << wsrv::wsrvd::usage();
        return code;
    }
    if (startup->help) {
        std::cout << wsrv::wsrvd::usage();
        return 0;
    }
    if (startup->token.empty()) {
        std::cerr << "wsrvd: warning: no ipc_token_file; only the peer uid check protects the "
                     "socket\n";
    }
    return serve(*startup, sys);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::vector<std::string_view> args{argv + 1, argv + argc};
        return run(args);
    } catch (const std::exception& error) {
        // Only startup can reach here (allocation while parsing); report it, never swallow it.
        std::cerr << "wsrvd: fatal: " << error.what() << '\n';
        return 1;
    }
}

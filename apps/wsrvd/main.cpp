// SPDX-License-Identifier: Apache-2.0
// wsrvd: the WebSocket sidecar daemon (ADR-0017). This file is the composition root: it is the one
// place that creates the concrete loop, signal handling and IPC server and wires them together.
#include "base/clock.hpp"
#include "net/event_loop.hpp"
#include "net/signal_watcher.hpp"
#include "net/syscalls.hpp"

#include <array>
#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "capi/embedded_server.hpp"
#include "wsrvd/embedded_backend.hpp"
#include "wsrvd/healthcheck.hpp"
#include "wsrvd/ipc_server.hpp"
#include "wsrvd/reload.hpp"
#include "wsrvd/startup.hpp"

namespace {

int fail_with(std::string_view what, const wsrv::Error& error) {
    std::cerr << "wsrvd: " << what << ": " << error.context << '\n';
    return 1;
}

wsrv::wsrvd::IpcServerConfig ipc_config(const wsrv::wsrvd::Startup& startup) {
    wsrv::wsrvd::IpcServerConfig ipc;
    ipc.socket_path = startup.config.socket_path();
    ipc.token = startup.token;
    ipc.max_frame = startup.config.max_frame();
    ipc.hello_timeout = startup.config.hello_timeout();
    ipc.stall_timeout = startup.config.stall_timeout();
    ipc.restart_grace = startup.config.embedded().server().shutdown_grace;
    ipc.handshake_hook = startup.config.embedded().handshake_hook();
    return ipc;
}

/// Create the WebSocket server and the IPC server on `loop`, start the WebSocket threads, run the
/// loop until it is stopped, and shut everything down in the right order.
int run_daemon(wsrv::EventLoop& loop,
               wsrv::ISyscalls& sys,
               const wsrv::wsrvd::Startup& startup,
               wsrv::EmbeddedServer*& reloadable) {
    // The WebSocket side. Created (not started) first: the IPC server needs its event descriptor.
    auto embedded = wsrv::EmbeddedServer::create(startup.config.embedded());
    if (!embedded) {
        return fail_with("cannot create the WebSocket server", embedded.error());
    }
    wsrv::wsrvd::EmbeddedBackend backend{**embedded};
    reloadable = embedded->get();  // SIGHUP may reload TLS from now on (see serve())

    auto server = wsrv::wsrvd::IpcServer::create(loop, sys, backend, ipc_config(startup));
    if (!server) {
        return fail_with("cannot open the IPC socket", server.error());
    }
    std::cerr << "wsrvd: ipc socket " << startup.config.socket_path() << " ready\n";

    // Only now spawn the WebSocket threads: the signal mask is already set (see serve()), the IPC
    // socket is listening, and an application can connect before the first browser does.
    if (const wsrv::Result<void> started = (*embedded)->start(); !started) {
        std::cerr << "wsrvd: cannot start the WebSocket server: " << (*embedded)->start_error()
                  << '\n';
        server->reset();
        return 1;
    }
    std::cerr << "wsrvd: websocket server listening on port " << (*embedded)->port() << '\n';
    if ((*embedded)->publish_port() != 0) {
        std::cerr << "wsrvd: publish endpoint listening on port " << (*embedded)->publish_port() << '\n';
    }

    const wsrv::Result<void> ran = loop.run();
    reloadable = nullptr;  // the signal handler must not touch a server that is being closed
    // Shutdown order matters: the IPC server uses the backend's descriptor and the loop, so it
    // goes first (and removes the socket file); then the WebSocket server drains and joins.
    server->reset();
    embedded->reset();
    return ran ? 0 : fail_with("event loop failed", ran.error());
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
    // SIGHUP re-reads the TLS certificate (ADR-0019 section 6). The handler runs on this thread, in
    // the loop, so `reloadable` needs no lock: it is set and cleared on the same thread.
    wsrv::EmbeddedServer* reloadable = nullptr;
    const std::array<int, 3> signals{SIGINT, SIGTERM, SIGHUP};
    auto watcher =
        wsrv::SignalWatcher::create(**loop, sys, signals, [&loop, &reloadable](int signo) {
            if (signo == SIGHUP) {
                if (reloadable != nullptr) {
                    std::cerr << wsrv::wsrvd::reload_tls_on_hangup(*reloadable).line << '\n';
                }
                return;
            }
            (*loop)->stop();  // graceful: the loop returns, destructors run in reverse order
        });
    if (!watcher) {
        return fail_with("cannot watch signals", watcher.error());
    }
    return run_daemon(**loop, sys, startup, reloadable);
}

/// --healthcheck (ADR-0030): one line, exit 0 or 1.
int health(const wsrv::wsrvd::Startup& startup) {
    constexpr std::chrono::milliseconds kDeadline{2000};
    const wsrv::Result<void> result =
        wsrv::wsrvd::check_health(wsrv::wsrvd::health_target(startup.config), kDeadline);
    if (!result) {
        std::cerr << "wsrvd: unhealthy: " << result.error().context << '\n';
        return 1;
    }
    std::cout << "wsrvd: healthy\n";
    return 0;
}

int run(const std::vector<std::string_view>& args, const std::vector<std::string_view>& environment) {
    wsrv::LinuxSyscalls sys;
    std::string detail;
    const auto startup = wsrv::wsrvd::prepare(args, environment, sys, &detail);
    if (!startup) {
        const int code = fail_with("cannot start", startup.error());
        if (!detail.empty()) {
            std::cerr << "wsrvd: the setting came from the environment variable " << detail << '\n';
        }
        std::cerr << '\n' << wsrv::wsrvd::usage();
        return code;
    }
    if (startup->help) {
        std::cout << wsrv::wsrvd::usage();
        return 0;
    }
    if (startup->healthcheck) {
        return health(*startup);
    }
    if (startup->token.empty()) {
        std::cerr << "wsrvd: warning: no ipc_token_file; only the peer uid check protects the "
                     "socket\n";
    }
    return serve(*startup, sys);
}

}  // namespace

int main(int argc, char** argv, char** envp) {
    try {
        const std::vector<std::string_view> args{argv + 1, argv + argc};
        std::vector<std::string_view> environment;
        for (char** entry = envp; entry != nullptr && *entry != nullptr; ++entry) {
            environment.emplace_back(*entry);
        }
        return run(args, environment);
    } catch (const std::exception& error) {
        // Only startup can reach here (allocation while parsing); report it, never swallow it.
        std::cerr << "wsrvd: fatal: " << error.what() << '\n';
        return 1;
    }
}

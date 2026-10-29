// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "server/server_builder.hpp"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <thread>

#include <pthread.h>

namespace wsrv::test {

/// A real Server on a real loopback port, run on its own thread (the loop belongs to the thread
/// that creates it). The destructor asks it to stop and joins.
class RunningServer {
public:
    RunningServer(ServerBuilder builder, bool with_signals = false) {
        std::promise<std::uint16_t> ready;
        std::future<std::uint16_t> port = ready.get_future();
        thread_ = std::thread([this, builder = std::move(builder), with_signals, &ready] {
            try {
                server_ = builder.build();
                if (with_signals) {
                    const Result<void> installed = server_->install_signal_handlers();
                    if (!installed) {
                        throw ConfigError{"signals"};
                    }
                }
                ready.set_value(server_->local_endpoint().port);
            } catch (...) {
                ready.set_exception(std::current_exception());
                return;
            }
            static_cast<void>(server_->run().has_value());
            finished_.store(true);
            release_.get_future().wait();  // destroyed on this thread, after the test stopped it
            server_.reset();
        });
        port_ = port.get();  // rethrows a build failure in the test thread
    }

    RunningServer(const RunningServer&) = delete;
    RunningServer& operator=(const RunningServer&) = delete;

    ~RunningServer() {
        if (thread_.joinable()) {
            server_->stop();
            release_.set_value();
            thread_.join();
        }
    }

    [[nodiscard]] std::uint16_t port() const {
        return port_;
    }
    [[nodiscard]] Server& server() {
        return *server_;
    }
    /// Deliver `signo` to the server's thread only (it blocks the signal, so it is not fatal).
    void signal(int signo) {
        pthread_kill(thread_.native_handle(), signo);
    }
    /// True once run() has returned.
    [[nodiscard]] bool finished() const {
        return finished_.load();
    }

private:
    std::unique_ptr<Server> server_;
    std::thread thread_;
    std::uint16_t port_ = 0;
    std::atomic<bool> finished_{false};
    std::promise<void> release_;
};

}  // namespace wsrv::test

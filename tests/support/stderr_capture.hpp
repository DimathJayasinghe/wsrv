// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <string>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

namespace wsrv::test {

/// Points the process's stderr at a pipe for as long as the object lives, so a test can read what a
/// started server really wrote (ADR-0037 logs to descriptor 2 and nowhere else). The previous
/// stderr is put back by the destructor. One at a time; the pipe holds 64 KiB, far more than a test
/// writes.
class StderrCapture {
public:
    StderrCapture() {
        if (::pipe2(pipe_, O_NONBLOCK | O_CLOEXEC) == 0) {
            saved_ = ::dup(STDERR_FILENO);
            ::dup2(pipe_[1], STDERR_FILENO);
        }
    }
    StderrCapture(const StderrCapture&) = delete;
    StderrCapture& operator=(const StderrCapture&) = delete;
    ~StderrCapture() {
        restore();
        for (const int fd : pipe_) {
            if (fd >= 0) {
                ::close(fd);
            }
        }
    }

    /// Everything written since the last call; waits up to `settle` for lines from other threads.
    std::string take(std::chrono::milliseconds settle = std::chrono::milliseconds{0}) {
        std::this_thread::sleep_for(settle);
        std::string text;
        char buffer[4096];
        for (;;) {
            const ssize_t got = ::read(pipe_[0], buffer, sizeof buffer);
            if (got <= 0) {
                return text;
            }
            text.append(buffer, static_cast<std::size_t>(got));
        }
    }
    /// Collected output until it contains `needle` or `timeout` passes; the text so far either way.
    std::string wait_for(const std::string& needle,
                         std::chrono::milliseconds timeout = std::chrono::seconds{3}) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (collected_.find(needle) == std::string::npos &&
               std::chrono::steady_clock::now() < deadline) {
            collected_ += take();
            if (collected_.find(needle) == std::string::npos) {
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
            }
        }
        return collected_;
    }
    /// Put the real stderr back early (so a failing assertion prints where a person can see it).
    void restore() {
        if (saved_ >= 0) {
            ::dup2(saved_, STDERR_FILENO);
            ::close(saved_);
            saved_ = -1;
        }
    }

private:
    int pipe_[2]{-1, -1};
    int saved_ = -1;
    std::string collected_;
};

}  // namespace wsrv::test

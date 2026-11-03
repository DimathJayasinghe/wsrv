// SPDX-License-Identifier: Apache-2.0
#pragma once

// A stand-in for the WebSocket server behind wsrvd. It sits on the REAL EventQueue (real bounds,
// real event_fd semantics, real pending() count), so a backpressure test can prove that events
// stayed queued rather than being dropped. Commands are recorded under a mutex: the loop thread
// writes them and the test thread reads them.
#include "capi/event_queue.hpp"
#include "net/syscalls.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wsrvd/backend.hpp"

namespace wsrv::testing {

struct RecordedCommand {
    enum class Kind : std::uint8_t { Text, Binary, Close };
    Kind kind = Kind::Text;
    ConnectionId conn = 0;
    std::string data;  // payload, or the close reason
    std::uint16_t code = 0;
};

class FakeBackend final : public wsrvd::IBackend {
public:
    explicit FakeBackend(EventQueueConfig config = {}) {
        auto queue = EventQueue::create(sys_, config);
        EXPECT_TRUE(queue.has_value());
        queue_ = std::move(*queue);
    }

    // ---- what the test does to the "server" ----
    EventQueue& queue() {
        return *queue_;
    }
    Result<PushOutcome> push(HostEventType type,
                             ConnectionId conn,
                             std::string_view data = {},
                             std::uint16_t close_code = 0,
                             bool clean = false) {
        HostEvent event;
        event.type = type;
        event.conn = conn;
        for (const char c : data) {
            event.data.push_back(static_cast<std::byte>(c));
        }
        event.close_code = close_code;
        event.clean = clean;
        return queue_->push(std::move(event));
    }
    /// Make every following command fail with this error; nullopt = succeed again.
    void fail_commands_with(std::optional<ErrorCode> code) {
        const std::lock_guard lock{mutex_};
        command_result_ = code;
    }
    [[nodiscard]] std::vector<RecordedCommand> commands() const {
        const std::lock_guard lock{mutex_};
        return commands_;
    }
    [[nodiscard]] std::size_t polls() const {
        const std::lock_guard lock{mutex_};
        return polls_;
    }

    // ---- IBackend ----
    std::optional<HostEvent> poll() override {
        {
            const std::lock_guard lock{mutex_};
            ++polls_;
        }
        std::optional<Popped> popped = queue_->try_pop();
        if (!popped) {
            return std::nullopt;
        }
        return std::move(popped->event);
    }
    int event_fd() const noexcept override {
        return queue_->event_fd();
    }
    Result<void> send_text(ConnectionId conn, ByteView text) override {
        return record(RecordedCommand::Kind::Text, conn, as_chars(text), 0);
    }
    Result<void> send_binary(ConnectionId conn, ByteView data) override {
        return record(RecordedCommand::Kind::Binary, conn, as_chars(data), 0);
    }
    Result<void> close(ConnectionId conn, std::uint16_t code, std::string_view reason) override {
        return record(RecordedCommand::Kind::Close, conn, reason, code);
    }

private:
    Result<void> record(RecordedCommand::Kind kind,
                        ConnectionId conn,
                        std::string_view data,
                        std::uint16_t code) {
        const std::lock_guard lock{mutex_};
        commands_.push_back(RecordedCommand{kind, conn, std::string{data}, code});
        if (command_result_) {
            return fail(*command_result_, "fake backend: scripted failure");
        }
        return {};
    }

    LinuxSyscalls sys_;
    std::unique_ptr<EventQueue> queue_;
    mutable std::mutex mutex_;
    std::vector<RecordedCommand> commands_;
    std::optional<ErrorCode> command_result_;
    std::size_t polls_ = 0;
};

}  // namespace wsrv::testing

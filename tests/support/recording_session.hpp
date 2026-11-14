// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "server/handler.hpp"
#include "server/session_handle.hpp"

#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace wsrv::test {

/// A Session that records what it was asked to do, and on which thread.
class RecordingSession final : public Session {
public:
    explicit RecordingSession(ConnectionId id) : id_{id} {}
    [[nodiscard]] ConnectionId id() const noexcept override {
        return id_;
    }
    [[nodiscard]] const Endpoint& peer() const noexcept override {
        return peer_;
    }
    [[nodiscard]] Result<void> send_text(std::string_view text) override {
        texts.emplace_back(text);
        threads.push_back(std::this_thread::get_id());
        return {};
    }
    [[nodiscard]] Result<void> send_binary(ByteView data) override {
        binary_sizes.push_back(data.size());
        std::string copy;
        for (const std::byte b : data) {
            copy.push_back(static_cast<char>(b));
        }
        binaries.push_back(std::move(copy));
        return {};
    }
    [[nodiscard]] Result<MessageWriter> begin_text() override {
        return fail(ErrorCode::Closing, "stub");
    }
    [[nodiscard]] Result<MessageWriter> begin_binary() override {
        return fail(ErrorCode::Closing, "stub");
    }
    void pause_reads() override {}
    void resume_reads() override {
        ++resumes;
    }
    void close(CloseCode code, std::string_view reason) override {
        closes.emplace_back(code, std::string{reason});
    }
    [[nodiscard]] SessionHandle handle() const override {
        return {};  // not needed here
    }

    std::vector<std::string> texts;
    int resumes = 0;
    std::vector<std::size_t> binary_sizes;
    std::vector<std::string> binaries;
    std::vector<std::pair<CloseCode, std::string>> closes;
    std::vector<std::thread::id> threads;

private:
    ConnectionId id_;
    Endpoint peer_;
};

class FakeDirectory final : public ISessionDirectory {
public:
    void run(ConnectionId id, const SessionCommand& command) override {
        const auto found = sessions.find(id);
        if (found != sessions.end()) {
            command(*found->second);
        }
    }
    void decide(ConnectionId /*id*/, const HandshakeVerdict& /*verdict*/) override {}
    std::map<ConnectionId, Session*> sessions;
};

}  // namespace wsrv::test

// SPDX-License-Identifier: Apache-2.0
// A chat with rooms on the pub/sub broker (ADR-0024). It is a consumer of the public API only:
// a handler per connection, a Broker shared by all of them, and a BrokerMembership member so a
// connection's rooms end with it. Text protocol, one command per message:
//
//   join <room>            enter a room                      ok joined <room>
//   leave <room>           leave it                          ok left <room>
//   say <room> <text>      tell the others in a room         ok sent <room> <accepted> <refused>
//   members <room>         how many are in a room            ok members <room> <n>
//   rooms                  the rooms this connection is in   ok rooms <a> <b> ...
//
// Others in the room receive `msg <room> user<id>: <text>`. Any failure is `error: <why>`.
// Every bound comes from the broker (names, rooms per connection, payload size): the example adds
// none of its own and trusts none of the input.
#pragma once

#include "server/broker.hpp"
#include "server/handler.hpp"

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace chat {

enum class CommandKind { Join, Leave, Say, Members, Rooms };

struct Command {
    CommandKind kind = CommandKind::Rooms;
    std::string room;
    std::string text;  ///< Say only
};

namespace detail {

/// The part of `line` before the first space, and the rest after it (empty when there is none).
inline std::pair<std::string_view, std::string_view> split_word(std::string_view line) {
    const std::size_t space = line.find(' ');
    if (space == std::string_view::npos) {
        return {line, std::string_view{}};
    }
    return {line.substr(0, space), line.substr(space + 1)};
}

/// A command with one argument: a single non-empty word.
inline std::optional<Command> with_room(CommandKind kind, std::string_view rest) {
    if (rest.empty() || rest.find(' ') != std::string_view::npos) {
        return std::nullopt;
    }
    return Command{kind, std::string{rest}, {}};
}

}  // namespace detail

/// Pure: what a line means, or nullopt. Exact words only (no case folding, no trimming), so there
/// is one spelling of every command. The room name is checked later by the broker.
inline std::optional<Command> parse_command(std::string_view line) {
    const auto [word, rest] = detail::split_word(line);
    if (word == "rooms") {
        return rest.empty() ? std::optional<Command>{Command{CommandKind::Rooms, {}, {}}}
                            : std::nullopt;
    }
    if (word == "join") {
        return detail::with_room(CommandKind::Join, rest);
    }
    if (word == "leave") {
        return detail::with_room(CommandKind::Leave, rest);
    }
    if (word == "members") {
        return detail::with_room(CommandKind::Members, rest);
    }
    if (word == "say") {
        const auto [room, text] = detail::split_word(rest);
        if (room.empty() || text.empty()) {
            return std::nullopt;
        }
        return Command{CommandKind::Say, std::string{room}, std::string{text}};
    }
    return std::nullopt;
}

/// One connection's side of the chat. Loop-thread-only, like every handler.
class ChatHandler final : public wsrv::IWebSocketHandler {
public:
    explicit ChatHandler(wsrv::Broker& broker) : broker_{broker}, membership_{broker} {}

    void on_text(wsrv::Session& session, std::string_view text) override {
        const std::optional<Command> command = parse_command(text);
        reply(session, command ? run(session, *command) : std::string{"error: unknown command"});
    }

private:
    static void reply(wsrv::Session& session, const std::string& answer) {
        // A refused send means the connection is closing or too slow; the server handles both.
        static_cast<void>(session.send_text(answer).has_value());
    }

    std::string run(wsrv::Session& session, const Command& command) {
        switch (command.kind) {
        case CommandKind::Join:
            return join(session, command.room);
        case CommandKind::Leave:
            membership_.leave(command.room);
            rooms_.erase(command.room);
            return "ok left " + command.room;
        case CommandKind::Say:
            return say(session, command);
        case CommandKind::Members:
            return "ok members " + command.room + " " +
                   std::to_string(broker_.subscriber_count(command.room));
        case CommandKind::Rooms:
            break;
        }
        std::string answer = "ok rooms";
        for (const std::string& room : rooms_) {
            answer += ' ' + room;
        }
        return answer;
    }

    std::string join(wsrv::Session& session, const std::string& room) {
        const wsrv::Result<void> joined = membership_.subscribe(session.handle(), room);
        if (!joined) {
            return failure(joined.error().code, "not a valid room name", "a limit was reached");
        }
        rooms_.insert(room);
        return "ok joined " + room;
    }

    std::string say(const wsrv::Session& session, const Command& command) {
        if (!rooms_.contains(command.room)) {
            return "error: join the room first";  // no talking into rooms one is not in
        }
        const std::string line =
            "msg " + command.room + " user" + std::to_string(session.id()) + ": " + command.text;
        const auto report = broker_.publish_text(command.room, line, session.id());
        if (!report) {
            return failure(report.error().code, "message too long", "try again later");
        }
        return "ok sent " + command.room + " " + std::to_string(report->accepted) + " " +
               std::to_string(report->refused);
    }

    /// Wording for the two failures a peer can cause; the broker's code says which bound it was.
    static std::string
    failure(wsrv::ErrorCode code, std::string_view invalid_argument, std::string_view limit) {
        return std::string{"error: "} +
               std::string{code == wsrv::ErrorCode::InvalidArgument ? invalid_argument : limit};
    }

    wsrv::Broker& broker_;
    wsrv::BrokerMembership membership_;  // gives every room back when the connection ends
    std::set<std::string> rooms_;        // bounded by the broker's per-connection limit
};

class ChatFactory final : public wsrv::IHandlerFactory {
public:
    explicit ChatFactory(wsrv::Broker& broker) : broker_{broker} {}
    std::unique_ptr<wsrv::IWebSocketHandler> create(const wsrv::ConnectionInfo& /*info*/) override {
        return std::make_unique<ChatHandler>(broker_);
    }

private:
    wsrv::Broker& broker_;
};

}  // namespace chat

// SPDX-License-Identifier: Apache-2.0
// The chat example (examples/chat): the command grammar as a pure function, then the whole thing on
// a real server with three loops. The example is a consumer of the public API, so these tests are
// also the proof that the Broker is usable the way the documentation says.
#include "chat/chat.hpp"

#include "server/server_builder.hpp"
#include "support/running_server.hpp"
#include "support/tcp_client.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using chat::Command;
using chat::CommandKind;
using chat::parse_command;
using wsrv::test::RunningServer;
using wsrv::test::TcpClient;

// ---- the grammar -------------------------------------------------------------------------------

TEST(ChatCommand, EachCommandIsOneWordThenItsArguments) {
    const auto join = parse_command("join lobby");
    ASSERT_TRUE(join.has_value());
    EXPECT_EQ(join->kind, CommandKind::Join);
    EXPECT_EQ(join->room, "lobby");

    const auto say = parse_command("say lobby hello  there, world");
    ASSERT_TRUE(say.has_value());
    EXPECT_EQ(say->kind, CommandKind::Say);
    EXPECT_EQ(say->room, "lobby");
    EXPECT_EQ(say->text, "hello  there, world") << "the text keeps its own spacing";

    EXPECT_EQ(parse_command("leave lobby")->kind, CommandKind::Leave);
    EXPECT_EQ(parse_command("members lobby")->kind, CommandKind::Members);
    EXPECT_EQ(parse_command("rooms")->kind, CommandKind::Rooms);
}

TEST(ChatCommand, AnythingElseIsNotACommand) {
    for (const char* bad : {"",
                            " ",
                            "join",
                            "join ",
                            "leave",
                            "say lobby",
                            "say lobby ",
                            "say",
                            "JOIN lobby",
                            "dance lobby",
                            "rooms extra",
                            "join a b",
                            "members",
                            " join lobby",
                            "join  lobby"}) {
        EXPECT_FALSE(parse_command(bad).has_value()) << "'" << bad << "'";
    }
}

// ---- on a server -------------------------------------------------------------------------------

class ChatServerTest : public ::testing::Test {
protected:
    ChatServerTest() : factory_{broker_} {
        wsrv::ServerBuilder builder;
        builder.listen("127.0.0.1", 0).handler_factory(factory_);
        builder.config().threads = 3;
        builder.config().handshake.allowed_origins = {"https://app.example.com"};
        builder.config().max_connections_per_ip = 0;  // every client here is 127.0.0.1
        builder.config().max_new_connections_per_ip_per_second = 0;
        running_ = std::make_unique<RunningServer>(std::move(builder));
    }

    std::unique_ptr<TcpClient> connect() {
        auto client = std::make_unique<TcpClient>(running_->port());
        EXPECT_TRUE(client->upgrade());
        return client;
    }

    static std::string say(TcpClient& client, std::string_view line) {
        EXPECT_TRUE(client.send_frame(wsrv::Opcode::Text, line));
        return next(client);
    }
    static std::string next(TcpClient& client) {
        const auto frame = client.read_frame();
        return frame ? frame->payload : std::string{"<none>"};
    }

    template <class Condition>
    static bool eventually(Condition&& condition) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (condition()) {
                return true;
            }
            std::this_thread::sleep_for(5ms);
        }
        return condition();
    }

    wsrv::Broker broker_;
    chat::ChatFactory factory_;
    std::unique_ptr<RunningServer> running_;
};

TEST_F(ChatServerTest, AMessageGoesToTheOtherMembersOfTheRoomOnly) {
    auto alice = connect();
    auto bob = connect();
    auto carol = connect();  // in another room
    EXPECT_EQ(say(*alice, "join lobby"), "ok joined lobby");
    EXPECT_EQ(say(*bob, "join lobby"), "ok joined lobby");
    EXPECT_EQ(say(*carol, "join elsewhere"), "ok joined elsewhere");

    const std::string reply = say(*alice, "say lobby hi all");
    EXPECT_EQ(reply, "ok sent lobby 1 0");  // one member besides the sender, none refused
    const std::string heard = next(*bob);
    EXPECT_EQ(heard.rfind("msg lobby user", 0), 0U) << heard;
    EXPECT_NE(heard.find(": hi all"), std::string::npos) << heard;
    // carol heard nothing: her next frame is the answer to her own next command
    EXPECT_EQ(say(*carol, "rooms"), "ok rooms elsewhere");
}

TEST_F(ChatServerTest, OnlyAMemberMayTalkAndBadInputGetsAnError) {
    auto alice = connect();
    EXPECT_EQ(say(*alice, "say lobby hello"), "error: join the room first");
    EXPECT_EQ(say(*alice, "join bad room"), "error: unknown command");
    EXPECT_EQ(say(*alice, "join bad*room"), "error: not a valid room name");
    EXPECT_EQ(say(*alice, "frobnicate"), "error: unknown command");
    EXPECT_EQ(broker_.topic_count(), 0U) << "nothing was created by those";
}

TEST_F(ChatServerTest, LeavingStopsDeliveryAndMembersCountsTheRoom) {
    auto alice = connect();
    auto bob = connect();
    EXPECT_EQ(say(*alice, "join lobby"), "ok joined lobby");
    EXPECT_EQ(say(*bob, "join lobby"), "ok joined lobby");
    EXPECT_EQ(say(*alice, "members lobby"), "ok members lobby 2");
    EXPECT_EQ(say(*bob, "leave lobby"), "ok left lobby");
    EXPECT_EQ(say(*alice, "members lobby"), "ok members lobby 1");
    EXPECT_EQ(say(*alice, "say lobby anyone?"), "ok sent lobby 0 0");
    EXPECT_EQ(say(*bob, "rooms"), "ok rooms") << "bob heard nothing after leaving";
}

TEST_F(ChatServerTest, TheLimitsOfTheBrokerComeBackAsErrorsNotCrashes) {
    auto alice = connect();
    for (int i = 0; i < 64; ++i) {
        ASSERT_EQ(say(*alice, "join room" + std::to_string(i)),
                  "ok joined room" + std::to_string(i));
    }
    EXPECT_EQ(say(*alice, "join one-too-many"), "error: a limit was reached");
    const std::string big(70 * 1024, 'x');  // over the broker's 64 KiB payload bound
    EXPECT_EQ(say(*alice, "say room0 " + big), "error: message too long");
}

TEST_F(ChatServerTest, EveryoneLeavingLeavesNothingBehind) {
    {
        std::vector<std::unique_ptr<TcpClient>> clients;
        for (int i = 0; i < 6; ++i) {
            clients.push_back(connect());
            EXPECT_EQ(say(*clients.back(), "join lobby"), "ok joined lobby");
            EXPECT_EQ(say(*clients.back(), "join side"), "ok joined side");
        }
    }  // all six disconnect without a word
    EXPECT_TRUE(eventually([&] { return broker_.topic_count() == 0; }));
}

}  // namespace

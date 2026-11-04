// SPDX-License-Identifier: Apache-2.0
// Relay: which events the application is shown, and what happens to its commands. No sockets here;
// the backend is a fake sitting on the real EventQueue.
#include "wsrvd/relay.hpp"

#include "support/fake_backend.hpp"
#include "wsrv/wsrv.h"

#include <gtest/gtest.h>

#include <string_view>

namespace {

using wsrv::ByteView;
using wsrv::ConnectionId;
using wsrv::ErrorCode;
using wsrv::HostEventType;
using wsrv::testing::FakeBackend;
using wsrv::testing::RecordedCommand;
using wsrv::wsrvd::Relay;

ByteView bytes(std::string_view text) {
    return ByteView{reinterpret_cast<const std::byte*>(text.data()), text.size()};  // NOLINT
}

wsrv::ipc::SendData
send(std::uint32_t request, ConnectionId conn, std::string_view text, bool is_text = true) {
    wsrv::ipc::SendData command;
    command.request = request;
    command.conn = conn;
    command.text = is_text;
    command.data = bytes(text);
    return command;
}

wsrv::ipc::CloseCommand close_command(std::uint32_t request,
                                      ConnectionId conn,
                                      std::uint16_t code = 1000,
                                      std::string_view reason = {}) {
    wsrv::ipc::CloseCommand command;
    command.request = request;
    command.conn = conn;
    command.code = code;
    command.reason = bytes(reason);
    return command;
}

constexpr std::uint8_t status(wsrv_status s) {
    return static_cast<std::uint8_t>(s);
}

class RelayTest : public ::testing::Test {
protected:
    /// Open `conn` and hand the Open event to the application.
    void open(ConnectionId conn) {
        ASSERT_TRUE(backend_.push(HostEventType::Open, conn, "127.0.0.1:1").has_value());
        const auto event = relay_.next_event();
        ASSERT_TRUE(event.has_value());
        ASSERT_EQ(event->type, HostEventType::Open);
    }

    FakeBackend backend_;
    Relay relay_{backend_};
};

TEST_F(RelayTest, EventsComeOutInOrderUntilTheQueueIsEmpty) {
    ASSERT_TRUE(backend_.push(HostEventType::Open, 1, "a").has_value());
    ASSERT_TRUE(backend_.push(HostEventType::Text, 1, "hello").has_value());
    const auto first = relay_.next_event();
    const auto second = relay_.next_event();
    ASSERT_TRUE(first && second);
    EXPECT_EQ(first->type, HostEventType::Open);
    EXPECT_EQ(second->type, HostEventType::Text);
    EXPECT_FALSE(relay_.next_event().has_value());
}

TEST_F(RelayTest, ACommandForAnUnknownConnectionIsNoSuchConnAndNeverReachesTheBackend) {
    const auto answer = relay_.execute(send(5, 99, "x"));
    EXPECT_EQ(answer.request, 5U);
    EXPECT_EQ(answer.status, status(WSRV_ERR_NO_SUCH_CONN));
    EXPECT_TRUE(backend_.commands().empty());
}

TEST_F(RelayTest, ACommandForAnOpenConnectionIsForwardedAndAnswered) {
    open(1);
    const auto text = relay_.execute(send(1, 1, "hi"));
    const auto binary = relay_.execute(send(2, 1, "\x01", false));
    EXPECT_EQ(text.status, status(WSRV_OK));
    EXPECT_EQ(binary.status, status(WSRV_OK));
    EXPECT_EQ(text.request, 1U);
    EXPECT_EQ(binary.request, 2U);
    const auto commands = backend_.commands();
    ASSERT_EQ(commands.size(), 2U);
    EXPECT_EQ(commands[0].kind, RecordedCommand::Kind::Text);
    EXPECT_EQ(commands[0].data, "hi");
    EXPECT_EQ(commands[1].kind, RecordedCommand::Kind::Binary);
}

TEST_F(RelayTest, RequestIdZeroIsReservedForTheHandshake) {
    open(1);
    EXPECT_EQ(relay_.execute(send(0, 1, "x")).status, status(WSRV_ERR_INVALID_ARGUMENT));
    EXPECT_EQ(relay_.execute(close_command(0, 1)).status, status(WSRV_ERR_INVALID_ARGUMENT));
    EXPECT_TRUE(backend_.commands().empty()) << "a command with id 0 must not run";
}

TEST_F(RelayTest, ABackendFailureBecomesTheMatchingStatus) {
    open(1);
    backend_.fail_commands_with(ErrorCode::Backpressure);
    EXPECT_EQ(relay_.execute(send(1, 1, "x")).status, status(WSRV_ERR_BACKPRESSURE));
    backend_.fail_commands_with(ErrorCode::InvalidUtf8);
    EXPECT_EQ(relay_.execute(send(2, 1, "x")).status, status(WSRV_ERR_INVALID_UTF8));
    backend_.fail_commands_with(ErrorCode::Closing);
    EXPECT_EQ(relay_.execute(send(3, 1, "x")).status, status(WSRV_ERR_NO_SUCH_CONN));
}

TEST_F(RelayTest, CloseIsForwardedWithCodeAndReason) {
    open(1);
    EXPECT_EQ(relay_.execute(close_command(9, 1, 4001, "bye")).status, status(WSRV_OK));
    const auto commands = backend_.commands();
    ASSERT_EQ(commands.size(), 1U);
    EXPECT_EQ(commands[0].kind, RecordedCommand::Kind::Close);
    EXPECT_EQ(commands[0].code, 4001U);
    EXPECT_EQ(commands[0].data, "bye");
}

TEST_F(RelayTest, ACloseEventEndsTheConnectionForCommands) {
    open(1);
    ASSERT_TRUE(backend_.push(HostEventType::Close, 1, "", 1000, true).has_value());
    ASSERT_TRUE(relay_.next_event().has_value());
    EXPECT_EQ(relay_.execute(send(1, 1, "late")).status, status(WSRV_ERR_NO_SUCH_CONN));
    EXPECT_EQ(relay_.open_count(), 0U);
}

TEST_F(RelayTest, TheOpenSetFollowsTheEventsTheApplicationWasGiven) {
    ASSERT_TRUE(backend_.push(HostEventType::Open, 1).has_value());
    ASSERT_TRUE(backend_.push(HostEventType::Open, 2).has_value());
    EXPECT_EQ(relay_.open_count(), 0U) << "not told yet, so not known yet";
    ASSERT_TRUE(relay_.next_event().has_value());
    EXPECT_EQ(relay_.open_count(), 1U);
    ASSERT_TRUE(relay_.next_event().has_value());
    EXPECT_EQ(relay_.open_count(), 2U);
    EXPECT_EQ(relay_.execute(send(1, 2, "x")).status, status(WSRV_OK));
}

// ---- the application goes away ----------------------------------------------------------------

TEST_F(RelayTest, AfterDetachTheOldConnectionsAreStaleAndCommandsForThemAreRefused) {
    open(1);
    open(2);
    relay_.detach();
    EXPECT_EQ(relay_.open_count(), 0U);
    EXPECT_EQ(relay_.stale_count(), 2U);
    EXPECT_EQ(relay_.execute(send(1, 1, "x")).status, status(WSRV_ERR_NO_SUCH_CONN));
    EXPECT_TRUE(backend_.commands().empty());
}

TEST_F(RelayTest, EventsOfStaleConnectionsAreDroppedAndTheirCloseForgetsThem) {
    open(1);
    relay_.detach();
    ASSERT_TRUE(backend_.push(HostEventType::Text, 1, "for nobody").has_value());
    ASSERT_TRUE(backend_.push(HostEventType::Open, 2, "new").has_value());
    ASSERT_TRUE(backend_.push(HostEventType::Close, 1, "", 1012, true).has_value());
    ASSERT_TRUE(backend_.push(HostEventType::Text, 2, "for the new app").has_value());

    const auto first = relay_.next_event();
    ASSERT_TRUE(first);
    EXPECT_EQ(first->conn, 2U) << "the stale Text must not reach the new application";
    const auto second = relay_.next_event();
    ASSERT_TRUE(second);
    EXPECT_EQ(second->type, HostEventType::Text);
    EXPECT_EQ(second->conn, 2U) << "the stale Close is swallowed too";
    EXPECT_EQ(relay_.stale_count(), 0U) << "its Close ended it";
    EXPECT_EQ(relay_.open_count(), 1U);
}

TEST_F(RelayTest, StaleConnectionsAreClosedWithTheGivenCodeAndNobodyElse) {
    open(1);
    open(2);
    relay_.detach();
    open(3);  // a connection the next application knows about
    relay_.close_stale(1012, "service restart");
    const auto commands = backend_.commands();
    ASSERT_EQ(commands.size(), 2U);
    for (const auto& command : commands) {
        EXPECT_EQ(command.kind, RecordedCommand::Kind::Close);
        EXPECT_EQ(command.code, 1012U);
        EXPECT_EQ(command.data, "service restart");
        EXPECT_NE(command.conn, 3U);
    }
}

// ---- handshake answers (ADR-0018) --------------------------------------------------------------

TEST_F(RelayTest, HandshakeAnswersGoToTheBackendWithoutAnOpenConnection) {
    // The question is about a connection that does not exist yet: no Open has been seen.
    wsrv::ipc::HandshakeAccept accept;
    accept.request = 3;
    accept.conn = 8;
    accept.subprotocol = bytes("chat");
    EXPECT_EQ(relay_.execute(accept).status, status(WSRV_OK));
    wsrv::ipc::HandshakeReject reject;
    reject.request = 4;
    reject.conn = 9;
    reject.status = 403;
    const auto answer = relay_.execute(reject);
    EXPECT_EQ(answer.request, 4U);
    EXPECT_EQ(answer.status, status(WSRV_OK));
    const auto commands = backend_.commands();
    ASSERT_EQ(commands.size(), 2U);
    EXPECT_EQ(commands[0].kind, RecordedCommand::Kind::Accept);
    EXPECT_EQ(commands[0].conn, 8U);
    EXPECT_EQ(commands[0].data, "chat");
    EXPECT_EQ(commands[1].kind, RecordedCommand::Kind::Reject);
    EXPECT_EQ(commands[1].conn, 9U);
    EXPECT_EQ(commands[1].code, 403U);
}

TEST_F(RelayTest, HandshakeAnswersFollowTheSameRulesAsOtherCommands) {
    wsrv::ipc::HandshakeAccept accept;
    accept.conn = 8;
    EXPECT_EQ(relay_.execute(accept).status, status(WSRV_ERR_INVALID_ARGUMENT)) << "request 0";
    wsrv::ipc::HandshakeReject reject;
    reject.conn = 8;
    reject.status = 403;
    EXPECT_EQ(relay_.execute(reject).status, status(WSRV_ERR_INVALID_ARGUMENT)) << "request 0";
    EXPECT_TRUE(backend_.commands().empty()) << "a command with id 0 must not run";

    accept.request = 1;
    reject.request = 2;
    backend_.fail_commands_with(ErrorCode::Closing);  // the question is over (late, or answered)
    EXPECT_EQ(relay_.execute(accept).status, status(WSRV_ERR_NO_SUCH_CONN));
    backend_.fail_commands_with(ErrorCode::InvalidArgument);  // a status or name the core refuses
    EXPECT_EQ(relay_.execute(reject).status, status(WSRV_ERR_INVALID_ARGUMENT));
    backend_.fail_commands_with(ErrorCode::BufferFull);
    EXPECT_EQ(relay_.execute(accept).status, status(WSRV_ERR_BACKPRESSURE));
}

}  // namespace

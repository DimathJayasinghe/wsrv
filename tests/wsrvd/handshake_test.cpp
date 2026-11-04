// SPDX-License-Identifier: Apache-2.0
#include "wsrvd/handshake.hpp"

#include "wsrv/wsrv.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <variant>

namespace {

using wsrv::ByteBuffer;
using wsrv::ByteView;
using wsrv::ipc::Hello;
using wsrv::ipc::HelloOk;
using wsrv::ipc::Message;
using wsrv::ipc::Ping;
using wsrv::ipc::Status;
using wsrv::wsrvd::Handshake;

ByteView bytes(std::string_view text) {
    return ByteView{reinterpret_cast<const std::byte*>(text.data()), text.size()};  // NOLINT
}

Hello hello(std::string_view token, std::uint16_t major = wsrv::ipc::kVersionMajor) {
    Hello m;
    m.major = major;
    m.token = bytes(token);
    return m;
}

void expect_refused(const Handshake::Outcome& outcome) {
    EXPECT_FALSE(outcome.accepted);
    const auto* status = std::get_if<Status>(&outcome.reply);
    ASSERT_NE(status, nullptr) << "a refusal is a STATUS";
    EXPECT_EQ(status->request, 0U) << "HELLO carries no request id: 0 means 'the handshake'";
    EXPECT_EQ(status->status, static_cast<std::uint8_t>(WSRV_ERR_INVALID_ARGUMENT));
}

TEST(Handshake, AcceptsTheRightVersionAndTokenAndAnswersWithTheFrameLimit) {
    const Handshake handshake{"s3cret", 123456};
    const auto outcome = handshake.on_first_message(Message{hello("s3cret")});
    ASSERT_TRUE(outcome.accepted);
    const auto* ok = std::get_if<HelloOk>(&outcome.reply);
    ASSERT_NE(ok, nullptr);
    EXPECT_EQ(ok->max_frame, 123456U);
    EXPECT_EQ(ok->major, wsrv::ipc::kVersionMajor);
}

TEST(Handshake, ARefusedTokenIsAStatusWithRequestZero) {
    const Handshake handshake{"s3cret", 1000};
    expect_refused(handshake.on_first_message(Message{hello("wrong!")}));
    expect_refused(handshake.on_first_message(Message{hello("")}));
    expect_refused(handshake.on_first_message(Message{hello("s3cret-and-more")}));
}

TEST(Handshake, ADifferentMajorVersionIsRefused) {
    const Handshake handshake{"s3cret", 1000};
    expect_refused(handshake.on_first_message(
        Message{hello("s3cret", static_cast<std::uint16_t>(wsrv::ipc::kVersionMajor + 1))}));
}

TEST(Handshake,
     ANewerMinorVersionIsAccepted) {  // minor versions only add; the daemon answers with its own
    const Handshake handshake{"", 1000};
    Hello newer = hello("");
    newer.minor = static_cast<std::uint16_t>(wsrv::ipc::kVersionMinor + 7);
    EXPECT_TRUE(handshake.on_first_message(Message{newer}).accepted);
}

TEST(Handshake, AFirstFrameThatIsNotHelloIsRefused) {
    const Handshake handshake{"", 1000};
    expect_refused(handshake.on_first_message(Message{Ping{}}));
    expect_refused(handshake.on_first_message(Message{Status{.request = 9, .status = 0}}));
}

TEST(Handshake, WithoutAConfiguredTokenAnyTokenIsAccepted) {
    const Handshake handshake{"", 1000};
    EXPECT_TRUE(handshake.on_first_message(Message{hello("")}).accepted);
    EXPECT_TRUE(handshake.on_first_message(Message{hello("whatever")}).accepted);
}

TEST(Handshake, EveryRefusalLooksTheSameSoTheReasonCannotBeProbed) {
    const Handshake handshake{"s3cret", 1000};
    const auto wrong_token = handshake.on_first_message(Message{hello("nope")});
    const auto wrong_major = handshake.on_first_message(Message{hello("s3cret", 9)});
    const auto not_hello = handshake.on_first_message(Message{Ping{}});
    EXPECT_EQ(std::get<Status>(wrong_token.reply).status,
              std::get<Status>(wrong_major.reply).status);
    EXPECT_EQ(std::get<Status>(wrong_token.reply).status, std::get<Status>(not_hello.reply).status);
}

TEST(Handshake, TheBeforeHelloFrameLimitFitsTheLargestLegalHelloExactly) {
    const std::string token(wsrv::ipc::kMaxTokenSize, 'x');
    ByteBuffer out{1U << 16};
    ASSERT_TRUE(wsrv::ipc::encode(Message{hello(token)}, 1U << 16, out).has_value());
    // The wire frame is a 4-byte length field followed by `length` bytes.
    EXPECT_EQ(out.size() - 4, wsrv::wsrvd::kHelloMaxFrame);
}

TEST(Handshake, WhenTheDaemonAsksQuestionsAnOlderClientIsRefusedTheSameWay) {  // ADR-0018
    const Handshake handshake{"", 1000, true};
    Hello old = hello("");
    old.minor = wsrv::ipc::kHandshakeHookMinor - 1;  // cannot read a Handshake event
    expect_refused(handshake.on_first_message(Message{old}));
    Hello current = hello("");
    current.minor = wsrv::ipc::kHandshakeHookMinor;
    EXPECT_TRUE(handshake.on_first_message(Message{current}).accepted);
    const Handshake quiet{"", 1000};  // hook off: an old client has nothing to be asked
    EXPECT_TRUE(quiet.on_first_message(Message{old}).accepted);
}

}  // namespace

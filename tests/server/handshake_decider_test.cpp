// SPDX-License-Identifier: Apache-2.0
// The pieces of the asynchronous handshake decision that need no connection (ADR-0018): what the
// application is told, how it answers from another thread, and the bound on waiting upgrades.
#include "server/handshake_decider.hpp"

#include "base/clock.hpp"
#include "http/request_parser.hpp"
#include "net/event_loop.hpp"
#include "net/syscalls.hpp"
#include "server/pending_gate.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using wsrv::ConnectionId;
using wsrv::ErrorCode;
using wsrv::HandshakeRequest;
using wsrv::HandshakeVerdict;

wsrv::HttpRequest parse(std::string_view text) {
    wsrv::HttpRequestParser parser{wsrv::HttpLimits{}};
    const auto progress = parser.feed(wsrv::as_bytes(text));
    EXPECT_TRUE(progress.has_value() && progress->complete);
    return parser.request();
}

std::string upgrade(std::string_view extra_headers = "", std::string_view target = "/chat") {
    return "GET " + std::string{target} +
           " HTTP/1.1\r\nHost: example.com\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
           "Origin: https://app.example.com\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "Sec-WebSocket-Version: 13\r\n" +
           std::string{extra_headers} + "\r\n";
}

/// make_handshake_request with the pieces a test cares about.
std::optional<HandshakeRequest> make(ConnectionId id,
                                     const wsrv::HttpRequest& request,
                                     std::string_view negotiated,
                                     const std::vector<std::string>& forward) {
    const wsrv::Endpoint peer;
    return wsrv::make_handshake_request(wsrv::HandshakeFacts{id, peer, request, negotiated},
                                        forward);
}

// ---- what the application is told ---------------------------------------------------------------

TEST(MakeHandshakeRequest, DescribesTheRequestWithoutForwardingSecretsByDefault) {
    const auto request = parse(upgrade("Cookie: session=secret\r\nAuthorization: Bearer t\r\n"
                                       "Sec-WebSocket-Protocol: chat, v2\r\n",
                                       "/room/7?token=abc&x=1"));
    const std::optional<HandshakeRequest> made = make(9, request, "chat", {});
    ASSERT_TRUE(made.has_value());
    EXPECT_EQ(made->id, 9U);
    EXPECT_EQ(made->path, "/room/7");
    EXPECT_EQ(made->query, "token=abc&x=1");
    EXPECT_EQ(made->origin, "https://app.example.com");
    EXPECT_EQ(made->host, "example.com");
    EXPECT_EQ(made->subprotocol, "chat");
    EXPECT_EQ(made->offered_subprotocols, (std::vector<std::string>{"chat", "v2"}));
    EXPECT_TRUE(made->forwarded.empty()) << "Cookie and Authorization only travel if listed";
}

TEST(MakeHandshakeRequest, ForwardsOnlyTheListedHeadersEveryValueInOrder) {
    const auto request = parse(upgrade("Cookie: a=1\r\nX-Other: no\r\nCookie: b=2\r\n"
                                       "authorization: Bearer t\r\n"));
    const auto made = make(1, request, "", {"Cookie", "Authorization"});
    ASSERT_TRUE(made.has_value());
    ASSERT_EQ(made->forwarded.size(), 3U);
    EXPECT_EQ(made->forwarded[0].name, "Cookie");
    EXPECT_EQ(made->forwarded[0].value, "a=1");
    EXPECT_EQ(made->forwarded[1].value, "b=2");
    EXPECT_EQ(made->forwarded[2].name, "Authorization") << "the configured spelling is used";
    EXPECT_EQ(made->forwarded[2].value, "Bearer t");
}

TEST(MakeHandshakeRequest, ARequestWithoutAQueryHasAnEmptyOne) {
    const auto made = make(1, parse(upgrade()), "", {});
    ASSERT_TRUE(made.has_value());
    EXPECT_EQ(made->path, "/chat");
    EXPECT_TRUE(made->query.empty());
}

TEST(MakeHandshakeRequest, AForwardedValueOver4KiBIsRefusedNotCut) {  // EC-H34
    const std::string big(wsrv::kMaxForwardedValueBytes + 1, 'a');
    const auto request = parse(upgrade("Cookie: " + big + "\r\n"));
    EXPECT_FALSE(make(1, request, "", {"Cookie"}).has_value());
    const std::string fits(wsrv::kMaxForwardedValueBytes, 'a');
    const auto fine = parse(upgrade("Cookie: " + fits + "\r\n"));
    EXPECT_TRUE(make(1, fine, "", {"Cookie"}).has_value());
}

TEST(MakeHandshakeRequest, AForwardedTotalOver16KiBIsRefused) {  // EC-H34
    // Five values of 3.5 KiB each: every one fits alone, together they do not.
    std::string headers;
    for (int i = 0; i < 5; ++i) {
        headers += "Cookie: " + std::string(3500, 'a') + "\r\n";
    }
    wsrv::HttpLimits limits;
    limits.max_total_bytes = 64 * 1024;
    wsrv::HttpRequestParser parser{limits};
    ASSERT_TRUE(parser.feed(wsrv::as_bytes(upgrade(headers))).has_value());
    EXPECT_FALSE(make(1, parser.request(), "", {"Cookie"}).has_value());
}

// ---- the answer, from any thread ----------------------------------------------------------------

class FakeDirectory final : public wsrv::ISessionDirectory {
public:
    void run(ConnectionId /*id*/, const wsrv::SessionCommand& /*command*/) override {}
    void decide(ConnectionId id, const HandshakeVerdict& verdict) override {
        verdicts.emplace_back(id, verdict);
        threads.push_back(std::this_thread::get_id());
    }
    std::vector<std::pair<ConnectionId, HandshakeVerdict>> verdicts;
    std::vector<std::thread::id> threads;
};

struct PendingHandleTest : public ::testing::Test {
    static inline wsrv::LinuxSyscalls sys;
    static inline wsrv::SteadyClock clock;
    FakeDirectory directory;
    std::unique_ptr<wsrv::EventLoop> loop = [] {
        auto created = wsrv::EventLoop::create(sys, clock);
        EXPECT_TRUE(created.has_value());
        return std::move(*created);
    }();
    std::shared_ptr<wsrv::LoopLink> link = std::make_shared<wsrv::LoopLink>(*loop, directory);

    wsrv::PendingHandle handle(std::vector<std::string> offered = {"chat", "v2"}) {
        return wsrv::PendingHandle{
            link, 5, std::make_shared<const std::vector<std::string>>(std::move(offered))};
    }
    void run_loop() {
        for (int i = 0; i < 5; ++i) {
            ASSERT_TRUE(loop->run_once(10ms).has_value());
        }
    }
};

TEST_F(PendingHandleTest, AnAcceptReachesTheDirectoryOnTheLoopThreadNotTheCallersThread) {
    std::thread caller([&] { EXPECT_TRUE(handle().accept().has_value()); });
    caller.join();
    run_loop();
    ASSERT_EQ(directory.verdicts.size(), 1U);
    EXPECT_EQ(directory.verdicts[0].first, 5U);
    EXPECT_TRUE(directory.verdicts[0].second.accept);
    EXPECT_FALSE(directory.verdicts[0].second.subprotocol.has_value());
    EXPECT_EQ(directory.threads[0], std::this_thread::get_id());
}

TEST_F(PendingHandleTest, AcceptMayChooseAnOfferedSubprotocolOnly) {
    EXPECT_TRUE(handle().accept("v2").has_value());
    const auto bad = handle().accept("not-offered");
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code, ErrorCode::InvalidArgument);  // the caller learns, the wait goes on
    run_loop();
    ASSERT_EQ(directory.verdicts.size(), 1U) << "the bad call posted nothing";
    EXPECT_EQ(directory.verdicts[0].second.subprotocol, "v2");
}

TEST_F(PendingHandleTest, RejectAcceptsOnlyTheDocumentedStatuses) {
    for (const int status : {400, 401, 403, 404, 429, 500, 503}) {
        EXPECT_TRUE(handle().reject(status).has_value()) << status;
    }
    for (const int status : {0, 200, 301, 302, 418, 502, 599, -1}) {
        const auto result = handle().reject(status);
        ASSERT_FALSE(result.has_value()) << status;
        EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
    }
    run_loop();
    ASSERT_EQ(directory.verdicts.size(), 7U);
    EXPECT_FALSE(directory.verdicts[0].second.accept);
    EXPECT_EQ(directory.verdicts[0].second.status, 400);
}

TEST_F(PendingHandleTest, AnInertHandleAndAClosedLinkReportClosing) {
    EXPECT_EQ(wsrv::PendingHandle{}.accept().error().code, ErrorCode::Closing);
    EXPECT_EQ(wsrv::PendingHandle{}.reject(403).error().code, ErrorCode::Closing);
    link->close();
    EXPECT_EQ(handle().reject(403).error().code, ErrorCode::Closing);
}

// ---- the bound on waiting upgrades --------------------------------------------------------------

TEST(PendingGate, AdmitsUpToTheBoundAndFreesTheSlotWhenItsOwnerIsGone) {  // EC-H31
    wsrv::PendingGate gate{2};
    auto first = gate.try_enter();
    auto second = gate.try_enter();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(gate.waiting(), 2U);
    EXPECT_FALSE(gate.try_enter().has_value()) << "the next one over the bound is refused";
    first.reset();
    EXPECT_EQ(gate.waiting(), 1U);
    EXPECT_TRUE(gate.try_enter().has_value());
}

TEST(PendingGate, AMovedSlotIsReleasedExactlyOnce) {
    wsrv::PendingGate gate{3};
    {
        auto slot = gate.try_enter();
        ASSERT_TRUE(slot.has_value());
        std::optional<wsrv::PendingSlot> moved{std::move(*slot)};
        slot.reset();  // the moved-from slot must not release again
        EXPECT_EQ(gate.waiting(), 1U);
    }
    EXPECT_EQ(gate.waiting(), 0U);
}

TEST(PendingGate, IsSafeToUseFromSeveralThreadsAndNeverExceedsItsBound) {
    wsrv::PendingGate gate{8};
    std::atomic<std::size_t> most{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 2000; ++i) {
                if (auto slot = gate.try_enter()) {
                    std::size_t seen = gate.waiting();
                    std::size_t known = most.load();
                    while (seen > known && !most.compare_exchange_weak(known, seen)) {
                    }
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_LE(most.load(), 8U);
    EXPECT_EQ(gate.waiting(), 0U);
}

}  // namespace

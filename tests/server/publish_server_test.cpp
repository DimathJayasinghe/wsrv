// SPDX-License-Identifier: Apache-2.0
// The publish listener on a running server (ADR-0028 section 3, amendments 1 and 2): the happy path
// over real sockets, and the hostile inputs that make it safe to expose: a wrong token never
// reaches the action or a body buffer, a long head, a body that never ends, bytes smuggled after
// the body, a transfer coding, too many clients, and off unless asked for.
#include "server/server_builder.hpp"
#include "support/fakes.hpp"
#include "support/raw_socket.hpp"
#include "support/running_server.hpp"
#include "support/tcp_client.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using wsrv::test::RawSocket;
using wsrv::test::RunningServer;

const std::string kToken(40, 'k');  // 40 bytes: over the 32-byte floor

class SilentFactory final : public wsrv::IHandlerFactory {
public:
    std::unique_ptr<wsrv::IWebSocketHandler> create(const wsrv::ConnectionInfo& /*info*/) override {
        return std::make_unique<wsrv::IWebSocketHandler>();
    }
};

/// What the action was asked to do (it runs on the server's thread; the test reads it afterwards).
struct Call {
    std::string topic;
    std::string payload;
    bool binary = false;
    std::uint64_t except = 0;
};

class Recorder {
public:
    wsrv::PublishSources sources() {
        wsrv::PublishSources out;
        out.token = kToken;
        out.publish =
            [this](const wsrv::PublishMessage& message) -> wsrv::Result<wsrv::PublishReport> {
            {
                const std::lock_guard lock{mutex_};
                calls_.push_back(Call{
                    std::string{message.topic},
                    std::string{reinterpret_cast<const char*>(message.payload.data()),  // NOLINT
                                message.payload.size()},
                    message.binary,
                    message.except});
            }
            if (message.topic == "bad") {
                return wsrv::fail(wsrv::ErrorCode::InvalidArgument, "test");
            }
            if (message.topic == "full") {
                return wsrv::PublishReport{.subscribers = 2, .accepted = 0, .refused = 2};
            }
            return wsrv::PublishReport{.subscribers = 3, .accepted = 3, .refused = 0};
        };
        return out;
    }
    std::vector<Call> calls() {
        const std::lock_guard lock{mutex_};
        return calls_;
    }

private:
    std::mutex mutex_;
    std::vector<Call> calls_;
};

struct Fixture {
    SilentFactory factory;
    wsrv::test::SharedLogger logs;
    Recorder recorder;
    wsrv::PublishSources sources = recorder.sources();
    wsrv::ServerBuilder builder;
    Fixture() {
        builder.listen("127.0.0.1", 0).handler_factory(factory).admin("127.0.0.1", 0);
        builder.logger(logs);
        builder.publish("127.0.0.1", 0, sources);
        builder.config().handshake.allowed_origins = {"https://app.example.com"};
        builder.config().publish_request_timeout = 400ms;
        builder.config().publish_max_connections = 2;
        builder.config().publish_max_body_bytes = 1024;
    }
};

std::string post(const std::string& query,
                 const std::string& body,
                 const std::string& token = kToken,
                 const std::string& extra = {}) {
    return "POST /publish" + query + " HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer " + token +
           "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + extra + "\r\n" + body;
}

/// One request, read until the server closes.
std::string round_trip(std::uint16_t port, const std::string& request) {
    RawSocket client{port};
    EXPECT_TRUE(client.connected());
    client.send(request);
    bool closed = false;
    const std::string response = client.read_all(closed);
    EXPECT_TRUE(closed) << "every answer ends with a close";
    return response;
}

bool starts_with(const std::string& text, const std::string& prefix) {
    return text.rfind(prefix, 0) == 0;
}

std::uint16_t publish_port(RunningServer& running) {
    return running.server().publish_endpoint()->port;
}

TEST(PublishServer, AnAuthenticatedPublishReachesTheActionAndIsAnsweredWithTheCounts) {
    Fixture f;
    RunningServer running{f.builder};
    const std::string response =
        round_trip(publish_port(running), post("?topic=room.7&except=42&binary=1", "hello"));
    EXPECT_TRUE(starts_with(response, "HTTP/1.1 200 OK\r\n")) << response;
    EXPECT_NE(response.find("subscribers=3 accepted=3 refused=0\n"), std::string::npos) << response;
    const auto calls = f.recorder.calls();
    ASSERT_EQ(calls.size(), 1U);
    EXPECT_EQ(calls[0].topic, "room.7");
    EXPECT_EQ(calls[0].payload, "hello");
    EXPECT_TRUE(calls[0].binary);
    EXPECT_EQ(calls[0].except, 42U);
}

TEST(PublishServer, TheBodyMayArriveInPiecesAfterTheHead) {
    Fixture f;
    RunningServer running{f.builder};
    RawSocket client{publish_port(running)};
    const std::string whole = post("?topic=t", "0123456789");
    const std::size_t cut = whole.size() - 6;  // head plus the first 4 bytes, then the rest
    client.send(whole.substr(0, cut));
    std::this_thread::sleep_for(50ms);
    client.send(whole.substr(cut, 3));
    std::this_thread::sleep_for(50ms);
    client.send(whole.substr(cut + 3));
    bool closed = false;
    const std::string response = client.read_all(closed);
    EXPECT_TRUE(starts_with(response, "HTTP/1.1 200 OK\r\n")) << response;
    ASSERT_EQ(f.recorder.calls().size(), 1U);
    EXPECT_EQ(f.recorder.calls()[0].payload, "0123456789");
}

TEST(PublishServer, AWrongTokenNeverReachesTheActionAndANeverEndingBodyIsNeverWaitedFor) {
    Fixture f;
    RunningServer running{f.builder};
    // Declares far more than the bound and sends nothing of it: 401 at once, not 413, not a wait.
    const std::string head = "POST /publish?topic=t HTTP/1.1\r\nAuthorization: Bearer " +
                             std::string(40, 'x') + "\r\nContent-Length: 999999999\r\n\r\n";
    const auto start = std::chrono::steady_clock::now();
    const std::string response = round_trip(publish_port(running), head);
    EXPECT_TRUE(starts_with(response, "HTTP/1.1 401 ")) << response;
    EXPECT_LT(std::chrono::steady_clock::now() - start, 300ms) << "answered before the deadline";
    EXPECT_TRUE(f.recorder.calls().empty());
    EXPECT_NE(response.find("WWW-Authenticate: Bearer\r\n"), std::string::npos);
}

TEST(PublishServer, ARefusedPublishIsALoggedWarningThatNeverRepeatsTheCredential) {  // ADR-0037
    Fixture f;
    RunningServer running{f.builder};
    const std::string wrong(40, 'x');
    const std::string response = round_trip(publish_port(running), post("?topic=t", "hi", wrong));
    ASSERT_TRUE(starts_with(response, "HTTP/1.1 401 ")) << response;
    const auto line = f.logs.wait_for("publish_rejected");
    ASSERT_TRUE(line.has_value());
    EXPECT_EQ(line->level, wsrv::LogLevel::Warn);
    EXPECT_NE(line->message.find("401"), std::string::npos) << line->message;
    const std::string all = f.logs.everything();
    EXPECT_EQ(all.find(wrong), std::string::npos) << "the credential the peer sent is never logged";
    EXPECT_EQ(all.find(kToken), std::string::npos) << "nor the real one";
}

TEST(PublishServer,
     AnAcceptedPublishWritesNoWarning) {  // ADR-0037: success is a counter, not a line
    Fixture f;
    RunningServer running{f.builder};
    const std::string response = round_trip(publish_port(running), post("?topic=room.7", "hello"));
    ASSERT_TRUE(starts_with(response, "HTTP/1.1 200 OK")) << response;
    EXPECT_FALSE(f.logs.wait_for("publish_rejected", 200ms).has_value());
}

TEST(PublishServer, ATokenThatIsAPrefixOrLongerOrEmptyIsRefusedToo) {
    Fixture f;
    RunningServer running{f.builder};
    for (const std::string& token :
         {kToken.substr(0, 39), kToken + "k", std::string{"k"}, std::string(40, 'K')}) {
        EXPECT_TRUE(starts_with(round_trip(publish_port(running), post("?topic=t", "x", token)),
                                "HTTP/1.1 401 "))
            << token.size();
    }
    EXPECT_TRUE(
        starts_with(round_trip(publish_port(running),
                               "POST /publish?topic=t HTTP/1.1\r\nContent-Length: 1\r\n\r\nx"),
                    "HTTP/1.1 401 "));
    EXPECT_TRUE(f.recorder.calls().empty());
}

TEST(PublishServer, ABodyOverTheBoundIs413AfterAuthenticationAndNothingIsPublished) {
    Fixture f;
    RunningServer running{f.builder};
    const std::string head = "POST /publish?topic=t HTTP/1.1\r\nAuthorization: Bearer " + kToken +
                             "\r\nContent-Length: 1025\r\n\r\n";
    EXPECT_TRUE(starts_with(round_trip(publish_port(running), head), "HTTP/1.1 413 "));
    EXPECT_TRUE(f.recorder.calls().empty());
    // The bound itself is fine.
    EXPECT_TRUE(
        starts_with(round_trip(publish_port(running), post("?topic=t", std::string(1024, 'a'))),
                    "HTTP/1.1 200 "));
}

TEST(PublishServer, FramingTricksAreRefusedWithTheirOwnStatus) {
    Fixture f;
    RunningServer running{f.builder};
    const std::uint16_t port = publish_port(running);
    const std::string auth = "Authorization: Bearer " + kToken + "\r\n";
    const auto status = [&](const std::string& request) {
        const std::string response = round_trip(port, request);
        return response.substr(9, 3);
    };
    EXPECT_EQ(status("POST /publish?topic=t HTTP/1.1\r\n" + auth +
                     "Transfer-Encoding: chunked\r\n\r\n1\r\nx\r\n0\r\n\r\n"),
              "501");
    EXPECT_EQ(status("POST /publish?topic=t HTTP/1.1\r\n" + auth +
                     "Content-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\nx"),
              "501");
    EXPECT_EQ(status("POST /publish?topic=t HTTP/1.1\r\n" + auth + "\r\n"), "411");
    EXPECT_EQ(status("POST /publish?topic=t HTTP/1.1\r\n" + auth +
                     "Content-Length: 1\r\nContent-Length: 1\r\n\r\nx"),
              "400");
    EXPECT_EQ(status("POST /publish?topic=t HTTP/1.1\r\n" + auth +
                     "Expect: 100-continue\r\nContent-Length: 1\r\n\r\nx"),
              "417");
    EXPECT_EQ(status("GET /publish?topic=t HTTP/1.1\r\n" + auth + "\r\n"), "405");
    EXPECT_EQ(status("POST /elsewhere HTTP/1.1\r\n" + auth + "Content-Length: 0\r\n\r\n"), "404");
    EXPECT_EQ(status("POST /publish?topic=a%2Fb HTTP/1.1\r\n" + auth + "Content-Length: 0\r\n\r\n"),
              "400");
    EXPECT_TRUE(f.recorder.calls().empty());
}

TEST(PublishServer, BytesAfterTheDeclaredBodyAreARequestSmugglingShapeAndRefused) {
    Fixture f;
    RunningServer running{f.builder};
    const std::string smuggled =
        post("?topic=t", "ab") + "POST /publish?topic=evil HTTP/1.1\r\n\r\n";
    EXPECT_TRUE(starts_with(round_trip(publish_port(running), smuggled), "HTTP/1.1 400 "));
    EXPECT_TRUE(f.recorder.calls().empty());
}

TEST(PublishServer, ALongHeadIs400) {
    Fixture f;
    RunningServer running{f.builder};
    const std::string head =
        post("?topic=t", "x", kToken, "X-Padding: " + std::string(5000, 'p') + "\r\n");
    EXPECT_TRUE(starts_with(round_trip(publish_port(running), head), "HTTP/1.1 400 "));
    EXPECT_TRUE(f.recorder.calls().empty());
}

TEST(PublishServer, ARequestThatNeverEndsIsClosedAtTheDeadline) {
    Fixture f;
    RunningServer running{f.builder};
    {  // a head that never completes (slowloris)
        RawSocket client{publish_port(running)};
        client.send("POST /publish?topic=t HTTP/1.1\r\nHost: x\r\n");
        bool closed = false;
        const std::string response = client.read_all(closed, 2s);
        EXPECT_TRUE(closed);
        EXPECT_TRUE(response.empty()) << "no answer to half a request";
    }
    {  // a body that stops short
        RawSocket client{publish_port(running)};
        client.send(
            post("?topic=t", "0123456789").substr(0, post("?topic=t", "0123456789").size() - 4));
        bool closed = false;
        static_cast<void>(client.read_all(closed, 2s));
        EXPECT_TRUE(closed);
    }
    EXPECT_TRUE(f.recorder.calls().empty());
}

TEST(PublishServer, OnlyAFewClientsAreServedAtOnce) {
    Fixture f;  // two at a time
    RunningServer running{f.builder};
    RawSocket first{publish_port(running)};
    RawSocket second{publish_port(running)};
    first.send("POST /publish");  // both hold a slot with half a request
    second.send("POST /publish");
    std::this_thread::sleep_for(100ms);
    RawSocket third{publish_port(running)};
    third.send(post("?topic=t", "x"));
    bool closed = false;
    const std::string response = third.read_all(closed, 1s);
    EXPECT_TRUE(closed);
    EXPECT_TRUE(response.empty()) << "the third client is dropped without being served";
    EXPECT_TRUE(f.recorder.calls().empty());
}

TEST(PublishServer, TheActionsAnswersBecomeStatuses) {
    Fixture f;
    RunningServer running{f.builder};
    EXPECT_TRUE(
        starts_with(round_trip(publish_port(running), post("?topic=bad", "x")), "HTTP/1.1 400 "));
    const std::string full = round_trip(publish_port(running), post("?topic=full", "x"));
    EXPECT_TRUE(starts_with(full, "HTTP/1.1 429 ")) << full;
    EXPECT_NE(full.find("subscribers=2 accepted=0 refused=2"), std::string::npos);
}

TEST(PublishServer, EveryAnswerIsCountedAndTheCountersReachMetrics) {
    Fixture f;
    RunningServer running{f.builder};
    const std::uint16_t port = publish_port(running);
    static_cast<void>(round_trip(port, post("?topic=t", "x")));
    static_cast<void>(round_trip(port, post("?topic=t", "x")));
    static_cast<void>(round_trip(port, post("?topic=t", "x", std::string(40, 'z'))));
    static_cast<void>(round_trip(port, "GET /publish HTTP/1.1\r\n\r\n"));
    const std::string metrics = round_trip(running.server().admin_endpoint()->port,
                                           "GET /metrics HTTP/1.1\r\nHost: x\r\n\r\n");
    EXPECT_NE(metrics.find("wsrv_publish_http_total{status=\"200\"} 2\n"), std::string::npos)
        << metrics;
    EXPECT_NE(metrics.find("wsrv_publish_http_total{status=\"401\"} 1\n"), std::string::npos);
    EXPECT_NE(metrics.find("wsrv_publish_http_total{status=\"405\"} 1\n"), std::string::npos);
    EXPECT_NE(metrics.find("wsrv_publish_http_total{status=\"413\"} 0\n"), std::string::npos);
}

TEST(PublishServer, ItIsOffUnlessAskedForAndRefusesAShortToken) {
    SilentFactory factory;
    wsrv::ServerBuilder off;
    off.listen("127.0.0.1", 0).handler_factory(factory);
    RunningServer running{off};
    EXPECT_FALSE(running.server().publish_endpoint().has_value());

    wsrv::PublishSources weak;
    weak.token = std::string(31, 'k');
    weak.publish = [](const wsrv::PublishMessage&) -> wsrv::Result<wsrv::PublishReport> {
        return wsrv::PublishReport{};
    };
    wsrv::ServerBuilder short_token;
    short_token.listen("127.0.0.1", 0).handler_factory(factory).publish("127.0.0.1", 0, weak);
    EXPECT_THROW(static_cast<void>(short_token.build()), wsrv::ConfigError);
}

TEST(PublishServer, WhileTheServerShutsDownThePublishAnswerIs503) {
    Fixture f;
    f.builder.config().shutdown_grace = 3s;
    RunningServer running{f.builder};
    wsrv::test::TcpClient websocket{running.port()};
    ASSERT_TRUE(websocket.upgrade());  // keeps the server in its grace period
    ASSERT_TRUE(running.server().request_shutdown().has_value());
    std::this_thread::sleep_for(100ms);
    const std::string response = round_trip(publish_port(running), post("?topic=t", "x"));
    EXPECT_TRUE(starts_with(response, "HTTP/1.1 503 ")) << response;
    EXPECT_TRUE(f.recorder.calls().empty());
}

}  // namespace

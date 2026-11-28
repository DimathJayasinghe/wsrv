// SPDX-License-Identifier: Apache-2.0
#include "capi/embedded_server.hpp"

#include "support/stderr_capture.hpp"
#include "support/tcp_client.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace wsrv;
using namespace wsrv::test;
using namespace std::chrono_literals;

EmbeddedConfig base_config() {
    EmbeddedConfig config;
    EXPECT_TRUE(config.set("allow_origin", "https://app.example.com").has_value());
    EXPECT_TRUE(config.set("shutdown_grace_ms", "2000").has_value());
    return config;
}

std::unique_ptr<EmbeddedServer> started(const EmbeddedConfig& config = base_config()) {
    auto server = EmbeddedServer::create(config);
    EXPECT_TRUE(server.has_value());
    EXPECT_TRUE((*server)->start().has_value());
    return std::move(*server);
}

std::string as_string(const std::vector<std::byte>& bytes) {
    std::string out;
    for (const std::byte b : bytes) {
        out.push_back(static_cast<char>(b));
    }
    return out;
}

// ADR-0037: what an operator sees. These read the process's real stderr.

TEST(EmbeddedServerLogging, StartingWritesOneListeningLineWithTheActualPort) {
    StderrCapture capture;
    auto server = started();
    const std::string text = capture.wait_for("event=listening");
    capture.restore();
    EXPECT_NE(text.find(" level=info event=listening "), std::string::npos) << text;
    EXPECT_NE(text.find("port " + std::to_string(server->port()) + "\""), std::string::npos)
        << text;
    EXPECT_EQ(text.rfind("ts=", 0), 0U) << "the line starts with its timestamp";
}

// The log goes to the descriptor that was stderr when the server was created, by a private copy,
// not to "whatever is descriptor 2 now": a host that closes or redirects its stdio afterwards (a
// daemon that closed 0-2 and then let a socket take number 2) must not receive log lines in a
// client socket.
TEST(EmbeddedServerLogging, TheLogDoesNotFollowDescriptorTwoWhenTheHostChangesIt) {
    StderrCapture capture;
    auto server = EmbeddedServer::create(base_config());
    ASSERT_TRUE(server.has_value());
    const int elsewhere = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
    ASSERT_GE(elsewhere, 0);
    ASSERT_NE(::dup2(elsewhere, STDERR_FILENO), -1);  // the host redirects stderr after create()
    ::close(elsewhere);
    ASSERT_TRUE((*server)->start().has_value());
    const std::string text = capture.wait_for("event=listening");
    capture.restore();
    EXPECT_NE(text.find("event=listening"), std::string::npos)
        << "the line must reach the stderr of create() time, not /dev/null";
}

TEST(EmbeddedServerLogging, TheLevelKeepsInfoOut) {
    StderrCapture capture;
    EmbeddedConfig config = base_config();
    ASSERT_TRUE(config.set("log_level", "warn").has_value());
    auto server = started(config);
    const std::string text = capture.take(200ms);
    capture.restore();
    EXPECT_EQ(text, "") << "a quiet server at warn level writes nothing while healthy";
}

TEST(EmbeddedServerLogging, ARefusedOriginIsLoggedWithoutTheAddressUnlessAsked) {
    StderrCapture capture;
    auto server = started();
    {
        TcpClient client{server->port()};
        EXPECT_FALSE(client.upgrade("https://evil.example.x"));
    }
    const std::string text = capture.wait_for("event=origin_refused");
    capture.restore();
    EXPECT_NE(text.find(" level=warn event=origin_refused conn="), std::string::npos) << text;
    EXPECT_EQ(text.find("peer="), std::string::npos)
        << "an address is personal data: off by default";
    EXPECT_EQ(text.find("evil.example.x"), std::string::npos)
        << "what the peer sent is never logged";
}

TEST(EmbeddedServerLogging, WithLogPeerTheClientAddressIsNamed) {
    StderrCapture capture;
    EmbeddedConfig config = base_config();
    ASSERT_TRUE(config.set("log_peer", "true").has_value());
    auto server = started(config);
    {
        TcpClient client{server->port()};
        EXPECT_FALSE(client.upgrade("https://evil.example.x"));
    }
    const std::string text = capture.wait_for("event=origin_refused");
    capture.restore();
    EXPECT_NE(text.find(" peer=127.0.0.1:"), std::string::npos) << text;
}

#ifndef WSRV_TEST_HAS_TLS
TEST(EmbeddedServer, AskingForTlsInABuildWithoutTlsFailsInsteadOfServingPlainText) {
    EmbeddedConfig config = base_config();
    ASSERT_TRUE(config.set("tls_cert", "/etc/wsrv/cert.pem").has_value());
    ASSERT_TRUE(config.set("tls_key", "/etc/wsrv/key.pem").has_value());
    auto server = EmbeddedServer::create(config);
    ASSERT_TRUE(server.has_value());
    const auto started = (*server)->start();
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error().code, ErrorCode::InvalidConfig);
    EXPECT_NE((*server)->start_error().find("no TLS"), std::string::npos);
    EXPECT_FALSE((*server)->running());
}
#endif

#ifndef WSRV_TEST_HAS_DEFLATE
TEST(EmbeddedServer, AskingForDeflateInABuildWithoutZlibFailsInsteadOfServingUncompressed) {
    EmbeddedConfig config = base_config();
    ASSERT_TRUE(config.set("permessage_deflate", "true").has_value());
    auto server = EmbeddedServer::create(config);
    ASSERT_TRUE(server.has_value());
    const auto started = (*server)->start();
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error().code, ErrorCode::InvalidConfig);
    EXPECT_NE((*server)->start_error().find("zlib"), std::string::npos);
    EXPECT_FALSE((*server)->running());
}
#endif

TEST(EmbeddedServer, StartListensOnTheChosenPortAndTheDestructorEndsTheThread) {
    const auto server = started();
    EXPECT_NE(server->port(), 0U);
    EXPECT_TRUE(server->running());
    TcpClient client{server->port()};
    EXPECT_TRUE(client.upgrade());
}  // destroyed with a client still connected: no hang, no leak (ASan/TSan)

TEST(EmbeddedServer, AMessageGoesThroughTheHostAndBack) {
    const auto server = started();
    TcpClient client{server->port()};
    ASSERT_TRUE(client.upgrade());
    const auto open = server->poll(3000ms);
    ASSERT_TRUE(open.has_value());
    EXPECT_EQ(open->type, HostEventType::Open);

    ASSERT_TRUE(client.send_frame(Opcode::Text, "ping"));
    const auto text = server->poll(3000ms);
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(as_string(text->data), "ping");

    ASSERT_TRUE(server->send_text(text->conn, "pong").has_value());
    const auto reply = client.read_frame();
    ASSERT_TRUE(reply.has_value());
    EXPECT_EQ(reply->payload, "pong");
}

TEST(EmbeddedServer, PollWithNothingWaitingTimesOutAfterAboutTheTimeout) {
    const auto server = started();
    const auto begin = std::chrono::steady_clock::now();
    EXPECT_FALSE(server->poll(150ms).has_value());
    EXPECT_GE(std::chrono::steady_clock::now() - begin, 140ms);
    EXPECT_FALSE(server->poll(0ms).has_value());  // zero means: look, do not wait
}

TEST(EmbeddedServer, ABindFailureIsReportedByStartAndNotLater) {
    const auto first = started();
    EmbeddedConfig config = base_config();
    ASSERT_TRUE(config.set("listen_port", std::to_string(first->port())).has_value());
    auto second = EmbeddedServer::create(config);
    ASSERT_TRUE(second.has_value());
    const Result<void> result = (*second)->start();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::AddressInUse);
    EXPECT_FALSE((*second)->running());
}

TEST(EmbeddedServer, AnUnusableConfigurationIsReportedByStartWithTheReason) {
    EmbeddedConfig config = base_config();
    ASSERT_TRUE(config.set("threads", "0").has_value());  // well formed, but not usable
    auto server = EmbeddedServer::create(config);
    ASSERT_TRUE(server.has_value());
    const Result<void> result = (*server)->start();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidConfig);
    EXPECT_NE((*server)->start_error().find("threads"), std::string::npos);
}

TEST(EmbeddedServer, EventQueueLimitsTooSmallAreRefusedAtCreate) {
    EmbeddedConfig config = base_config();
    ASSERT_TRUE(config.set("event_queue_events", "1").has_value());
    const auto server = EmbeddedServer::create(config);
    ASSERT_FALSE(server.has_value());
    EXPECT_EQ(server.error().code, ErrorCode::InvalidConfig);
}

TEST(EmbeddedServer, StartingTwiceOrUsingItBeforeStartIsRefusedCleanly) {
    auto created = EmbeddedServer::create(base_config());
    ASSERT_TRUE(created.has_value());
    EmbeddedServer& server = **created;
    EXPECT_FALSE(server.poll(0ms).has_value());
    EXPECT_EQ(server.send_text(1, "x").error().code, ErrorCode::Closing);
    EXPECT_FALSE(server.running());
    ASSERT_TRUE(server.start().has_value());
    const Result<void> again = server.start();
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error().code, ErrorCode::InvalidArgument);
}

TEST(EmbeddedServer, AGracefulStopClosesEveryClientAndTheHostSeesEveryClose) {
    const auto server = started();
    TcpClient a{server->port()};
    TcpClient b{server->port()};
    ASSERT_TRUE(a.upgrade());
    ASSERT_TRUE(b.upgrade());
    int opens = 0;
    while (opens < 2) {
        const auto event = server->poll(3000ms);
        ASSERT_TRUE(event.has_value());
        opens += event->type == HostEventType::Open ? 1 : 0;
    }
    ASSERT_TRUE(server->stop().has_value());
    for (TcpClient* client : {&a, &b}) {
        const auto frame = client->read_frame();
        ASSERT_TRUE(frame.has_value());
        EXPECT_EQ(frame->opcode, Opcode::Close);
        EXPECT_TRUE(client->send_frame(Opcode::Close, frame->payload.substr(0, 2)));
    }
    int closes = 0;
    while (closes < 2) {
        const auto event = server->poll(3000ms);
        ASSERT_TRUE(event.has_value()) << "closes seen: " << closes;
        if (event->type == HostEventType::Close) {
            ++closes;
            EXPECT_EQ(event->close_code, 1001);  // the code the clients echoed back
        }
    }
    for (int i = 0; i < 300 && server->running(); ++i) {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_FALSE(server->running());
    EXPECT_TRUE(server->stop().has_value());  // idempotent, also after the server ended
}

// wsrv.h: every OPEN is followed by exactly one CLOSE. Clients that ignore the shutdown are dropped
// after the grace period, on every loop, and the host must still see all their Closes.
TEST(EmbeddedServer, ClientsThatIgnoreTheShutdownStillGiveTheHostOneCloseEachOnEveryLoop) {
    EmbeddedConfig config = base_config();
    ASSERT_TRUE(config.set("threads", "2").has_value());
    ASSERT_TRUE(config.set("shutdown_grace_ms", "100").has_value());
    const auto server = started(config);
    std::vector<std::unique_ptr<TcpClient>> clients;
    for (int i = 0; i < 4; ++i) {  // round-robin: two per loop
        clients.push_back(std::make_unique<TcpClient>(server->port()));
        ASSERT_TRUE(clients.back()->upgrade());
    }
    int opens = 0;
    while (opens < 4) {
        const auto event = server->poll(3000ms);
        ASSERT_TRUE(event.has_value()) << "opens seen: " << opens;
        opens += event->type == HostEventType::Open ? 1 : 0;
    }
    ASSERT_TRUE(server->stop().has_value());
    for (const auto& client : clients) {
        ASSERT_TRUE(client->read_frame().has_value());  // the 1001, which it ignores
    }
    int closes = 0;
    while (closes < 4) {
        const auto event = server->poll(3000ms);
        ASSERT_TRUE(event.has_value()) << "closes seen: " << closes;
        closes += event->type == HostEventType::Close ? 1 : 0;
    }
    EXPECT_EQ(closes, 4);
}

// ADR-0027: the broker is a config key; a host drives rooms by connection id.
TEST(EmbeddedServer, TheBrokerKeyGivesTheHostRoomsByConnectionIdAndItsCountersReachTheMetrics) {
    EmbeddedConfig config = base_config();
    ASSERT_TRUE(config.set("broker", "true").has_value());
    ASSERT_TRUE(config.set("admin_port", "0").has_value());
    const auto server = started(config);
    TcpClient a{server->port()};
    TcpClient b{server->port()};
    ASSERT_TRUE(a.upgrade());
    const auto open_a = server->poll(3000ms);
    ASSERT_TRUE(b.upgrade());
    const auto open_b = server->poll(3000ms);
    ASSERT_TRUE(open_a && open_b);
    ASSERT_TRUE(server->subscribe(open_a->conn, "room.1").has_value());
    ASSERT_TRUE(server->subscribe(open_b->conn, "room.1").has_value());
    EXPECT_EQ(server->topic_subscribers("room.1").value_or(0), 2U);

    const auto report = server->publish_text("room.1", "hello all", open_a->conn);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->accepted, 1U);
    const auto frame = b.read_frame();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->payload, "hello all");
    EXPECT_TRUE(a.silent_for(200ms));

    // The broker's counters reach the server's metrics (ADR-0024 section 6): one publish, one
    // accepted delivery (the publisher was left out).
    TcpClient admin{server->admin_port()};
    ASSERT_TRUE(admin.send("GET /metrics HTTP/1.1\r\nHost: x\r\n\r\n"));
    const std::string page = admin.read_http_head() + admin.read_all();
    EXPECT_NE(page.find("wsrv_broker_publishes_total 1"), std::string::npos) << page;
    EXPECT_NE(page.find("wsrv_broker_deliveries_total{result=\"accepted\"} 1"), std::string::npos)
        << page;
}

// ADR-0041: the admin token reaches the listener through the embedded config, which is how the
// daemon starts the server: /metrics needs it, the probes do not, and a server without one is open.
TEST(EmbeddedServer, TheAdminTokenGuardsMetricsAndNotTheProbes) {
    const std::string token(48, 't');
    EmbeddedConfig config = base_config();
    ASSERT_TRUE(config.set("admin_port", "0").has_value());
    ASSERT_TRUE(config.set_admin_token(token).has_value());
    const auto server = started(config);
    const auto page = [&](const std::string& request) {
        TcpClient admin{server->admin_port()};
        EXPECT_TRUE(admin.send(request));
        return admin.read_http_head() + admin.read_all();
    };
    const std::string refused = page("GET /metrics HTTP/1.1\r\nHost: x\r\n\r\n");
    EXPECT_EQ(refused.rfind("HTTP/1.1 401 Unauthorized\r\n", 0), 0U) << refused;
    EXPECT_EQ(refused.find("wsrv_connections_current"), std::string::npos);
    const std::string allowed =
        page("GET /metrics HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer " + token + "\r\n\r\n");
    EXPECT_EQ(allowed.rfind("HTTP/1.1 200 OK\r\n", 0), 0U) << allowed;
    EXPECT_NE(allowed.find("wsrv_connections_current"), std::string::npos) << allowed;
    EXPECT_EQ(page("GET /healthz HTTP/1.1\r\n\r\n").rfind("HTTP/1.1 200 OK\r\n", 0), 0U);
    EXPECT_EQ(page("GET /readyz HTTP/1.1\r\n\r\n").rfind("HTTP/1.1 200 OK\r\n", 0), 0U);
}

TEST(EmbeddedServer, WithoutTheBrokerKeyBrokerCommandsAreInvalidArgument) {
    const auto server = started();
    const auto result = server->publish_text("room", "x");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

// ADR-0028: the publish endpoint. The token is not a config key, so it cannot leak from a config
// file or the C ABI's string settings; the endpoint needs the broker and a token to start at all.
const std::string kPublishToken(48, 'p');

EmbeddedConfig publish_config() {
    EmbeddedConfig config = base_config();
    EXPECT_TRUE(config.set("broker", "true").has_value());
    EXPECT_TRUE(config.set("publish_port", "0").has_value());
    EXPECT_TRUE(config.set_publish_token(kPublishToken).has_value());
    return config;
}

std::string http_publish(std::uint16_t port, const std::string& query, const std::string& body) {
    TcpClient http{port};
    EXPECT_TRUE(http.valid());
    EXPECT_TRUE(http.send(
        "POST /publish" + query + " HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer " + kPublishToken +
        "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body));
    return http.read_http_head() + http.read_all();
}

TEST(EmbeddedServer, AnHttpPublishReachesTheSubscribersOfARoomThroughTheBroker) {
    const auto server = started(publish_config());
    ASSERT_NE(server->publish_port(), 0);
    TcpClient a{server->port()};
    TcpClient b{server->port()};
    ASSERT_TRUE(a.upgrade());
    const auto open_a = server->poll(3000ms);
    ASSERT_TRUE(b.upgrade());
    const auto open_b = server->poll(3000ms);
    ASSERT_TRUE(open_a && open_b);
    ASSERT_TRUE(server->subscribe(open_a->conn, "room.7").has_value());
    ASSERT_TRUE(server->subscribe(open_b->conn, "room.7").has_value());

    const std::string answer = http_publish(
        server->publish_port(), "?topic=room.7&except=" + std::to_string(open_a->conn), "hi room");
    EXPECT_EQ(answer.rfind("HTTP/1.1 200 OK\r\n", 0), 0U) << answer;
    EXPECT_NE(answer.find("subscribers=1 accepted=1 refused=0"), std::string::npos) << answer;
    const auto frame = b.read_frame();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->payload, "hi room");
    EXPECT_TRUE(a.silent_for(200ms)) << "the excepted connection gets nothing";

    // Not UTF-8 as text is 415; the same bytes as binary go through. A topic nobody joined is a
    // success with zero subscribers.
    EXPECT_EQ(
        http_publish(server->publish_port(), "?topic=room.7", "\xff\xfe").rfind("HTTP/1.1 415 ", 0),
        0U);
    EXPECT_EQ(http_publish(server->publish_port(), "?topic=room.7&binary=1", "\xff\xfe")
                  .rfind("HTTP/1.1 200 ", 0),
              0U);
    EXPECT_NE(http_publish(server->publish_port(), "?topic=empty", "x")
                  .find("subscribers=0 accepted=0 refused=0"),
              std::string::npos);
    EXPECT_EQ(
        http_publish(server->publish_port(), "?topic=bad%20name", "x").rfind("HTTP/1.1 400 ", 0),
        0U);
    EXPECT_EQ(
        http_publish(server->publish_port(), "?topic=bad!name", "x").rfind("HTTP/1.1 400 ", 0), 0U);
}

TEST(EmbeddedServer, PublishNeedsTheBrokerAndATokenOrStartFails) {
    {
        EmbeddedConfig config = base_config();
        ASSERT_TRUE(config.set("publish_port", "0").has_value());
        ASSERT_TRUE(config.set_publish_token(kPublishToken).has_value());  // no broker
        auto server = EmbeddedServer::create(config);
        ASSERT_TRUE(server.has_value());
        const auto result = (*server)->start();
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, ErrorCode::InvalidConfig);
        EXPECT_FALSE((*server)->running());
    }
    {
        EmbeddedConfig config = base_config();
        ASSERT_TRUE(config.set("broker", "true").has_value());
        ASSERT_TRUE(config.set("publish_port", "0").has_value());  // no token
        auto server = EmbeddedServer::create(config);
        ASSERT_TRUE(server.has_value());
        EXPECT_FALSE((*server)->start().has_value());
    }
}

TEST(EmbeddedServer, ThePublishTokenIsNotAKeyAndMustBeLongEnough) {
    EmbeddedConfig config = base_config();
    EXPECT_FALSE(config.set("publish_token", kPublishToken).has_value());  // unknown key: refused
    EXPECT_FALSE(config.set_publish_token(std::string(31, 'x')).has_value());
    EXPECT_TRUE(config.set_publish_token(std::string(32, 'x')).has_value());
}

// ADR-0023: the admin listener is a text key like every other.
TEST(EmbeddedServer, TheAdminKeysServeMetricsOnTheirOwnPort) {
    EmbeddedConfig config = base_config();
    ASSERT_TRUE(config.set("admin_port", "0").has_value());  // 0: the kernel chooses
    const auto server = started(config);
    ASSERT_NE(server->admin_port(), 0);
    TcpClient admin{server->admin_port()};
    ASSERT_TRUE(admin.valid());
    ASSERT_TRUE(admin.send("GET /metrics HTTP/1.1\r\nHost: x\r\n\r\n"));
    const std::string head = admin.read_http_head();
    EXPECT_EQ(head.rfind("HTTP/1.1 200 OK\r\n", 0), 0U) << head;
}

TEST(EmbeddedServer, WithoutAnAdminPortThereIsNoAdminListener) {
    const auto server = started();
    EXPECT_EQ(server->admin_port(), 0);
}

// ADR-0021: the limit is a text key like every other, so the C API, PHP and wsrvd get it for free.
TEST(EmbeddedServer, ThePerIpCapKeyRefusesTheNextConnectionFromTheSameAddressWith429) {
    EmbeddedConfig config = base_config();
    ASSERT_TRUE(config.set("max_connections_per_ip", "1").has_value());
    const auto server = started(config);
    TcpClient first{server->port()};
    ASSERT_TRUE(first.upgrade());
    TcpClient second{server->port()};
    const std::string head = second.read_http_head();
    EXPECT_EQ(head.rfind("HTTP/1.1 429 ", 0), 0U) << head;
    EXPECT_TRUE(second.wait_for_eof());
}

TEST(EmbeddedServer, ASecondServerCanBeStartedAfterTheFirstIsGone) {
    std::uint16_t port = 0;
    {
        const auto first = started();
        port = first->port();
    }
    EmbeddedConfig config = base_config();
    ASSERT_TRUE(config.set("listen_port", std::to_string(port)).has_value());
    auto second = EmbeddedServer::create(config);
    ASSERT_TRUE(second.has_value());
    EXPECT_TRUE((*second)->start().has_value());  // the port was released
}

// ADR-0016 section 2 (fork safety): threads do not survive fork(), so a child that inherited a
// running server must get WrongProcess from every call and must be able to destroy it without
// joining a thread that does not exist there (a join would hang or crash the child).
TEST(EmbeddedServer, AForkedChildGetsWrongProcessAndCanDestroyTheServerWithoutHanging) {
    auto server = started();
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        ::alarm(10);  // a hang in the child ends it by SIGALRM, so the parent sees a failure
        int code = 0;
        if (server->check_process().has_value() || server->start().has_value() ||
            server->stop().has_value() || server->poll(0ms).has_value() ||
            server->send_text(ConnectionId{1}, "x").has_value()) {
            code = 1;
        }
        server.reset();  // destructor in the child
        ::_exit(code);
    }
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    EXPECT_TRUE(WIFEXITED(status)) << "child was killed (hang or crash), status " << status;
    EXPECT_EQ(WEXITSTATUS(status), 0);
    // The parent's server is untouched by the child.
    TcpClient client{server->port()};
    EXPECT_TRUE(client.upgrade());
}

// ADR-0018: with the hook on, the host decides who gets in, through poll() and two commands.
TEST(EmbeddedServer, WithTheHookOnTheHostDecidesWhoGetsIn) {
    EmbeddedConfig config = base_config();
    ASSERT_TRUE(config.set("handshake_hook", "true").has_value());
    ASSERT_TRUE(config.set("handshake_forward_header", "Cookie").has_value());
    const auto server = started(config);

    TcpClient allowed{server->port()};
    std::string request = TcpClient::upgrade_request();
    request.insert(request.size() - 2, "Cookie: session=good\r\n");
    ASSERT_TRUE(allowed.send(request));
    const auto question = server->poll(3000ms);
    ASSERT_TRUE(question.has_value());
    ASSERT_EQ(question->type, HostEventType::Handshake);
    ASSERT_TRUE(server->accept_handshake(question->conn, std::nullopt).has_value());
    EXPECT_EQ(allowed.read_http_head().rfind("HTTP/1.1 101 ", 0), 0U);
    const auto open = server->poll(3000ms);
    ASSERT_TRUE(open.has_value());
    EXPECT_EQ(open->type, HostEventType::Open);

    TcpClient refused{server->port()};
    ASSERT_TRUE(refused.send(TcpClient::upgrade_request()));
    const auto second = server->poll(3000ms);
    ASSERT_TRUE(second.has_value());
    ASSERT_EQ(second->type, HostEventType::Handshake);
    ASSERT_TRUE(server->reject_handshake(second->conn, 403).has_value());
    EXPECT_EQ(refused.read_http_head().rfind("HTTP/1.1 403 ", 0), 0U);
}

TEST(EmbeddedServer, WithoutTheHookTheHostIsNeverAsked) {
    const auto server = started();
    TcpClient client{server->port()};
    ASSERT_TRUE(client.upgrade());
    const auto first = server->poll(3000ms);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->type, HostEventType::Open);
    EXPECT_EQ(server->accept_handshake(first->conn, std::nullopt).error().code, ErrorCode::Closing);
}

TEST(EmbeddedServer, AQuestionNobodyAnswersIsA503AfterTheConfiguredWait) {
    EmbeddedConfig config = base_config();
    ASSERT_TRUE(config.set("handshake_hook", "1").has_value());
    ASSERT_TRUE(config.set("handshake_decision_timeout_ms", "200").has_value());
    const auto server = started(config);
    TcpClient client{server->port()};
    ASSERT_TRUE(client.send(TcpClient::upgrade_request()));
    const auto question = server->poll(3000ms);
    ASSERT_TRUE(question.has_value());
    EXPECT_EQ(client.read_http_head().rfind("HTTP/1.1 503 ", 0), 0U);
}

TEST(EmbeddedServer, ClosingTheHandshakeIntakeRefusesNewUpgradesAtOnce) {
    EmbeddedConfig config = base_config();
    ASSERT_TRUE(config.set("handshake_hook", "true").has_value());
    const auto server = started(config);
    server->set_handshake_intake(false);
    TcpClient client{server->port()};
    ASSERT_TRUE(client.send(TcpClient::upgrade_request()));
    EXPECT_EQ(client.read_http_head().rfind("HTTP/1.1 503 ", 0), 0U);
    EXPECT_FALSE(server->poll(100ms).has_value());
}

}  // namespace

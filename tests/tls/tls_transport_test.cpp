// SPDX-License-Identifier: Apache-2.0
#include "tls/tls_transport.hpp"

#include "support/fake_connection_env.hpp"
#include "support/fakes.hpp"
#include "support/tls_fixtures.hpp"
#include "support/tls_peer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "tls/server_context.hpp"

namespace {

using wsrv::ErrorCode;
using wsrv::TlsServerContext;
using wsrv::TlsTransport;
using wsrv::test::FakeTransport;
using wsrv::test::TlsTestClient;

constexpr std::size_t kWholeBurst = static_cast<std::size_t>(-1);
namespace fs = std::filesystem;
constexpr fs::perms kPublic =
    fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read;
constexpr fs::perms kPrivate = fs::perms::owner_read | fs::perms::owner_write;
constexpr std::size_t kRoomy = std::size_t{1} << 24;  // "the kernel buffer has plenty of room"

/// A server-side TlsTransport over a FakeTransport, and a client the test steers byte by byte.
class Rig {
public:
    explicit Rig(int client_max_version = 0) : client_{client_max_version} {
        const auto pem = wsrv::test::make_pem({});
        wsrv::TlsConfig config;
        config.certificate_chain_file = dir_.write("chain.pem", pem.certificate, kPublic);
        config.private_key_file = dir_.write("key.pem", pem.key, kPrivate);
        auto loaded = TlsServerContext::load(config, logger_);
        EXPECT_TRUE(loaded.has_value());
        context_ = std::move(*loaded);
        auto inner = std::make_unique<FakeTransport>();
        fake = inner.get();
        auto created = TlsTransport::create(std::move(inner), context_->current());
        EXPECT_TRUE(created.has_value());
        tls = std::move(*created);
    }

    /// Hand `bytes` to the server `chunk` at a time, reading after every slice. The last read's
    /// result is kept in `last`; application bytes the server produced pile up in `received`.
    void send_to_server(const std::vector<std::byte>& bytes, std::size_t chunk = kWholeBurst) {
        for (std::size_t at = 0; at < bytes.size();) {
            const std::size_t n = std::min(chunk, bytes.size() - at);
            fake->feed(wsrv::ByteView{bytes}.subspan(at, n));
            at += n;
            read_once();
        }
        if (bytes.empty()) {
            read_once();
        }
    }

    void read_once() {
        std::array<std::byte, 4096> buffer{};
        last = tls->read(buffer);
        if (last && *last > 0) {
            received.append(wsrv::as_chars(wsrv::ByteView{buffer}.first(*last)));
        }
    }

    /// Move what the server wrote to the client.
    void deliver_to_client() {
        client_.feed(fake->written);
        fake->written.clear();
    }

    /// Run the handshake to the end: both sides complete, proven by one application message.
    bool handshake(std::size_t chunk = kWholeBurst) {
        for (int round = 0; round < 400 && !client_.done(); ++round) {
            client_.handshake();
            send_to_server(client_.take_output(), chunk);
            deliver_to_client();
        }
        if (!client_.done()) {
            return false;
        }
        client_.write("ping");
        send_to_server(client_.take_output(), chunk);
        deliver_to_client();
        client_.read_all();  // post-handshake messages, if any
        return received == "ping";
    }

    TlsTestClient& client() {
        return client_;
    }

    FakeTransport* fake = nullptr;
    std::unique_ptr<TlsTransport> tls;
    wsrv::Result<std::size_t> last{std::size_t{0}};
    std::string received;

private:
    wsrv::test::TempDir dir_;
    wsrv::test::RecordingLogger logger_;
    std::unique_ptr<TlsServerContext> context_;
    TlsTestClient client_;
};

wsrv::ErrorCode error_of(const wsrv::Result<std::size_t>& result) {
    EXPECT_FALSE(result.has_value());
    return result ? ErrorCode::InvalidArgument : result.error().code;
}

// ---- the handshake (EC-T01) ---------------------------------------------------------------------

TEST(TlsTransport, TheHandshakeCompletesInOneBurstPerFlight) {
    Rig rig;
    EXPECT_TRUE(rig.handshake());
    EXPECT_EQ(rig.client().version(), TLS1_3_VERSION);
}

TEST(TlsTransport, TheHandshakeCompletesWhenTheBytesArriveOneAtATime) {
    Rig rig;
    EXPECT_TRUE(rig.handshake(1));
}

TEST(TlsTransport, TheHandshakeCompletesWithTls12) {
    Rig rig{TLS1_2_VERSION};
    EXPECT_TRUE(rig.handshake(7));
    EXPECT_EQ(rig.client().version(), TLS1_2_VERSION);
}

TEST(TlsTransport, ReadAndWriteWaitUntilTheHandshakeIsDone) {
    Rig rig;
    std::array<std::byte, 16> buffer{};
    EXPECT_EQ(error_of(rig.tls->read(buffer)), ErrorCode::WouldBlock);
    const std::array<std::byte, 4> data{};
    EXPECT_EQ(error_of(rig.tls->write(data)), ErrorCode::WouldBlock);
    EXPECT_TRUE(rig.fake->written.empty());  // nothing to say before the ClientHello arrived
}

TEST(TlsTransport, DataFlowsInBothDirections) {
    Rig rig;
    ASSERT_TRUE(rig.handshake());
    const auto sent = rig.tls->write(wsrv::as_bytes(std::string_view{"hello client"}));
    ASSERT_TRUE(sent.has_value());
    EXPECT_EQ(*sent, 12U);
    rig.deliver_to_client();
    EXPECT_EQ(rig.client().read_all(), "hello client");
}

TEST(TlsTransport, AHandshakeThatCannotWriteAsksToBeWokenWhenWritable) {
    Rig rig;
    rig.fake->set_write_capacity(0);  // the kernel buffer is full
    rig.client().handshake();
    rig.send_to_server(rig.client().take_output());
    EXPECT_EQ(error_of(rig.last), ErrorCode::WouldBlock);
    EXPECT_TRUE(rig.tls->wants_write());  // else edge-triggered epoll would never wake us again
    EXPECT_TRUE(rig.fake->written.empty());

    rig.fake->set_write_capacity(kRoomy);  // "writable" arrives; the connection calls read() again
    rig.read_once();
    EXPECT_EQ(error_of(rig.last), ErrorCode::WouldBlock);
    EXPECT_FALSE(rig.tls->wants_write());
    EXPECT_FALSE(rig.fake->written.empty());
}

TEST(TlsTransport, ADribblingKernelBufferStillCompletesTheHandshake) {
    Rig rig;
    rig.fake->set_write_capacity(1);  // one byte of room per call; the test tops it up
    for (int round = 0; round < 4000 && !rig.client().done(); ++round) {
        rig.client().handshake();
        rig.send_to_server(rig.client().take_output(), 3);
        rig.fake->set_write_capacity(1);
        rig.read_once();
        rig.deliver_to_client();
    }
    EXPECT_TRUE(rig.client().done());
}

// ---- writing (EC-T06) ---------------------------------------------------------------------------

TEST(TlsTransport, ALargeWriteSurvivesAKernelBufferThatFillsAndEmpties) {
    Rig rig;
    ASSERT_TRUE(rig.handshake());
    std::string payload(200000, 'x');
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>('a' + (i * 7 + i / 13) % 26);  // not all the same byte
    }
    const std::array<std::size_t, 5> capacities{0, 1, 700, 5000, 40000};
    std::string at_client;
    std::size_t sent = 0;
    for (std::size_t round = 0; sent < payload.size(); ++round) {
        ASSERT_LT(round, 20000U) << "no progress";
        const auto n = rig.tls->write(wsrv::as_bytes(std::string_view{payload}.substr(sent)));
        if (n) {
            sent += *n;
        } else {
            ASSERT_EQ(n.error().code, ErrorCode::WouldBlock);
        }
        rig.fake->set_write_capacity(capacities[round % capacities.size()]);
        rig.read_once();  // "writable" event: let the transport continue flushing
        rig.deliver_to_client();
        at_client += rig.client().read_all();
    }
    rig.fake->set_write_capacity(kRoomy);
    rig.read_once();
    rig.deliver_to_client();
    at_client += rig.client().read_all();
    EXPECT_EQ(at_client, payload);
}

TEST(TlsTransport, OutputIsBoundedWhenThePeerStopsReading) {
    Rig rig;
    ASSERT_TRUE(rig.handshake());
    rig.fake->set_write_capacity(0);
    const std::vector<std::byte> block(16384, std::byte{0x41});
    std::size_t accepted = 0;
    for (int i = 0; i < 1000; ++i) {
        const auto n = rig.tls->write(block);
        if (!n) {
            EXPECT_EQ(n.error().code, ErrorCode::WouldBlock);
            break;
        }
        accepted += *n;
    }
    EXPECT_GT(accepted, 0U);
    EXPECT_LE(accepted, std::size_t{100000});  // a cap, not "until memory runs out"
    EXPECT_TRUE(rig.tls->wants_write());
}

TEST(TlsTransport, AnEmptyWriteIsAcceptedAndSendsNothing) {
    Rig rig;
    ASSERT_TRUE(rig.handshake());
    rig.fake->written.clear();
    const auto n = rig.tls->write(wsrv::ByteView{});
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(*n, 0U);
    EXPECT_TRUE(rig.fake->written.empty());
}

// ---- reading ------------------------------------------------------------------------------------

TEST(TlsTransport, BufferedPlaintextIsReadWithoutNewBytesFromTheSocket) {
    // The kernel will not announce data the transport already took: buffered_input() is how the
    // connection knows to read again by itself (ADR-0019 section 3).
    Rig rig;
    ASSERT_TRUE(rig.handshake());
    rig.received.clear();
    rig.client().write("aaaa");
    rig.client().write("bbbb");
    rig.fake->feed(rig.client().take_output());

    std::string got;
    std::array<std::byte, 3> small{};
    auto first = rig.tls->read(small);
    ASSERT_TRUE(first.has_value());
    got.append(wsrv::as_chars(wsrv::ByteView{small}.first(*first)));
    EXPECT_TRUE(rig.tls->buffered_input());
    while (true) {
        const auto n = rig.tls->read(small);
        if (!n) {
            EXPECT_EQ(n.error().code, ErrorCode::WouldBlock);
            break;
        }
        got.append(wsrv::as_chars(wsrv::ByteView{small}.first(*n)));
    }
    EXPECT_EQ(got, "aaaabbbb");
    EXPECT_FALSE(rig.tls->buffered_input());
}

// ---- failures -----------------------------------------------------------------------------------

TEST(TlsTransport, PlainHttpOnATlsPortIsATlsFailure) {  // EC-T02
    Rig rig;
    rig.fake->feed(std::string_view{"GET / HTTP/1.1\r\nHost: example\r\n\r\n"});
    rig.read_once();
    EXPECT_EQ(error_of(rig.last), ErrorCode::TlsFailure);
    EXPECT_TRUE(rig.last.error().context.starts_with("tls:"));
}

TEST(TlsTransport, GarbageInsteadOfAClientHelloIsATlsFailure) {
    Rig rig;
    rig.fake->feed(std::vector<std::byte>(300, std::byte{0xFF}));
    rig.read_once();
    EXPECT_EQ(error_of(rig.last), ErrorCode::TlsFailure);
}

TEST(TlsTransport, ACorruptedRecordIsATlsFailure) {
    Rig rig;
    ASSERT_TRUE(rig.handshake());
    rig.client().write("hello");
    auto record = rig.client().take_output();
    ASSERT_GT(record.size(), 12U);
    record[record.size() / 2] ^= std::byte{0x01};
    rig.send_to_server(record);
    EXPECT_EQ(error_of(rig.last), ErrorCode::TlsFailure);
}

TEST(TlsTransport, ATransportThatFailedStaysFailed) {
    Rig rig;
    rig.fake->feed(std::vector<std::byte>(300, std::byte{0xFF}));
    rig.read_once();
    ASSERT_EQ(error_of(rig.last), ErrorCode::TlsFailure);
    rig.read_once();
    EXPECT_EQ(error_of(rig.last), ErrorCode::TlsFailure);
    const std::array<std::byte, 1> one{};
    EXPECT_NE(error_of(rig.tls->write(one)), ErrorCode::WouldBlock);
}

TEST(TlsTransport, CloseNotifyIsACleanEndOfStream) {  // EC-T04
    Rig rig;
    ASSERT_TRUE(rig.handshake());
    rig.client().shutdown();
    rig.send_to_server(rig.client().take_output());
    ASSERT_TRUE(rig.last.has_value());
    EXPECT_EQ(*rig.last, 0U);  // 0 = EOF, a clean close
    rig.read_once();
    ASSERT_TRUE(rig.last.has_value());
    EXPECT_EQ(*rig.last, 0U);  // and it stays that way
}

TEST(TlsTransport, ACutConnectionAfterTheHandshakeIsConnectionClosed) {  // EC-T04
    Rig rig;
    ASSERT_TRUE(rig.handshake());
    rig.fake->feed_eof();
    rig.read_once();
    EXPECT_EQ(error_of(rig.last), ErrorCode::ConnectionClosed);
}

TEST(TlsTransport, ACutConnectionDuringTheHandshakeIsConnectionClosed) {
    Rig rig;
    rig.client().handshake();
    auto hello = rig.client().take_output();
    hello.resize(hello.size() / 2);
    rig.send_to_server(hello);
    rig.fake->feed_eof();
    rig.read_once();
    EXPECT_EQ(error_of(rig.last), ErrorCode::ConnectionClosed);
}

TEST(TlsTransport, AnErrorOfTheDescriptorPassesThrough) {
    Rig rig;
    ASSERT_TRUE(rig.handshake());
    rig.fake->fail_reads(ErrorCode::Io);
    rig.read_once();
    EXPECT_EQ(error_of(rig.last), ErrorCode::Io);
}

// EC-T05. TLS 1.2 is the only version that has renegotiation. OpenSSL 3 already refuses a client's
// request by default; our explicit SSL_OP_NO_RENEGOTIATION (pinned in server_context_test) keeps
// that true for any OpenSSL build and configuration, so this test checks the behaviour end to end.
TEST(TlsTransport, RenegotiationIsRefused) {
    Rig rig{TLS1_2_VERSION};
    ASSERT_TRUE(rig.handshake());
    rig.received.clear();
    rig.client().renegotiate();
    const auto request = rig.client().take_output();
    ASSERT_GT(request.size(), 100U) << "the client sent no ClientHello";
    rig.send_to_server(request);
    // Either the server keeps the connection and ignores the request, or it ends it: it never
    // delivers application data from it and never runs a second handshake.
    if (rig.last) {
        EXPECT_EQ(*rig.last, 0U);
    } else {
        EXPECT_TRUE(rig.last.error().code == ErrorCode::WouldBlock ||
                    rig.last.error().code == ErrorCode::TlsFailure);
    }
    // What the server answered is the proof: a ServerHello flight (certificate and all) would be
    // hundreds of bytes; a refusal is at most a short alert record (content type 21).
    EXPECT_LT(rig.fake->written.size(), 64U);
    if (!rig.fake->written.empty()) {
        EXPECT_EQ(rig.fake->written.front(), std::byte{21});
    }
    EXPECT_TRUE(rig.received.empty());
}

TEST(TlsTransport, APeerThatForcesAnswersAndNeverReadsThemIsCutOff) {
    // The output cap must also hold on the read path: every KeyUpdate request makes the server
    // queue a KeyUpdate of its own. A peer that sends thousands and reads none must not make the
    // queue grow without bound (it fails the stream instead).
    Rig rig;
    ASSERT_TRUE(rig.handshake());
    rig.fake->set_write_capacity(0);  // the peer's socket never drains
    std::size_t rounds = 0;
    for (;
         rounds < 40000 && (rig.last.has_value() || rig.last.error().code == ErrorCode::WouldBlock);
         ++rounds) {
        ASSERT_TRUE(rig.client().key_update_request());
        rig.client().write("k");
        rig.send_to_server(rig.client().take_output());
    }
    ASSERT_LT(rounds, 40000U) << "the queue of unsent answers was never bounded";
    EXPECT_EQ(error_of(rig.last), ErrorCode::TlsFailure);
}

// ---- shutdown and close -------------------------------------------------------------------------

TEST(TlsTransport, ShutdownWriteSendsCloseNotifyThenTheFin) {
    Rig rig;
    ASSERT_TRUE(rig.handshake());
    rig.tls->shutdown_write();
    EXPECT_TRUE(rig.fake->shut_down);
    rig.deliver_to_client();
    rig.client().read_all();
    EXPECT_TRUE(rig.client().closed());
}

TEST(TlsTransport, CloseReleasesTheDescriptorAndIsIdempotent) {
    Rig rig;
    ASSERT_TRUE(rig.handshake());
    EXPECT_EQ(rig.tls->fd(), 42);  // the inner transport's descriptor
    rig.tls->close();
    rig.tls->close();
    EXPECT_TRUE(rig.fake->closed);
    EXPECT_EQ(rig.tls->fd(), -1);
    rig.read_once();
    EXPECT_EQ(error_of(rig.last), ErrorCode::ConnectionClosed);
    const std::array<std::byte, 1> one{};
    EXPECT_EQ(error_of(rig.tls->write(one)), ErrorCode::ConnectionClosed);
    EXPECT_FALSE(rig.tls->wants_write());
    EXPECT_FALSE(rig.tls->buffered_input());
    rig.tls->shutdown_write();  // harmless after close
}

TEST(TlsTransport, CreateRefusesMissingParts) {
    wsrv::test::RecordingLogger logger;
    EXPECT_FALSE(TlsTransport::create(nullptr, nullptr).has_value());
    EXPECT_FALSE(TlsTransport::create(std::make_unique<FakeTransport>(), nullptr).has_value());
}

}  // namespace

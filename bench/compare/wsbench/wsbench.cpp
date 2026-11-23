// SPDX-License-Identifier: Apache-2.0
//
// wsbench: a standalone WebSocket load generator for bench/compare. It speaks RFC 6455 to any
// server and shares no code with wsrv, so the same client measures every contender the same way.
//
//   wsbench echo   --host H --port P --connections 256 --threads 2 --size 64 --seconds 10
//   wsbench idle   --host H --port P --connections 10000 --seconds 10
//   wsbench fanout --host H --port P --connections 1000 --rounds 30 --size 128 --trigger ws
//
// echo    closed loop: every connection sends one message, waits for the echo, sends the next.
// idle    opens N connections, holds them, reports how many are established (the runner reads the
//         server's memory while it holds).
// fanout  N subscribers; one publish per round reaches all of them; the time from the trigger to
//         the last subscriber is the figure (also to the first).
// Triggers: ws (a "bcast:" text message from a publisher connection, the server app broadcasts it),
// http (POST to --trigger-url, with --trigger-header lines), pusher (a signed Pusher REST event).
// Subscribe: none (the server subscribes on connect) or pusher (pusher:subscribe on --channel).
//
// Output: one JSON object on stdout. Not part of the product; kept simple on purpose.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t now_us() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch())
            .count());
}

struct Options {
    std::string mode;
    std::string host = "127.0.0.1";
    std::string port = "8080";
    std::string path = "/";
    std::size_t connections = 100;
    std::size_t threads = 1;
    std::size_t size = 64;
    std::size_t seconds = 10;
    std::size_t warmup = 2;
    std::size_t rounds = 20;
    std::size_t ramp = 2000;  // new connections per second, all threads together
    std::string subscribe = "none";
    std::string trigger = "ws";
    std::string trigger_url;
    std::vector<std::string> trigger_headers;
    std::string pusher_key;
    std::string pusher_secret;
    std::string pusher_app_id;
    std::string channel = "bench";
};

[[noreturn]] void die(const std::string& message) {
    std::cerr << "wsbench: " << message << '\n';
    std::exit(2);
}

std::size_t to_size(const std::string& text, const std::string& name) {
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
    if (text.empty() || end == nullptr || *end != '\0' || value == 0) {
        die("bad number for " + name);
    }
    return static_cast<std::size_t>(value);
}

Options parse(int argc, char** argv) {
    if (argc < 2) {
        die("usage: wsbench echo|idle|fanout --host H --port P [options] (see the header of wsbench.cpp)");
    }
    Options o;
    o.mode = argv[1];
    if (o.mode != "echo" && o.mode != "idle" && o.mode != "fanout") {
        die("the mode is echo, idle or fanout");
    }
    for (int i = 2; i < argc; i += 2) {
        const std::string name = argv[i];
        if (i + 1 >= argc) {
            die("missing value for " + name);
        }
        const std::string value = argv[i + 1];
        if (name == "--host") o.host = value;
        else if (name == "--port") o.port = value;
        else if (name == "--path") o.path = value;
        else if (name == "--connections") o.connections = to_size(value, name);
        else if (name == "--threads") o.threads = to_size(value, name);
        else if (name == "--size") o.size = to_size(value, name);
        else if (name == "--seconds") o.seconds = to_size(value, name);
        else if (name == "--warmup") o.warmup = to_size(value, name);
        else if (name == "--rounds") o.rounds = to_size(value, name);
        else if (name == "--ramp") o.ramp = to_size(value, name);
        else if (name == "--subscribe") o.subscribe = value;
        else if (name == "--trigger") o.trigger = value;
        else if (name == "--trigger-url") o.trigger_url = value;
        else if (name == "--trigger-header") o.trigger_headers.push_back(value);
        else if (name == "--pusher-key") o.pusher_key = value;
        else if (name == "--pusher-secret") o.pusher_secret = value;
        else if (name == "--pusher-app-id") o.pusher_app_id = value;
        else if (name == "--channel") o.channel = value;
        else die("unknown option " + name);
    }
    if (o.subscribe != "none" && o.subscribe != "pusher") die("--subscribe is none or pusher");
    if (o.trigger != "ws" && o.trigger != "http" && o.trigger != "pusher") die("--trigger is ws, http or pusher");
    if (o.threads > o.connections) o.threads = o.connections;
    return o;
}

// ---- small helpers -------------------------------------------------------------------------

std::string base64(std::string_view in) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < in.size(); i += 3) {
        const std::size_t left = in.size() - i;
        std::uint32_t v = static_cast<std::uint8_t>(in[i]) << 16;
        if (left > 1) v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[i + 1])) << 8;
        if (left > 2) v |= static_cast<std::uint8_t>(in[i + 2]);
        out.push_back(kAlphabet[(v >> 18) & 63]);
        out.push_back(kAlphabet[(v >> 12) & 63]);
        out.push_back(left > 1 ? kAlphabet[(v >> 6) & 63] : '=');
        out.push_back(left > 2 ? kAlphabet[v & 63] : '=');
    }
    return out;
}

std::string hex(const unsigned char* data, std::size_t size) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(kDigits[data[i] >> 4]);
        out.push_back(kDigits[data[i] & 15]);
    }
    return out;
}

std::string md5_hex(std::string_view body) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    EVP_Digest(body.data(), body.size(), digest, &size, EVP_md5(), nullptr);
    return hex(digest, size);
}

std::string hmac_sha256_hex(std::string_view key, std::string_view message) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(message.data()), message.size(), digest, &size);
    return hex(digest, size);
}

std::uint32_t next_random(std::uint32_t& state) {  // xorshift32: masking keys need not be secret here
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

std::string encode_frame(std::uint8_t opcode, std::string_view payload, std::uint32_t& rng) {
    std::string frame;
    frame.push_back(static_cast<char>(0x80 | opcode));
    if (payload.size() < 126) {
        frame.push_back(static_cast<char>(0x80 | payload.size()));
    } else if (payload.size() < 65536) {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>(payload.size() >> 8));
        frame.push_back(static_cast<char>(payload.size() & 0xFF));
    } else {
        frame.push_back(static_cast<char>(0x80 | 127));
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(static_cast<char>((static_cast<std::uint64_t>(payload.size()) >> shift) & 0xFF));
        }
    }
    const std::uint32_t mask = next_random(rng);
    const char key[4] = {static_cast<char>(mask), static_cast<char>(mask >> 8),
                         static_cast<char>(mask >> 16), static_cast<char>(mask >> 24)};
    frame.append(key, 4);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        frame.push_back(static_cast<char>(payload[i] ^ key[i % 4]));
    }
    return frame;
}

struct Target {
    sockaddr_storage addr{};
    socklen_t length = 0;
};

Target resolve(const std::string& host, const std::string& port) {
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &found) != 0 || found == nullptr) {
        die("cannot resolve " + host + ":" + port);
    }
    Target target;
    std::memcpy(&target.addr, found->ai_addr, found->ai_addrlen);
    target.length = found->ai_addrlen;
    ::freeaddrinfo(found);
    return target;
}

int open_socket(const Target& target) {
    const int fd = ::socket(target.addr.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&target.addr), target.length) != 0 &&
        errno != EINPROGRESS) {
        ::close(fd);
        return -1;
    }
    return fd;
}

double percentile(std::vector<std::uint64_t>& values, double p) {
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t index = static_cast<std::size_t>(p * static_cast<double>(values.size() - 1) + 0.5);
    return static_cast<double>(values[std::min(index, values.size() - 1)]);
}

// ---- the connection loop -------------------------------------------------------------------

struct Conn {
    enum class State { Connecting, Handshake, Open, Dead };
    int fd = -1;
    State state = State::Connecting;
    std::string in;
    std::size_t in_off = 0;
    std::string out;
    std::string fragments;
    std::uint64_t born_us = 0;
    std::uint64_t sent_us = 0;
    std::uint64_t last_round = 0;
    bool ready = false;  // established (and subscribed, when asked)
    bool publisher = false;
};

class Loop;

class Handler {
public:
    virtual ~Handler() = default;
    virtual void on_open(Loop& loop, Conn& conn) = 0;
    virtual void on_message(Loop& loop, Conn& conn, std::string_view payload) = 0;
    virtual void on_dead(Loop& loop, Conn& conn) = 0;
};

class Loop {
public:
    Loop(const Options& options, const Target& target, Handler& handler, std::uint32_t seed)
        : options_{options}, target_{target}, handler_{handler}, rng_{seed | 1U},
          epoll_{::epoll_create1(EPOLL_CLOEXEC)} {
        if (epoll_ < 0) {
            die("epoll_create1 failed");
        }
    }
    ~Loop() {
        for (auto& conn : conns_) {
            if (conn->fd >= 0) {
                ::close(conn->fd);
            }
        }
        ::close(epoll_);
    }
    Loop(const Loop&) = delete;
    Loop& operator=(const Loop&) = delete;

    Conn* add(bool publisher) {
        auto conn = std::make_unique<Conn>();
        conn->fd = open_socket(target_);
        conn->publisher = publisher;
        conn->born_us = now_us();
        Conn* raw = conn.get();
        conns_.push_back(std::move(conn));
        if (raw->fd < 0) {
            raw->state = Conn::State::Dead;
            ++failed_;
            return raw;
        }
        epoll_event event{};
        event.events = EPOLLIN | EPOLLOUT | EPOLLET;
        event.data.ptr = raw;
        ::epoll_ctl(epoll_, EPOLL_CTL_ADD, raw->fd, &event);
        return raw;
    }

    void step(int timeout_ms) {
        epoll_event events[256];
        const int count = ::epoll_wait(epoll_, events, 256, timeout_ms);
        for (int i = 0; i < count; ++i) {
            auto* conn = static_cast<Conn*>(events[i].data.ptr);
            if (conn->state == Conn::State::Dead) {
                continue;
            }
            if ((events[i].events & (EPOLLERR | EPOLLHUP)) != 0 && (events[i].events & EPOLLIN) == 0) {
                kill(*conn);
                continue;
            }
            if ((events[i].events & EPOLLOUT) != 0) {
                on_writable(*conn);
            }
            if (conn->state != Conn::State::Dead && (events[i].events & EPOLLIN) != 0) {
                on_readable(*conn);
            }
        }
    }

    void send_text(Conn& conn, std::string_view text) {
        send_frame(conn, 0x1, text);
    }

    std::uint64_t failed() const { return failed_; }
    std::size_t open_count() const {
        std::size_t n = 0;
        for (const auto& conn : conns_) {
            n += conn->state == Conn::State::Open ? 1 : 0;
        }
        return n;
    }

private:
    void kill(Conn& conn) {
        if (conn.state == Conn::State::Dead) {
            return;
        }
        const bool was_ready = conn.ready;
        conn.state = Conn::State::Dead;
        ::close(conn.fd);
        conn.fd = -1;
        if (!was_ready) {
            ++failed_;
        }
        handler_.on_dead(*this, conn);
    }

    void send_frame(Conn& conn, std::uint8_t opcode, std::string_view payload) {
        if (conn.state != Conn::State::Open) {
            return;
        }
        conn.out += encode_frame(opcode, payload, rng_);
        flush(conn);
    }

    void flush(Conn& conn) {
        std::size_t done = 0;
        while (done < conn.out.size()) {
            const ssize_t n = ::send(conn.fd, conn.out.data() + done, conn.out.size() - done, MSG_NOSIGNAL);
            if (n > 0) {
                done += static_cast<std::size_t>(n);
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;  // EPOLLOUT (edge) calls flush again
            } else {
                conn.out.erase(0, done);
                kill(conn);
                return;
            }
        }
        conn.out.erase(0, done);
    }

    void on_writable(Conn& conn) {
        if (conn.state == Conn::State::Connecting) {
            int error = 0;
            socklen_t size = sizeof(error);
            ::getsockopt(conn.fd, SOL_SOCKET, SO_ERROR, &error, &size);
            if (error != 0) {
                kill(conn);
                return;
            }
            conn.state = Conn::State::Handshake;
            std::string raw_key(16, '\0');
            for (char& c : raw_key) c = static_cast<char>(next_random(rng_) & 0xFF);
            conn.out = "GET " + options_.path + " HTTP/1.1\r\nHost: " + options_.host + ":" + options_.port +
                       "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + base64(raw_key) +
                       "\r\nSec-WebSocket-Version: 13\r\n\r\n";
        }
        flush(conn);
    }

    void on_readable(Conn& conn) {
        char buffer[65536];
        while (conn.state != Conn::State::Dead) {
            const ssize_t n = ::recv(conn.fd, buffer, sizeof(buffer), 0);
            if (n > 0) {
                conn.in.append(buffer, static_cast<std::size_t>(n));
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            kill(conn);  // 0: the server closed; <0: an error
            if (conn.state == Conn::State::Dead && conn.in.empty()) {
                return;
            }
            break;
        }
        if (conn.state == Conn::State::Handshake) {
            finish_handshake(conn);
        }
        if (conn.state == Conn::State::Open) {
            parse_frames(conn);
        }
        if (conn.in_off > 0) {
            conn.in.erase(0, conn.in_off);
            conn.in_off = 0;
        }
    }

    void finish_handshake(Conn& conn) {
        const std::size_t end = conn.in.find("\r\n\r\n");
        if (end == std::string::npos) {
            return;
        }
        if (conn.in.compare(0, 12, "HTTP/1.1 101") != 0) {
            kill(conn);
            return;
        }
        conn.in_off = end + 4;
        conn.state = Conn::State::Open;
        handler_.on_open(*this, conn);
    }

    void parse_frames(Conn& conn) {
        while (conn.state == Conn::State::Open) {
            const std::size_t avail = conn.in.size() - conn.in_off;
            if (avail < 2) {
                return;
            }
            const auto* p = reinterpret_cast<const std::uint8_t*>(conn.in.data()) + conn.in_off;
            const bool fin = (p[0] & 0x80) != 0;
            const std::uint8_t opcode = p[0] & 0x0F;
            std::uint64_t length = p[1] & 0x7F;
            std::size_t header = 2;
            if ((p[1] & 0x80) != 0) {  // a server must not mask (RFC 6455 section 5.1)
                kill(conn);
                return;
            }
            if (length == 126) {
                if (avail < 4) return;
                length = (static_cast<std::uint64_t>(p[2]) << 8) | p[3];
                header = 4;
            } else if (length == 127) {
                if (avail < 10) return;
                length = 0;
                for (int i = 0; i < 8; ++i) length = (length << 8) | p[2 + i];
                header = 10;
            }
            if (length > (std::uint64_t{16} << 20)) {
                kill(conn);
                return;
            }
            if (avail < header + length) {
                return;
            }
            const std::string_view payload{conn.in.data() + conn.in_off + header, static_cast<std::size_t>(length)};
            conn.in_off += header + static_cast<std::size_t>(length);
            if (opcode == 0x8) {
                kill(conn);
                return;
            }
            if (opcode == 0x9) {
                send_frame(conn, 0xA, payload);
            } else if (opcode == 0x1 || opcode == 0x2 || opcode == 0x0) {
                if (fin && conn.fragments.empty()) {
                    handler_.on_message(*this, conn, payload);
                } else {
                    conn.fragments.append(payload);
                    if (fin) {
                        const std::string whole = std::move(conn.fragments);
                        conn.fragments.clear();
                        handler_.on_message(*this, conn, whole);
                    }
                }
            }
        }
    }

    const Options& options_;
    const Target& target_;
    Handler& handler_;
    std::uint32_t rng_;
    int epoll_;
    std::vector<std::unique_ptr<Conn>> conns_;
    std::uint64_t failed_ = 0;
};

bool contains(std::string_view text, std::string_view needle) {
    return text.find(needle) != std::string_view::npos;
}

// Pusher protocol pieces (Reverb speaks it): connection_established, subscribe, ping/pong.
void pusher_on_message(Loop& loop, Conn& conn, std::string_view payload, const Options& options) {
    if (contains(payload, "pusher:connection_established")) {
        loop.send_text(conn, "{\"event\":\"pusher:subscribe\",\"data\":{\"channel\":\"" + options.channel + "\"}}");
    } else if (contains(payload, "subscription_succeeded")) {
        conn.ready = true;
    } else if (contains(payload, "pusher:ping")) {
        loop.send_text(conn, "{\"event\":\"pusher:pong\",\"data\":{}}");
    }
}

// ---- echo ----------------------------------------------------------------------------------

struct EchoResult {
    std::uint64_t messages = 0;
    std::uint64_t failures = 0;
    std::vector<std::uint64_t> latency_us;
};

class EchoHandler final : public Handler {
public:
    EchoHandler(const Options& options, const std::atomic<bool>& measuring, EchoResult& result)
        : payload_(options.size, 'e'), measuring_{measuring}, result_{result} {}
    void on_open(Loop& loop, Conn& conn) override {
        conn.ready = true;
        conn.sent_us = now_us();
        loop.send_text(conn, payload_);
    }
    void on_message(Loop& loop, Conn& conn, std::string_view payload) override {
        const std::uint64_t done = now_us();
        if (payload != payload_) {
            ++result_.failures;
            return;
        }
        if (measuring_.load(std::memory_order_relaxed)) {
            ++result_.messages;
            result_.latency_us.push_back(done - conn.sent_us);
        }
        conn.sent_us = now_us();
        loop.send_text(conn, payload_);
    }
    void on_dead(Loop&, Conn&) override { ++result_.failures; }

private:
    std::string payload_;
    const std::atomic<bool>& measuring_;
    EchoResult& result_;
};

int run_echo(const Options& options, const Target& target) {
    std::atomic<bool> measuring{false};
    std::atomic<bool> stop{false};
    std::vector<EchoResult> results(options.threads);
    std::vector<std::thread> threads;
    for (std::size_t t = 0; t < options.threads; ++t) {
        const std::size_t mine = options.connections / options.threads + (t < options.connections % options.threads ? 1 : 0);
        threads.emplace_back([&, t, mine] {
            EchoHandler handler{options, measuring, results[t]};
            Loop loop{options, target, handler, static_cast<std::uint32_t>(0x9E3779B9U * (t + 1))};
            const std::size_t per_second = std::max<std::size_t>(1, options.ramp / options.threads);
            std::size_t opened = 0;
            std::uint64_t last_tick = now_us();
            while (!stop.load(std::memory_order_relaxed)) {
                const std::uint64_t now = now_us();
                if (opened < mine && now - last_tick >= 10000) {
                    const std::size_t batch = std::max<std::size_t>(1, per_second / 100);
                    for (std::size_t i = 0; i < batch && opened < mine; ++i, ++opened) loop.add(false);
                    last_tick = now;
                }
                loop.step(5);
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(options.warmup));
    measuring.store(true);
    const std::uint64_t begin = now_us();
    std::this_thread::sleep_for(std::chrono::seconds(options.seconds));
    measuring.store(false);
    const std::uint64_t end = now_us();
    stop.store(true);
    for (auto& thread : threads) thread.join();

    std::vector<std::uint64_t> all;
    std::uint64_t messages = 0;
    std::uint64_t failures = 0;
    for (auto& r : results) {
        messages += r.messages;
        failures += r.failures;
        all.insert(all.end(), r.latency_us.begin(), r.latency_us.end());
    }
    const double seconds = static_cast<double>(end - begin) / 1e6;
    std::printf("{\"mode\":\"echo\",\"connections\":%zu,\"threads\":%zu,\"size\":%zu,\"seconds\":%.2f,"
                "\"messages\":%llu,\"msgs_per_sec\":%.0f,\"failures\":%llu,"
                "\"p50_us\":%.0f,\"p90_us\":%.0f,\"p99_us\":%.0f,\"p999_us\":%.0f,\"max_us\":%.0f}\n",
                options.connections, options.threads, options.size, seconds,
                static_cast<unsigned long long>(messages), static_cast<double>(messages) / seconds,
                static_cast<unsigned long long>(failures), percentile(all, 0.50), percentile(all, 0.90),
                percentile(all, 0.99), percentile(all, 0.999), percentile(all, 1.0));
    return failures == 0 && messages > 0 ? 0 : 1;
}

// ---- idle ----------------------------------------------------------------------------------

class IdleHandler final : public Handler {
public:
    explicit IdleHandler(const Options& options) : options_{options} {}
    void on_open(Loop&, Conn& conn) override {
        if (options_.subscribe != "pusher") {
            conn.ready = true;
            connect_us.push_back(now_us() - conn.born_us);
        }
    }
    void on_message(Loop& loop, Conn& conn, std::string_view payload) override {
        const bool was = conn.ready;
        if (options_.subscribe == "pusher") {
            if (contains(payload, "pusher:connection_established")) {
                conn.ready = true;  // established is enough for idle; no subscription needed
                connect_us.push_back(now_us() - conn.born_us);
            } else if (contains(payload, "pusher:ping")) {
                loop.send_text(conn, "{\"event\":\"pusher:pong\",\"data\":{}}");
            }
        }
        static_cast<void>(was);
    }
    void on_dead(Loop&, Conn&) override { ++dropped; }
    std::vector<std::uint64_t> connect_us;
    std::uint64_t dropped = 0;

private:
    const Options& options_;
};

int run_idle(const Options& options, const Target& target) {
    IdleHandler handler{options};
    Loop loop{options, target, handler, 12345};
    const std::size_t per_tick = std::max<std::size_t>(1, options.ramp / 100);
    std::size_t opened = 0;
    std::uint64_t last_tick = now_us();
    const std::uint64_t ramp_deadline = now_us() + 120ULL * 1000000ULL;
    while (now_us() < ramp_deadline) {
        const std::uint64_t now = now_us();
        if (opened < options.connections && now - last_tick >= 10000) {
            for (std::size_t i = 0; i < per_tick && opened < options.connections; ++i, ++opened) loop.add(false);
            last_tick = now;
        }
        loop.step(5);
        if (opened == options.connections && handler.connect_us.size() + loop.failed() >= options.connections) {
            break;
        }
    }
    const std::size_t established = handler.connect_us.size();
    std::printf("{\"event\":\"holding\",\"established\":%zu,\"failed\":%llu}\n", established,
                static_cast<unsigned long long>(loop.failed()));
    std::fflush(stdout);
    const std::uint64_t hold_until = now_us() + options.seconds * 1000000ULL;
    while (now_us() < hold_until) {
        loop.step(50);
    }
    std::printf("{\"mode\":\"idle\",\"connections\":%zu,\"established\":%zu,\"failed\":%llu,\"dropped_while_holding\":%llu,"
                "\"connect_p50_us\":%.0f,\"connect_p99_us\":%.0f}\n",
                options.connections, established, static_cast<unsigned long long>(loop.failed()),
                static_cast<unsigned long long>(handler.dropped), percentile(handler.connect_us, 0.50),
                percentile(handler.connect_us, 0.99));
    return established == options.connections && handler.dropped == 0 ? 0 : 1;
}

// ---- fanout --------------------------------------------------------------------------------

struct Url {
    std::string host;
    std::string port = "80";
    std::string path = "/";
};

Url parse_url(const std::string& url) {
    const std::string scheme = "http://";
    if (url.compare(0, scheme.size(), scheme) != 0) die("--trigger-url must start with http://");
    std::string rest = url.substr(scheme.size());
    Url out;
    const std::size_t slash = rest.find('/');
    out.path = slash == std::string::npos ? "/" : rest.substr(slash);
    const std::string authority = rest.substr(0, slash);
    const std::size_t colon = authority.find(':');
    out.host = authority.substr(0, colon);
    if (colon != std::string::npos) out.port = authority.substr(colon + 1);
    return out;
}

/// One blocking HTTP POST; returns the status code, or 0 when there was no answer.
int http_post(const Url& url, const Target& target, const std::vector<std::string>& headers,
              const std::string& path, const std::string& body) {
    const int fd = ::socket(target.addr.ss_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 0;
    timeval timeout{10, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&target.addr), target.length) != 0) {
        ::close(fd);
        return 0;
    }
    std::string request = "POST " + path + " HTTP/1.1\r\nHost: " + url.host + ":" + url.port +
                          "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n";
    for (const std::string& header : headers) request += header + "\r\n";
    request += "\r\n" + body;
    std::size_t done = 0;
    while (done < request.size()) {
        const ssize_t n = ::send(fd, request.data() + done, request.size() - done, MSG_NOSIGNAL);
        if (n <= 0) {
            ::close(fd);
            return 0;
        }
        done += static_cast<std::size_t>(n);
    }
    char buffer[512];
    const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
    ::close(fd);
    int status = 0;
    if (n >= 12 && std::strncmp(buffer, "HTTP/1.", 7) == 0) status = std::atoi(buffer + 9);
    return status;
}

class FanoutHandler final : public Handler {
public:
    FanoutHandler(const Options& options, std::string_view marker_holder)
        : options_{options} {
        static_cast<void>(marker_holder);
    }
    void on_open(Loop&, Conn& conn) override {
        if (conn.publisher || options_.subscribe != "pusher") conn.ready = true;
        if (conn.ready) ++ready;
    }
    void on_message(Loop& loop, Conn& conn, std::string_view payload) override {
        if (options_.subscribe == "pusher" && !conn.publisher && !conn.ready) {
            pusher_on_message(loop, conn, payload, options_);
            if (conn.ready) ++ready;
            return;
        }
        if (conn.publisher) return;
        if (options_.subscribe == "pusher" && contains(payload, "pusher:ping")) {
            loop.send_text(conn, "{\"event\":\"pusher:pong\",\"data\":{}}");
            return;
        }
        if (round != 0 && conn.last_round != round && contains(payload, marker)) {
            conn.last_round = round;
            const std::uint64_t now = now_us();
            if (received == 0) first_us = now;
            last_us = now;
            ++received;
        }
    }
    void on_dead(Loop&, Conn& conn) override {
        if (conn.ready) ++dropped;
    }
    std::uint64_t round = 0;
    std::string marker;
    std::size_t ready = 0;
    std::size_t received = 0;
    std::uint64_t first_us = 0;
    std::uint64_t last_us = 0;
    std::uint64_t dropped = 0;

private:
    const Options& options_;
};

int run_fanout(const Options& options, const Target& target) {
    FanoutHandler handler{options, ""};
    Loop loop{options, target, handler, 777};
    const bool ws_trigger = options.trigger == "ws";
    Url trigger_url;
    Target trigger_target;
    if (!ws_trigger) {
        if (options.trigger == "pusher") {
            if (options.pusher_key.empty() || options.pusher_secret.empty() || options.pusher_app_id.empty() ||
                options.trigger_url.empty()) {
                die("--trigger pusher needs --trigger-url, --pusher-key, --pusher-secret and --pusher-app-id");
            }
        } else if (options.trigger_url.empty()) {
            die("--trigger http needs --trigger-url");
        }
        trigger_url = parse_url(options.trigger_url);
        trigger_target = resolve(trigger_url.host, trigger_url.port);
    }

    // 1. connect and subscribe N subscribers (and the publisher for the ws trigger)
    const std::size_t per_tick = std::max<std::size_t>(1, options.ramp / 100);
    std::size_t opened = 0;
    std::uint64_t last_tick = now_us();
    const std::size_t want = options.connections + (ws_trigger ? 1 : 0);
    const std::uint64_t setup_deadline = now_us() + 120ULL * 1000000ULL;
    Conn* publisher = nullptr;
    while (now_us() < setup_deadline && handler.ready < want) {
        const std::uint64_t now = now_us();
        if (opened < options.connections && now - last_tick >= 10000) {
            for (std::size_t i = 0; i < per_tick && opened < options.connections; ++i, ++opened) loop.add(false);
            last_tick = now;
        }
        if (ws_trigger && publisher == nullptr && opened == options.connections) publisher = loop.add(true);
        loop.step(5);
        if (opened == options.connections && (!ws_trigger || publisher != nullptr) && loop.failed() > 0 &&
            handler.ready + loop.failed() >= want) {
            break;
        }
    }
    const std::size_t subscribers = handler.ready - (ws_trigger && publisher != nullptr && publisher->ready ? 1 : 0);
    if (subscribers == 0) {
        std::printf("{\"mode\":\"fanout\",\"error\":\"no subscriber became ready\"}\n");
        return 1;
    }

    // 2. rounds
    std::vector<std::uint64_t> to_last;
    std::vector<std::uint64_t> to_first;
    double delivered_sum = 0;
    std::uint64_t trigger_failures = 0;
    for (std::size_t r = 1; r <= options.rounds; ++r) {
        handler.round = r;
        handler.marker = "R" + std::to_string(r) + ":";
        handler.received = 0;
        const std::string body = handler.marker + std::string(options.size > handler.marker.size() ? options.size - handler.marker.size() : 0, 'f');
        std::thread poster;
        std::atomic<int> http_status{-1};
        const std::uint64_t t0 = now_us();
        if (ws_trigger) {
            loop.send_text(*publisher, "bcast:" + body);
        } else if (options.trigger == "http") {
            poster = std::thread([&] { http_status = http_post(trigger_url, trigger_target, options.trigger_headers, trigger_url.path, body); });
        } else {
            poster = std::thread([&] {
                const std::string json = "{\"name\":\"bench\",\"channel\":\"" + options.channel + "\",\"data\":\"" + body + "\"}";
                const std::string route = "/apps/" + options.pusher_app_id + "/events";
                const std::string query = "auth_key=" + options.pusher_key + "&auth_timestamp=" +
                                          std::to_string(std::time(nullptr)) + "&auth_version=1.0&body_md5=" + md5_hex(json);
                const std::string signature = hmac_sha256_hex(options.pusher_secret, "POST\n" + route + "\n" + query);
                http_status = http_post(trigger_url, trigger_target, {"Content-Type: application/json"},
                                        route + "?" + query + "&auth_signature=" + signature, json);
            });
        }
        const std::uint64_t round_deadline = t0 + 10ULL * 1000000ULL;
        while (handler.received < subscribers && now_us() < round_deadline) loop.step(5);
        if (poster.joinable()) poster.join();
        if (!ws_trigger && (http_status.load() < 200 || http_status.load() > 299)) ++trigger_failures;
        delivered_sum += static_cast<double>(handler.received) / static_cast<double>(subscribers);
        if (handler.received > 0) {
            to_first.push_back(handler.first_us - t0);
            to_last.push_back(handler.last_us - t0);
        }
        handler.first_us = 0;
        const std::uint64_t gap_until = now_us() + 300000;  // let the server settle between rounds
        while (now_us() < gap_until) loop.step(20);
    }
    const double delivered = delivered_sum / static_cast<double>(options.rounds);
    const double last_p50 = percentile(to_last, 0.50);
    std::printf("{\"mode\":\"fanout\",\"subscribers\":%zu,\"rounds\":%zu,\"size\":%zu,\"trigger\":\"%s\","
                "\"delivered_ratio\":%.4f,\"trigger_failures\":%llu,\"dropped\":%llu,"
                "\"first_p50_us\":%.0f,\"last_p50_us\":%.0f,\"last_p90_us\":%.0f,\"last_p99_us\":%.0f,\"last_max_us\":%.0f,"
                "\"deliveries_per_sec_p50\":%.0f}\n",
                subscribers, options.rounds, options.size, options.trigger.c_str(), delivered,
                static_cast<unsigned long long>(trigger_failures), static_cast<unsigned long long>(handler.dropped),
                percentile(to_first, 0.50), last_p50, percentile(to_last, 0.90), percentile(to_last, 0.99),
                percentile(to_last, 1.0), last_p50 > 0 ? static_cast<double>(subscribers) / (last_p50 / 1e6) : 0.0);
    return delivered > 0.999 && trigger_failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    const Options options = parse(argc, argv);
    const Target target = resolve(options.host, options.port);
    if (options.mode == "echo") return run_echo(options, target);
    if (options.mode == "idle") return run_idle(options, target);
    return run_fanout(options, target);
}

// SPDX-License-Identifier: Apache-2.0
#include "net/stream_logger.hpp"

#include "base/log_format.hpp"
#include "support/fake_syscalls.hpp"
#include "support/fakes.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

// ADR-0037 sections 5 and 6: one whole line per write, below the level nothing, a flood held to a
// burst, and a destination that cannot take a line costs a counter and never a stall.

namespace {

using wsrv::LogLevel;
using wsrv::LogRecord;
using wsrv::test::Call;
using namespace std::chrono_literals;

class StreamLoggerTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(::pipe(fds_), 0);
        ASSERT_NE(::fcntl(fds_[0], F_SETFL, O_NONBLOCK), -1);  // the test's reader never blocks
        wall.set(
            std::chrono::system_clock::time_point{std::chrono::milliseconds{1'700'000'000'123}});
    }
    void TearDown() override {
        ::close(fds_[0]);
        ::close(fds_[1]);
    }

    wsrv::StreamLogger make(LogLevel level = LogLevel::Info) {
        return wsrv::StreamLogger{sys, wall, clock, fds_[1], level};
    }
    /// Everything written so far.
    std::string drain() {
        std::string text;
        char buffer[4096];
        for (;;) {
            const ssize_t n = ::read(fds_[0], buffer, sizeof buffer);
            if (n <= 0) {
                return text;
            }
            text.append(buffer, static_cast<std::size_t>(n));
        }
    }
    static std::vector<std::string> lines(const std::string& text) {
        std::vector<std::string> out;
        std::size_t start = 0;
        for (std::size_t end; (end = text.find('\n', start)) != std::string::npos;
             start = end + 1) {
            out.push_back(text.substr(start, end - start));
        }
        return out;
    }

    wsrv::test::FakeSyscalls sys;
    wsrv::test::FakeWallClock wall;
    wsrv::test::FakeClock clock;
    int fds_[2]{-1, -1};
};

}  // namespace

TEST_F(StreamLoggerTest, WritesOneWholeLineWithTheWallClockTimestamp) {
    auto logger = make();
    logger.log(LogRecord{LogLevel::Warn, "slow_consumer", "closing: queue over its limit", 17, ""});
    EXPECT_EQ(drain(),
              "ts=2023-11-14T22:13:20.123Z level=warn event=slow_consumer conn=17 "
              "msg=\"closing: queue over its limit\"\n");
}

TEST_F(StreamLoggerTest, BelowTheConfiguredLevelNothingIsWrittenAndEnabledSaysSo) {
    auto logger = make(LogLevel::Warn);
    EXPECT_FALSE(logger.enabled(LogLevel::Debug));
    EXPECT_FALSE(logger.enabled(LogLevel::Info));
    EXPECT_TRUE(logger.enabled(LogLevel::Warn));
    EXPECT_TRUE(logger.enabled(LogLevel::Error));
    logger.log(LogLevel::Info, "listening", "x");
    logger.log(LogLevel::Debug, "conn_open", "x");
    EXPECT_EQ(drain(), "");
    EXPECT_EQ(sys.calls(Call::Write), 0U) << "filtered before any system call";
    logger.log(LogLevel::Error, "internal_error", "x");
    EXPECT_FALSE(drain().empty());
}

TEST_F(StreamLoggerTest, AFloodOfOneEventIsABurstAndTheNextLineSaysHowManyWereHeldBack) {
    auto logger = make();
    for (int i = 0; i < 50; ++i) {
        logger.log(LogLevel::Warn, "bad_handshake", "refusing");
    }
    EXPECT_EQ(lines(drain()).size(), 10U);
    clock.advance(1s);
    logger.log(LogLevel::Warn, "bad_handshake", "refusing");
    const auto later = lines(drain());
    ASSERT_EQ(later.size(), 1U);
    EXPECT_NE(later[0].find(" suppressed=40 "), std::string::npos) << later[0];
}

TEST_F(StreamLoggerTest, ALineIsAtMostTheBoundWhateverThePeerSent) {
    auto logger = make();
    logger.log(LogRecord{LogLevel::Warn, "origin_refused", std::string(100'000, '\xff'), 1, ""});
    const std::string out = drain();
    EXPECT_LE(out.size(), wsrv::kLogMaxLine);
    EXPECT_EQ(lines(out).size(), 1U);
}

TEST_F(StreamLoggerTest, AnInterruptedWriteIsRetriedOnce) {
    auto logger = make();
    sys.fail(Call::Write, EINTR);
    logger.log(LogLevel::Info, "listening", "x");
    EXPECT_EQ(lines(drain()).size(), 1U);
}

TEST_F(StreamLoggerTest, AFailedWriteIsCountedAndTheNextLineThatGetsOutSaysSo) {
    auto logger = make();
    sys.fail(Call::Write, EPIPE);
    logger.log(LogLevel::Warn, "a", "lost");
    EXPECT_EQ(drain(), "");
    logger.log(LogLevel::Warn, "b", "kept");
    const auto got = lines(drain());
    ASSERT_EQ(got.size(), 1U);
    EXPECT_NE(got[0].find(" dropped=1 "), std::string::npos) << got[0];
    logger.log(LogLevel::Warn, "c", "kept");
    EXPECT_EQ(drain().find("dropped="), std::string::npos) << "reported once";
}

TEST_F(StreamLoggerTest, ADestinationThatIsNotWritableIsNotWrittenTo) {
    auto logger = make();
    sys.succeed(Call::Poll, 0);  // poll says: nothing ready
    logger.log(LogLevel::Warn, "a", "lost");
    EXPECT_EQ(sys.calls(Call::Write), 0U) << "no write() when poll() says the pipe is full";
    logger.log(LogLevel::Warn, "b", "kept");
    EXPECT_NE(drain().find(" dropped=1 "), std::string::npos);
}

TEST_F(StreamLoggerTest, AShortWriteIsCountedAsALostLine) {
    auto logger = make();
    sys.succeed(Call::Write, 5);
    logger.log(LogLevel::Warn, "a", "short");
    logger.log(LogLevel::Warn, "b", "whole");
    EXPECT_NE(drain().find(" dropped=1 "), std::string::npos);
}

// The property the whole design rests on: a full pipe (a stuck journald, a slow log driver) must
// not stall the thread that logs, which is an I/O loop.
TEST_F(StreamLoggerTest, AFullPipeDropsTheLineInsteadOfBlockingTheCaller) {
    // Fill the pipe through a second, non-blocking open of the same pipe. (A dup() would not do:
    // O_NONBLOCK belongs to the open file description, so it would make the logger's descriptor
    // non-blocking too and the test could never see a block.)
    const std::string path = "/proc/self/fd/" + std::to_string(fds_[1]);
    const int filler = ::open(path.c_str(), O_WRONLY | O_NONBLOCK);
    ASSERT_NE(filler, -1);
    const std::string junk(4096, 'x');
    std::size_t stuffed = 0;
    while (::write(filler, junk.data(), junk.size()) > 0) {
        stuffed += junk.size();
    }
    while (::write(filler, "x", 1) > 0) {  // and any partly used page
        ++stuffed;
    }
    ::close(filler);
    ASSERT_GT(stuffed, 0U);
    ASSERT_EQ(::fcntl(fds_[1], F_GETFL) & O_NONBLOCK, 0)
        << "the logger's descriptor must still block";

    auto logger = make();
    auto done =
        std::async(std::launch::async, [&] { logger.log(LogLevel::Error, "internal_error", "x"); });
    const bool returned = done.wait_for(2s) == std::future_status::ready;
    if (!returned) {
        static_cast<void>(drain());  // unblock the stuck writer so the test can end
    }
    EXPECT_TRUE(returned) << "log() blocked on a full pipe";
    done.wait();

    static_cast<void>(drain());
    logger.log(LogLevel::Error, "other_event", "x");
    EXPECT_NE(drain().find(" dropped=1 "), std::string::npos);
}

TEST_F(StreamLoggerTest, ManyThreadsNeverInterleaveInsideALine) {
    auto logger = make();
    constexpr int kThreads = 8;
    constexpr int kLinesEach = 10;  // one burst: the limiter lets every one through
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&logger, t] {
            const std::string event = "thread" + std::to_string(t);
            const std::string message(200, static_cast<char>('a' + t));
            for (int i = 0; i < kLinesEach; ++i) {
                logger.log(LogLevel::Warn, event, message);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    const auto got = lines(drain());
    ASSERT_EQ(got.size(), static_cast<std::size_t>(kThreads * kLinesEach));
    for (const auto& line : got) {
        ASSERT_EQ(line.rfind("ts=", 0), 0U) << line;
        const auto event_at = line.find(" event=thread");
        ASSERT_NE(event_at, std::string::npos) << line;
        const int who = line[event_at + std::string{" event=thread"}.size()] - '0';
        const std::string expected_tail =
            " msg=\"" + std::string(200, static_cast<char>('a' + who)) + "\"";
        EXPECT_EQ(line.substr(line.size() - expected_tail.size()), expected_tail) << line;
    }
}

// SPDX-License-Identifier: Apache-2.0
// ADR-0040 section 2: a connection charges the bytes it buffers to one shared counter by difference
// and gives back exactly what it charged, from a destructor. These tests pin the arithmetic and,
// above all, that the counter cannot drift: a budget that only grows turns a defence into an
// outage.
#include "server/buffer_ledger.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

namespace {

using wsrv::BufferLedger;

TEST(BufferLedger, ChargesTheDifferenceNotTheTotalEachTime) {
    std::atomic<std::size_t> total{0};
    BufferLedger ledger{&total};
    ledger.set(1000);
    EXPECT_EQ(total.load(), 1000U);
    ledger.set(1500);
    EXPECT_EQ(total.load(), 1500U) << "growing by 500 adds 500, not 1500";
    ledger.set(200);
    EXPECT_EQ(total.load(), 200U) << "shrinking subtracts the difference";
    ledger.set(200);
    EXPECT_EQ(total.load(), 200U) << "the same value changes nothing";
    EXPECT_EQ(ledger.charged(), 200U);
}

TEST(BufferLedger, TwoLedgersAddUpAndEachGivesBackOnlyItsOwn) {
    std::atomic<std::size_t> total{0};
    BufferLedger first{&total};
    {
        BufferLedger second{&total};
        first.set(300);
        second.set(700);
        EXPECT_EQ(total.load(), 1000U);
    }  // second ends here
    EXPECT_EQ(total.load(), 300U) << "the destructor refunds exactly what was charged";
    first.release();
    EXPECT_EQ(total.load(), 0U);
}

TEST(BufferLedger, ReleaseIsIdempotentAndTheDestructorAfterItRefundsNothingMore) {
    std::atomic<std::size_t> total{0};
    {
        BufferLedger ledger{&total};
        ledger.set(64);
        ledger.release();
        ledger.release();
        EXPECT_EQ(total.load(), 0U);
        EXPECT_EQ(ledger.charged(), 0U);
    }
    EXPECT_EQ(total.load(), 0U) << "no second refund: the counter would wrap";
}

TEST(BufferLedger, ALedgerThatIsChargedAgainAfterReleaseCountsAgain) {
    std::atomic<std::size_t> total{0};
    BufferLedger ledger{&total};
    ledger.set(10);
    ledger.release();
    ledger.set(25);
    EXPECT_EQ(total.load(), 25U);
}

TEST(BufferLedger, WithoutACounterItIsInert) {  // tests and fakes build connections without one
    BufferLedger ledger;
    ledger.set(123);
    ledger.release();
    SUCCEED();
}

// The property the whole design rests on, run under TSan in CI: many threads charge, change and
// drop their own ledgers in any order against one counter, and when the last is gone the counter
// is exactly zero.
TEST(BufferLedger, TheCounterIsExactlyZeroWhenEveryLedgerIsGoneWhateverTheInterleaving) {
    std::atomic<std::size_t> total{0};
    constexpr int kThreads = 4;
    constexpr int kRounds = 2000;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&total, t] {
            for (int round = 0; round < kRounds; ++round) {
                BufferLedger ledger{&total};
                std::size_t value = static_cast<std::size_t>((round * 7 + t) % 97) * 1024;
                for (int step = 0; step < 5; ++step) {
                    ledger.set(value);
                    value = (value * 3 + 11) % 100000;
                }
                if (round % 3 == 0) {
                    ledger.release();
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(total.load(), 0U);
}

}  // namespace

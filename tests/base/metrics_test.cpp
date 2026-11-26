// SPDX-License-Identifier: Apache-2.0
// Counters and their Prometheus text (ADR-0023): a closed set of metrics so peer data can never
// create a label, lock-free counters that stay exact under several writers, and an exposition that
// is well formed whatever the values are.
#include "base/metrics.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using wsrv::CounterBank;
using wsrv::Metric;
using wsrv::MetricsSnapshot;

constexpr std::size_t kCount = wsrv::kMetricCount;

Metric metric_at(std::size_t index) {
    return static_cast<Metric>(index);
}

TEST(Metrics, EveryMetricHasAUniqueNameAndLabelFromTheSafeAlphabet) {  // ADR-0023 section 1
    std::set<std::pair<std::string_view, std::string>> seen;
    std::set<std::string_view> finished;
    std::string_view previous;
    for (std::size_t i = 0; i < kCount; ++i) {
        const wsrv::MetricInfo& info = wsrv::metric_info(metric_at(i));
        EXPECT_FALSE(info.name.empty()) << i;
        EXPECT_TRUE(info.name.starts_with("wsrv_")) << info.name;
        for (const std::string_view text :
             {info.name, info.label1_key, info.label1_value, info.label2_key, info.label2_value}) {
            for (const char c : text) {
                EXPECT_TRUE((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')
                    << "'" << text << "' would need escaping in the exposition";
            }
        }
        EXPECT_EQ(info.label1_key.empty(), info.label1_value.empty()) << info.name;
        EXPECT_EQ(info.label2_key.empty(), info.label2_value.empty()) << info.name;
        EXPECT_TRUE(info.label2_key.empty() || !info.label1_key.empty()) << info.name;
        EXPECT_TRUE(
            seen.emplace(info.name,
                         std::string{info.label1_value} + "/" + std::string{info.label2_value})
                .second)
            << "duplicate series of " << info.name;
        if (info.name != previous) {  // a name's series are contiguous: one header per name
            EXPECT_TRUE(finished.insert(info.name).second) << info.name << " is split";
            previous = info.name;
        }
    }
}

TEST(Metrics, ACounterBankStartsAtZeroAndCountsExactly) {
    CounterBank bank;
    for (std::size_t i = 0; i < kCount; ++i) {
        EXPECT_EQ(bank.read(metric_at(i)), 0U);
    }
    bank.add(Metric::ConnectionsAccepted);
    bank.add(Metric::ConnectionsAccepted, 41);
    bank.add(Metric::BytesIn, 1000);
    EXPECT_EQ(bank.read(Metric::ConnectionsAccepted), 42U);
    EXPECT_EQ(bank.read(Metric::BytesIn), 1000U);
    EXPECT_EQ(bank.read(Metric::BytesOut), 0U) << "counters do not leak into each other";
}

TEST(Metrics, ASnapshotSumsTheBanksOfAllLoops) {
    CounterBank first;
    CounterBank second;
    first.add(Metric::MessagesInText, 3);
    second.add(Metric::MessagesInText, 4);
    second.add(Metric::BytesOut, 9);
    MetricsSnapshot snapshot;
    snapshot.accumulate(first);
    snapshot.accumulate(second);
    EXPECT_EQ(snapshot.value(Metric::MessagesInText), 7U);
    EXPECT_EQ(snapshot.value(Metric::BytesOut), 9U);
}

TEST(Metrics, ASnapshotSaturatesInsteadOfWrapping) {  // checked arithmetic, even for counters
    CounterBank bank;
    bank.add(Metric::BytesIn, std::numeric_limits<std::uint64_t>::max());
    MetricsSnapshot snapshot;
    snapshot.accumulate(bank);
    snapshot.accumulate(bank);
    EXPECT_EQ(snapshot.value(Metric::BytesIn), std::numeric_limits<std::uint64_t>::max());
}

TEST(Metrics, AdditionsFromManyThreadsAreExactAndAReaderSeesThemOnlyGrow) {  // run under TSan
    CounterBank bank;
    constexpr int kThreads = 4;
    constexpr int kPerThread = 20000;
    std::atomic<bool> go{false};
    std::vector<std::thread> writers;
    for (int t = 0; t < kThreads; ++t) {
        writers.emplace_back([&] {
            while (!go.load()) {
            }
            for (int i = 0; i < kPerThread; ++i) {
                bank.add(Metric::MessagesOutBinary);
            }
        });
    }
    go.store(true);
    std::uint64_t last = 0;
    bool monotonic = true;
    for (int i = 0; i < 2000; ++i) {
        const std::uint64_t now = bank.read(Metric::MessagesOutBinary);
        monotonic = monotonic && now >= last;
        last = now;
    }
    for (std::thread& writer : writers) {
        writer.join();
    }
    EXPECT_TRUE(monotonic);
    EXPECT_EQ(bank.read(Metric::MessagesOutBinary), std::uint64_t{kThreads} * kPerThread);
}

TEST(MetricBanks, EachLoopClaimsItsOwnBankAndTheSnapshotSumsThemAll) {
    wsrv::MetricBanks banks{3};
    wsrv::CounterBank& a = banks.claim();
    wsrv::CounterBank& b = banks.claim();
    wsrv::CounterBank& c = banks.claim();
    EXPECT_NE(&a, &b);
    EXPECT_NE(&b, &c);
    a.add(Metric::BytesIn, 1);
    b.add(Metric::BytesIn, 10);
    c.add(Metric::BytesIn, 100);
    EXPECT_EQ(banks.snapshot().value(Metric::BytesIn), 111U);
}

TEST(MetricBanks, ClaimingMoreThanPlannedSharesASpareBankAndCountsStillAdd) {  // never fails
    wsrv::MetricBanks banks{1};
    wsrv::CounterBank& first = banks.claim();
    wsrv::CounterBank& spare1 = banks.claim();
    wsrv::CounterBank& spare2 = banks.claim();
    EXPECT_NE(&first, &spare1);
    EXPECT_EQ(&spare1, &spare2) << "a miscounted loop must not take memory or fail";
    first.add(Metric::BytesOut, 5);
    spare1.add(Metric::BytesOut, 6);
    spare2.add(Metric::BytesOut, 7);
    EXPECT_EQ(banks.snapshot().value(Metric::BytesOut), 18U);
}

TEST(MetricBanks, TheSharedBankCountsInTheSnapshotAndOutlivesTheBanks) {  // ADR-0024 A3
    std::shared_ptr<wsrv::CounterBank> shared;
    {
        wsrv::MetricBanks banks{2};
        shared = banks.shared();
        shared->add(Metric::BrokerPublishes, 3);
        banks.claim().add(Metric::BrokerPublishes, 4);
        EXPECT_EQ(banks.snapshot().value(Metric::BrokerPublishes), 7U);
        EXPECT_EQ(shared, banks.shared()) << "one shared bank, not one per call";
    }
    shared->add(Metric::BrokerPublishes);  // the owner is gone: still valid memory
    EXPECT_EQ(shared->read(Metric::BrokerPublishes), 4U);
}

TEST(Metrics, TheNullSinkAcceptsEverythingAndKeepsNothing) {
    wsrv::NullMetrics sink;
    wsrv::IMetrics& metrics = sink;
    metrics.add(Metric::ConnectionsAccepted, 5);  // nothing to assert but that it is harmless
    SUCCEED();
}

TEST(MetricsText, TheExpositionHasOneHelpAndTypePerNameAndOneLinePerLabel) {
    CounterBank bank;
    bank.add(Metric::ConnectionsAccepted, 7);
    bank.add(Metric::RejectedGlobalCap, 2);
    MetricsSnapshot snapshot;
    snapshot.accumulate(bank);
    const std::string text = wsrv::render_prometheus(snapshot, wsrv::MetricsGauges{12});

    EXPECT_NE(text.find("# TYPE wsrv_connections_total counter\n"), std::string::npos) << text;
    EXPECT_NE(text.find("wsrv_connections_total{result=\"accepted\"} 7\n"), std::string::npos)
        << text;
    EXPECT_NE(text.find("wsrv_connections_total{result=\"rejected_global_cap\"} 2\n"),
              std::string::npos)
        << text;
    EXPECT_NE(text.find("# TYPE wsrv_connections_current gauge\nwsrv_connections_current 12\n"),
              std::string::npos)
        << text;
    std::size_t types = 0;
    for (std::size_t at = text.find("# TYPE wsrv_connections_total"); at != std::string::npos;
         at = text.find("# TYPE wsrv_connections_total", at + 1)) {
        ++types;
    }
    EXPECT_EQ(types, 1U) << "a name is declared once however many labels it has";
}

TEST(MetricsText, TheBufferedBytesGaugeAndTheShedCounterAreExposed) {  // ADR-0040
    CounterBank bank;
    bank.add(Metric::RejectedBufferBudget, 3);
    MetricsSnapshot snapshot;
    snapshot.accumulate(bank);
    const std::string text = wsrv::render_prometheus(snapshot, wsrv::MetricsGauges{12, 3456});
    EXPECT_NE(text.find("# TYPE wsrv_buffered_bytes gauge\nwsrv_buffered_bytes 3456\n"),
              std::string::npos)
        << text;
    EXPECT_NE(text.find("wsrv_connections_total{result=\"rejected_buffer_budget\"} 3\n"),
              std::string::npos)
        << text;
    EXPECT_NE(text.find("wsrv_connections_current 12\n"), std::string::npos) << text;
}

TEST(MetricsText, EveryMetricAppearsExactlyOnceEvenAtZero) {
    const std::string text = wsrv::render_prometheus(MetricsSnapshot{}, wsrv::MetricsGauges{});
    for (std::size_t i = 0; i < kCount; ++i) {
        const wsrv::MetricInfo& info = wsrv::metric_info(metric_at(i));
        std::string line{info.name};
        if (!info.label1_key.empty()) {
            line +=
                "{" + std::string{info.label1_key} + "=\"" + std::string{info.label1_value} + "\"";
            if (!info.label2_key.empty()) {
                line += "," + std::string{info.label2_key} + "=\"" +
                        std::string{info.label2_value} + "\"";
            }
            line += "}";
        }
        line += " 0\n";
        const std::size_t first = text.find("\n" + line);
        ASSERT_NE(first, std::string::npos) << line;
        EXPECT_EQ(text.find("\n" + line, first + 1), std::string::npos) << line;
    }
}

TEST(MetricsText, TheTextIsBoundedAndEveryLineIsACommentOrASample) {
    CounterBank bank;
    for (std::size_t i = 0; i < kCount; ++i) {
        bank.add(metric_at(i), std::numeric_limits<std::uint64_t>::max());
    }
    MetricsSnapshot snapshot;
    snapshot.accumulate(bank);
    const std::string text = wsrv::render_prometheus(snapshot, wsrv::MetricsGauges{});
    EXPECT_LE(text.size(), wsrv::kMaxMetricsTextBytes);
    ASSERT_FALSE(text.empty());
    EXPECT_EQ(text.back(), '\n');
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find('\n', start);
        const std::string line = text.substr(start, end - start);
        EXPECT_TRUE(line.starts_with("# HELP ") || line.starts_with("# TYPE ") ||
                    line.starts_with("wsrv_"))
            << line;
        start = end + 1;
    }
}

}  // namespace

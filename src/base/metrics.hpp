// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wsrv {

/// Every counter the server keeps, one enumerator per (metric name, label values) pair (ADR-0023
/// section 1). The set is closed at compile time, so no peer-supplied text can ever become a label
/// and the number of time series is a constant. Enumerators of one metric name stay together:
/// the exposition declares a name once, before its first series.
enum class Metric : std::uint8_t {
    // wsrv_connections_total{result}
    ConnectionsAccepted,
    RejectedGlobalCap,
    RejectedIpConnections,
    RejectedIpRate,
    RejectedIpTable,
    HandshakeFailed,
    // wsrv_messages_total{direction,type}
    MessagesInText,
    MessagesInBinary,
    MessagesOutText,
    MessagesOutBinary,
    // wsrv_bytes_total{direction}: bytes at the socket (ciphertext when TLS is on)
    BytesIn,
    BytesOut,
    // wsrv_close_total{class}
    CloseNormal,
    CloseProtocolError,
    ClosePolicy,
    CloseAbnormal,
    CloseOther,
    // single series
    SlowConsumerDrops,
    // wsrv_accept_errors_total{errno}
    AcceptErrorsEmfile,
    AcceptErrorsOther,
    TlsHandshakeFailures,
    // wsrv_broker_publishes_total
    BrokerPublishes,
    // wsrv_broker_deliveries_total{result}
    BrokerDeliveriesAccepted,
    BrokerDeliveriesRefused,
    // wsrv_publish_http_total{status}: answers of the publish endpoint (ADR-0028), by HTTP status
    PublishHttp200,
    PublishHttp400,
    PublishHttp401,
    PublishHttp404,
    PublishHttp405,
    PublishHttp411,
    PublishHttp413,
    PublishHttp415,
    PublishHttp417,
    PublishHttp429,
    PublishHttp501,
    PublishHttp503,
    Count  ///< not a metric: the size of the enumeration
};

inline constexpr std::size_t kMetricCount = static_cast<std::size_t>(Metric::Count);

/// Up to two labels per series; empty strings mean "no label".
struct MetricInfo {
    std::string_view name;
    std::string_view help;
    bool gauge;
    std::string_view label1_key;
    std::string_view label1_value;
    std::string_view label2_key;
    std::string_view label2_value;
};

[[nodiscard]] const MetricInfo& metric_info(Metric metric) noexcept;

/// Where the server and its connections report events (ADR-0023 section 1). Counting is a single
/// relaxed add: never blocks, never allocates, never throws.
/// Thread-affinity: any thread.
class IMetrics {
public:
    IMetrics() = default;
    IMetrics(const IMetrics&) = delete;
    IMetrics& operator=(const IMetrics&) = delete;
    IMetrics(IMetrics&&) = delete;
    IMetrics& operator=(IMetrics&&) = delete;
    virtual ~IMetrics() = default;

    virtual void add(Metric metric, std::uint64_t amount = 1) noexcept = 0;
};

/// Discards everything: the default for code that is not being observed.
class NullMetrics final : public IMetrics {
public:
    void add(Metric /*metric*/, std::uint64_t /*amount*/ = 1) noexcept override {}
};

/// The counters of one loop (ADR-0023 section 2): normally one writer (the shared bank has several;
/// the adds are atomic either way), any number of readers, no lock.
/// Aligned to a cache line so two loops' banks never share one.
/// Thread-affinity: any thread (relaxed atomics; totals are exact, order across counters is not).
class alignas(64) CounterBank final : public IMetrics {
public:
    void add(Metric metric, std::uint64_t amount = 1) noexcept override {
        values_[static_cast<std::size_t>(metric)].fetch_add(amount, std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t read(Metric metric) const noexcept {
        return values_[static_cast<std::size_t>(metric)].load(std::memory_order_relaxed);
    }

private:
    std::array<std::atomic<std::uint64_t>, kMetricCount> values_{};
};

/// A point-in-time sum over the banks of all loops. Counter by counter, not atomic across counters.
class MetricsSnapshot {
public:
    /// Add every counter of `bank`; a sum that would pass the maximum stays at the maximum.
    void accumulate(const CounterBank& bank) noexcept;
    [[nodiscard]] std::uint64_t value(Metric metric) const noexcept {
        return values_[static_cast<std::size_t>(metric)];
    }

private:
    std::array<std::uint64_t, kMetricCount> values_{};
};

/// The banks of one server: one per loop, claimed by each loop as it starts, plus one spare so a
/// miscounted loop shares instead of failing. Owned by the server, which outlives its loops, so a
/// scrape can read every bank whatever state the loops are in.
/// Thread-affinity: claim() and snapshot() any thread.
class MetricBanks {
public:
    /// Allocates `loops + 1` banks. May throw std::bad_alloc (construction only).
    explicit MetricBanks(std::size_t loops);
    MetricBanks(const MetricBanks&) = delete;
    MetricBanks& operator=(const MetricBanks&) = delete;
    MetricBanks(MetricBanks&&) = delete;
    MetricBanks& operator=(MetricBanks&&) = delete;
    ~MetricBanks() = default;

    /// The next unclaimed bank; the spare once all `loops` are taken.
    [[nodiscard]] CounterBank& claim() noexcept;
    /// The shared bank, for writers that are not a loop (the broker, ADR-0024 section 6): several
    /// threads add to it at once, which the relaxed atomics allow. A loop that claims it after
    /// the others are taken shares it the same way. It is reference counted so a holder that
    /// outlives these banks (a broker used from an embedder's thread while the server is being
    /// destroyed) writes to valid memory, never to freed memory (ADR-0024 A3).
    [[nodiscard]] std::shared_ptr<CounterBank> shared() const noexcept {
        return shared_;
    }
    [[nodiscard]] MetricsSnapshot snapshot() const noexcept;

private:
    std::size_t loops_;
    std::vector<CounterBank> banks_;       // one per loop; never resized
    std::shared_ptr<CounterBank> shared_;  // the spare, see shared()
    std::atomic<std::size_t> next_{0};
};

/// Values that are state, not events.
struct MetricsGauges {
    std::uint64_t connections_current = 0;
};

/// The text of the `/metrics` page never exceeds this (the enumeration is fixed and every value
/// has at most 20 digits), so the admin endpoint can size its response without asking the peer.
inline constexpr std::size_t kMaxMetricsTextBytes = std::size_t{16} * 1024;

/// Prometheus text exposition format 0.0.4: HELP and TYPE once per name, then one line per series.
[[nodiscard]] std::string render_prometheus(const MetricsSnapshot& snapshot,
                                            const MetricsGauges& gauges);

}  // namespace wsrv

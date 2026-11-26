// SPDX-License-Identifier: Apache-2.0
#include "base/metrics.hpp"

#include <limits>

namespace wsrv {

namespace {

constexpr MetricInfo connections_series(std::string_view result, std::string_view help) {
    return {"wsrv_connections_total", help, false, "result", result, "", ""};
}
constexpr MetricInfo messages_series(std::string_view direction, std::string_view type) {
    return {"wsrv_messages_total",
            "Data messages delivered or sent, by direction and type.",
            false,
            "direction",
            direction,
            "type",
            type};
}
constexpr MetricInfo bytes_series(std::string_view direction) {
    return {"wsrv_bytes_total",
            "Bytes read from or written to client sockets.",
            false,
            "direction",
            direction,
            "",
            ""};
}
constexpr MetricInfo close_series(std::string_view close_class) {
    return {"wsrv_close_total",
            "Closed connections by class of the close code.",
            false,
            "class",
            close_class,
            "",
            ""};
}
constexpr MetricInfo publish_http_series(std::string_view status) {
    return {"wsrv_publish_http_total",
            "Answers of the HTTP publish endpoint, by status.",
            false,
            "status",
            status,
            "",
            ""};
}
constexpr MetricInfo broker_deliveries_series(std::string_view result) {
    return {"wsrv_broker_deliveries_total",
            "Broker deliveries to subscribers, by whether the subscriber's loop accepted them.",
            false,
            "result",
            result,
            "",
            ""};
}
constexpr MetricInfo accept_errors_series(std::string_view error) {
    return {"wsrv_accept_errors_total",
            "Failed accept calls (each makes the acceptor back off).",
            false,
            "errno",
            error,
            "",
            ""};
}

/// In the order of enum Metric (checked by the size assertion and by the tests).
constexpr std::array<MetricInfo, kMetricCount> kInfo{{
    connections_series("accepted", "Connections by outcome at accept and handshake."),
    connections_series("rejected_global_cap", "Connections by outcome at accept and handshake."),
    connections_series("rejected_ip_connections",
                       "Connections by outcome at accept and handshake."),
    connections_series("rejected_ip_rate", "Connections by outcome at accept and handshake."),
    connections_series("rejected_ip_table", "Connections by outcome at accept and handshake."),
    connections_series("rejected_buffer_budget", "Connections by outcome at accept and handshake."),
    connections_series("handshake_failed", "Connections by outcome at accept and handshake."),
    messages_series("in", "text"),
    messages_series("in", "binary"),
    messages_series("out", "text"),
    messages_series("out", "binary"),
    bytes_series("in"),
    bytes_series("out"),
    close_series("normal"),
    close_series("protocol_error"),
    close_series("policy"),
    close_series("abnormal"),
    close_series("other"),
    MetricInfo{"wsrv_slow_consumer_drops_total",
               "Connections dropped because the peer did not read what was queued for it.",
               false,
               "",
               "",
               "",
               ""},
    accept_errors_series("emfile"),
    accept_errors_series("other"),
    MetricInfo{
        "wsrv_tls_handshake_failures_total", "TLS handshakes that failed.", false, "", "", "", ""},
    MetricInfo{"wsrv_broker_publishes_total",
               "Messages published to broker topics.",
               false,
               "",
               "",
               "",
               ""},
    broker_deliveries_series("accepted"),
    broker_deliveries_series("refused"),
    publish_http_series("200"),
    publish_http_series("400"),
    publish_http_series("401"),
    publish_http_series("404"),
    publish_http_series("405"),
    publish_http_series("411"),
    publish_http_series("413"),
    publish_http_series("415"),
    publish_http_series("417"),
    publish_http_series("429"),
    publish_http_series("501"),
    publish_http_series("503"),
}};

void append_number(std::string& out, std::uint64_t value) {
    out += std::to_string(value);
}

void append_series(std::string& out, const MetricInfo& info, std::uint64_t value) {
    out += info.name;
    if (!info.label1_key.empty()) {
        out += '{';
        out += info.label1_key;
        out += "=\"";
        out += info.label1_value;
        out += '"';
        if (!info.label2_key.empty()) {
            out += ',';
            out += info.label2_key;
            out += "=\"";
            out += info.label2_value;
            out += '"';
        }
        out += '}';
    }
    out += ' ';
    append_number(out, value);
    out += '\n';
}

void append_header(std::string& out, const MetricInfo& info) {
    out += "# HELP ";
    out += info.name;
    out += ' ';
    out += info.help;
    out += "\n# TYPE ";
    out += info.name;
    out += info.gauge ? " gauge\n" : " counter\n";
}

}  // namespace

const MetricInfo& metric_info(Metric metric) noexcept {
    return kInfo[static_cast<std::size_t>(metric)];
}

void MetricsSnapshot::accumulate(const CounterBank& bank) noexcept {
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t i = 0; i < kMetricCount; ++i) {
        const std::uint64_t add = bank.read(static_cast<Metric>(i));
        values_[i] = values_[i] > kMax - add ? kMax : values_[i] + add;
    }
}

MetricBanks::MetricBanks(std::size_t loops)
    : loops_{loops}, banks_(loops), shared_{std::make_shared<CounterBank>()} {}

CounterBank& MetricBanks::claim() noexcept {
    const std::size_t index = next_.fetch_add(1, std::memory_order_relaxed);
    return index < loops_ ? banks_[index] : *shared_;  // past the loops: the shared spare
}

MetricsSnapshot MetricBanks::snapshot() const noexcept {
    MetricsSnapshot sum;
    for (const CounterBank& bank : banks_) {
        sum.accumulate(bank);
    }
    sum.accumulate(*shared_);
    return sum;
}

std::string render_prometheus(const MetricsSnapshot& snapshot, const MetricsGauges& gauges) {
    std::string out;
    out.reserve(4096);
    append_header(
        out,
        MetricInfo{
            "wsrv_connections_current", "Connections currently open.", true, "", "", "", ""});
    out += "wsrv_connections_current ";
    append_number(out, gauges.connections_current);
    out += '\n';
    append_header(
        out,
        MetricInfo{"wsrv_buffered_bytes",
                   "Bytes held in partial inbound messages and outbound queues (ADR-0040).",
                   true,
                   "",
                   "",
                   "",
                   ""});
    out += "wsrv_buffered_bytes ";
    append_number(out, gauges.buffered_bytes);
    out += '\n';
    std::string_view previous;
    for (std::size_t i = 0; i < kMetricCount; ++i) {
        const MetricInfo& info = kInfo[i];
        if (info.name != previous) {
            append_header(out, info);
            previous = info.name;
        }
        append_series(out, info, snapshot.value(static_cast<Metric>(i)));
    }
    return out;
}

}  // namespace wsrv

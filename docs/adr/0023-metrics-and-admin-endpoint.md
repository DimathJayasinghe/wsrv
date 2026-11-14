# ADR-0023: Metrics and the admin endpoint

- Status: **Accepted** (2026-11-14; M10 slice 2; OPERATIONS section 3, FR-S8, ARCHITECTURE section 2 `IMetrics`)
- Date: 2026-11-14
- Deciders: project owner, maintainer assistant

## Context
OPERATIONS section 3 promises Prometheus metrics and `/metrics`, `/healthz`, `/readyz` on a separate admin listener; ARCHITECTURE lists `IMetrics` as an injected interface with a no-op default. None exists. Without numbers an operator cannot tell a flood (per-IP refusals, accept errors) from a slow consumer problem, and the runbook rows that say "check `wsrv_accept_errors_total`" point at nothing.

Forces:
- Counting happens on the hot path of every loop; it must cost an add, not a lock, and must not create a cross-loop cache-line fight.
- The label sets must be **bounded**: a label built from peer data (a path, a header, an address, a free close reason) is an unbounded-memory attack on the metrics store.
- A new thread needs an ADR (CLAUDE.md); the loops are shared-nothing and `protocol/`, `http/` must not know about the OS.
- The admin port serves an unauthenticated plain-text page: it is attack surface of its own and must be bounded and loopback by default.

## Decision

### 1. What is counted: a closed enumeration
`enum class Metric` in `base/metrics.hpp` lists every counter, one enumerator per (metric, label value), for example `ConnectionsAccepted`, `RejectedGlobalCap`, `RejectedIpConnections`, `RejectedIpRate`, `RejectedIpTable`, `HandshakeRefused`, `MessagesInText`, `BytesOut`, `CloseNormal`, `ClosePolicy`, `SlowConsumerDrops`, `AcceptErrorsEmfile`. Because the set is a compile-time enumeration, **peer data can never create a label**; the cardinality is a constant. Close codes are grouped into classes (normal 1000/1001, protocol 1002/1003/1007/1009, policy 1008, abnormal 1006/none, other) for the same reason, and the exposition says so.

`IMetrics { virtual void add(Metric, std::uint64_t = 1) noexcept = 0; }`, with `NullMetrics` as the default (ARCHITECTURE: "no-op defaults"). Gauges are read from state that already exists (`connections_current` is the server's live counter).

### 2. Storage: one bank per loop, summed at scrape
Each `LoopShard` owns a `CounterBank`: `std::array<std::atomic<uint64_t>, kMetricCount>` incremented with `fetch_add(relaxed)`. One writer (the loop), any number of readers. The accept path (loop 0) writes loop 0's bank. A scrape sums the banks. No lock, no shared cache line between loops (each bank is cache-line aligned), exact totals, monotonic per counter. The scrape is a snapshot taken counter by counter, not atomic across counters; documented (Prometheus does not need more).

### 3. The admin endpoint runs on loop 0, not on a new thread
`AdminEndpoint` (`src/server/admin_endpoint.*`) owns a second `TcpListener` and `Acceptor` on loop 0 and a small bounded set of `AdminConnection` objects (at most 8 at once; more are accepted and closed). Each reads one request head (at most 2 KiB, 5 s deadline), answers one of:
- `GET /metrics`: Prometheus text exposition, `200`.
- `GET /healthz`: `200 ok` while the process runs its loop (liveness).
- `GET /readyz`: `200 ready` while the server accepts and is not shutting down, else `503` (readiness).
- anything else: `404`; a non-GET method: `405`; an oversized or malformed head: `400`. Always `Connection: close`, no keep-alive, no request body is ever read.
and closes. The response is built in one string and written with one non-blocking send; if the kernel does not take it all in one call the connection is closed (the body is a few KiB at most; a scraper that cannot take it is dropped, not buffered for). Work per request is bounded by the size of the enumeration; at most 8 requests are in flight, so the admin port cannot stall loop 0 for long. No allocation sized from peer data.

Default bind `127.0.0.1`, port off unless configured: `admin_port` (0 = disabled, the default) and `admin_host` (default `127.0.0.1`) in `ServerConfig`, the embedded keys `admin_host` / `admin_port`, `wsrvd` flags `--admin-host` / `--admin-port`. Binding to a non-loopback address is allowed (a container exposing the port to Prometheus) and logged at `warn` with "unauthenticated".

### 4. Where it is counted
`Connection` receives an `IMetrics*` through the context it already holds (`ConnectionContext`; null counts nothing, so existing tests are unaffected); `ShardShared` carries the server's `MetricBanks`, from which each shard claims its bank (the banks belong to the server, which outlives its loops, so a scrape never reads a freed bank). `ServerBuilder::metrics()` from ARCHITECTURE is not provided: the server always keeps its own banks and exposes them (YAGNI; an injected sink would need cross-loop merging). Hook points are the places that already decide the thing: accept (admitted, global cap, each per-IP verdict), `Connection::refuse` (handshake refusals), message dispatch (text / binary count and payload bytes), the transport write path (bytes out), `finish`/on_close (close class), the slow-consumer drop, `Acceptor` error and shed counts, TLS handshake failure. The protocol and http modules are not touched: events they already return are counted by the connection that consumed them.

### 5. Not in this ADR
Histograms (`wsrv_handshake_seconds`, `wsrv_loop_lag_seconds`, `wsrv_outbound_queue_bytes`) need a bucket design and a clock read per event; they are listed in OPERATIONS as planned and tracked as a follow-up. Authentication of the admin port (loopback or network policy is the control), TLS on the admin port, a metrics API for hosts through the C ABI (the counters are readable through `Server::metrics_text()` for embedders; a stable ABI function is a separate decision).

## Consequences
+ Cheap, lock-free, exact counters with a constant label set; the runbook now points at real numbers.
+ No new thread, no new dependency; the admin port is bounded and off by default.
- The scrape is not atomic across counters.
- A slow `/metrics` client occupies one of 8 admin slots for at most the 5 s deadline.
- Loop 0 serves the admin port; a stalled loop 0 also stalls the scrape (which is itself the signal `/healthz` exists to give).
- Revisit trigger: histograms, or a deployment needing the admin port off loop 0.

## Slices
1. (done) This ADR; `Metric`, `IMetrics`, `CounterBank`, `MetricBanks`, exposition text, with unit tests; counting at accept / refuse / close / messages / accept errors; `Server::metrics()`, `metrics_text()`.
2. (done) `AdminEndpoint` and its config, embedded and `wsrvd` keys; end-to-end tests; OPERATIONS section 3.1, SECURITY_DESIGN T21, lesson 32.

As built: `ServerConfig::{admin, admin_max_connections, admin_request_timeout}`, `ServerBuilder::admin()`, `Server::admin_endpoint()`; the bounds `admin_max_connections` and `admin_request_timeout` are `ServerConfig` fields (C++ API) and keep their defaults for embedded/`wsrvd` users (keys can be added without an ABI change if a deployment needs them). No C ABI function exposes the admin port: a host that sets `admin_port` knows it, and only `admin_port=0` (tests) needs the bound value, available as `EmbeddedServer::admin_port()`.

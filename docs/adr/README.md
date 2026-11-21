# Architecture Decision Records

One decision per file, immutable once *Accepted* (supersede with a new ADR). Use [0000-template.md](0000-template.md).

| ADR | Title | Status |
|---|---|---|
| [0001](0001-cpp23-baseline.md) | C++23 baseline | Proposed |
| [0002](0002-sans-io-protocol-core.md) | Sans-I/O protocol core | Proposed |
| [0003](0003-shared-nothing-reactors.md) | Shared-nothing reactor-per-thread concurrency | Proposed |
| [0004](0004-epoll-behind-ipoller.md) | epoll behind `IPoller` | Proposed |
| [0005](0005-zero-core-dependencies.md) | Zero third-party deps in core; in-tree SHA-1/Base64/UTF-8 | Proposed |
| [0006](0006-result-based-errors.md) | `Result<T,E>` error handling | Proposed |
| [0007](0007-bounded-buffers-backpressure.md) | Bounded buffers & backpressure | Proposed |
| [0008](0008-conformance-fuzz-gates.md) | Autobahn + fuzzing + sanitizers as release gates | Proposed |
| [0009](0009-stable-c-abi-for-bindings.md) | Stable C ABI as the single binding surface | Proposed |
| [0010](0010-embedded-and-sidecar-modes.md) | Support embedded and sidecar integration modes | Proposed |
| [0011](0011-open-source-governance.md) | Open-source licensing & governance | Accepted |
| [0012](0012-randomness-behind-irandom.md) | Randomness behind `IRandom`; `crypto` stays OS-free | Accepted |
| [0013](0013-server-connection-model.md) | Server connection model, handler contract, teardown | Proposed |
| [0014](0014-multi-loop-server-and-session-handle.md) | Multi-loop server and the cross-thread SessionHandle | Proposed |
| [0015](0015-streaming-message-writer.md) | Streaming message writer and on_writable | Proposed |
| [0016](0016-c-abi-v0-embedded-pull-model.md) | C ABI v0 (embedded, pull model) and read pause | Accepted |
| [0017](0017-wsrvd-sidecar-and-ipc-v0.md) | `wsrvd` sidecar and IPC protocol v0 | Accepted |
| [0018](0018-handshake-authentication-hook.md) | Handshake authentication hook (application decides who connects) | Accepted |
| [0019](0019-tls-transport.md) | TLS (`wss://`) as a transport decorator over OpenSSL | Accepted |
| [0020](0020-permessage-deflate.md) | permessage-deflate (RFC 7692) over system zlib | Accepted |
| [0021](0021-per-ip-limits.md) | Per-IP connection and connection-rate limits | Accepted |
| [0022](0022-descriptor-budget-check.md) | Descriptor budget check at start (RLIMIT_NOFILE) | Accepted |
| [0023](0023-metrics-and-admin-endpoint.md) | Metrics and the admin endpoint | Accepted |
| [0024](0024-pub-sub-broker.md) | Pub/sub broker (topics and rooms) | Accepted |
| [0025](0025-php-ffi-binding.md) | PHP embedded binding over FFI | Proposed |

Statuses: Proposed → Accepted → (Deprecated | Superseded by ADR-NNNN). The project owner moves Proposed → Accepted.

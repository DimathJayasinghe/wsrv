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
| [0025](0025-php-ffi-binding.md) | PHP embedded binding over FFI | Accepted |
| [0026](0026-php-package-and-native-library-distribution.md) | Composer package and distribution of `libwsrv.so` | Accepted |
| [0027](0027-broker-through-the-c-abi.md) | Topics and rooms through the C ABI (ABI 0.5) | Accepted |
| [0028](0028-publish-from-anywhere.md) | Publish from anywhere: rooms and an HTTP publish endpoint for `wsrvd` | Accepted |
| [0029](0029-client-address-behind-a-proxy.md) | The client address behind a reverse proxy | Accepted |
| [0030](0030-container-configuration-and-healthcheck.md) | Environment configuration and a built-in healthcheck for containers | Accepted |
| [0031](0031-browser-and-node-client.md) | A small JavaScript client with reconnect (browser and Node) | Accepted |
| [0032](0032-pending-send-byte-budget.md) | A byte budget for sends waiting for a loop | Accepted |
| [0033](0033-release-supply-chain.md) | The release supply chain: scan, sign, attest, SBOM, a second registry | Accepted |
| [0034](0034-bulk-fan-out-in-chunks.md) | Work for many connections is posted in chunks, and refused closes are retried | Accepted |
| [0035](0035-wake-the-host-once-per-loop-iteration.md) | The host's wake-up is paid once per loop iteration, not once per message | Accepted |
| [0036](0036-two-fewer-system-calls-per-message.md) | Two fewer system calls per message: no read that only blocks, no getpid per call | Accepted |
| [0037](0037-runtime-logging.md) | Runtime logging: one structured line per event, to stderr, bounded and rate-limited | Accepted |
| [0038](0038-tcp-transport-for-the-ipc-link.md) | A TCP transport for the IPC link: a worker on another host, or on macOS and Windows against a daemon in Docker | Accepted |
| [0039](0039-generated-token-for-the-publish-endpoint.md) | The publish token must be one `wsrvd --generate-token` made (a hard requirement in 1.1) | Accepted |
| [0040](0040-global-budget-for-buffered-bytes.md) | A global budget for the bytes connections buffer: shed new connections above `max_buffered_bytes` | Accepted |

Statuses: Proposed → Accepted → (Deprecated | Superseded by ADR-NNNN). The project owner moves Proposed → Accepted.

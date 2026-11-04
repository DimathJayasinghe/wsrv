# ADR-0019: TLS (`wss://`) as a transport decorator over OpenSSL

- Status: **Accepted** (2026-11-04; the project owner approved M8 with "go ahead", and the decision that TLS is a built-in feature, in the maintainer's plan)
- Date: 2026-11-04
- Deciders: project owner, maintainer assistant

## Context
Browsers refuse `ws://` from an `https://` page, and a password, cookie or bearer token sent over `ws://` is readable by anyone on the path. A production WebSocket server therefore needs `wss://`: WebSocket inside TLS (RFC 8446 for TLS 1.3, RFC 5246 for TLS 1.2; RFC 6455 section 11.1.2 and 11.1.1 describe `wss`). Requirements: FR-T1 (TLS with hardened defaults, SNI, certificate hot-reload) and FR-T2 (TLS invisible to the protocol layers); threat T12 (downgrade / weak crypto) with the profile in SECURITY_DESIGN section 5; edge cases EC-T01 to EC-T07.

ADR-0005 keeps the core free of third-party code and names OpenSSL as the one allowed optional dependency "for TLS". Writing TLS ourselves is out of the question (CODING rule: security-critical code we cannot reasonably write is the exact case the dependency policy allows). This ADR is the "ADR before a dependency" that CLAUDE.md requires, and it is also structural: a new module, a change to the `ITransport` seam and to `Connection`'s poller interest.

The code base was prepared for this. `Connection` talks only to `ITransport`; the epoll loop is edge-triggered; `HandshakeDriver`, the protocol session and the handshake hook are byte-in / byte-out and never see the socket.

## Options considered
1. **A `TlsTransport` decorator that wraps another `ITransport`, in a new `tls/` module** (this ADR). Protocol layers and `Connection` are unchanged apart from one small hint (section 3).
2. **Terminate TLS in front of wsrv** (nginx, HAProxy, a cloud load balancer). Valid and common, and still supported (plain listener, the startup notice of SECURITY_DESIGN section 3), but it is not a replacement for a product that must be usable on its own. Rejected as the only option.
3. **Hand OpenSSL the socket** (`SSL_set_fd`). Fewer lines, but it bypasses `ISyscalls` (no fault injection in tests), makes the decorator depend on a real descriptor, and cannot be unit-tested against a scripted peer. Rejected.
4. **Another library** (mbedTLS, BoringSSL, rustls through FFI, GnuTLS). Smaller or newer, but OpenSSL is on every Linux distribution (so packaging and security patches come from the OS), is what ADR-0005 and BUILD_AND_TOOLING already name, and is what the dev container ships. Revisit only if a platform needs it; the `tls/` boundary below keeps that cheap.

## Decision

### 1. A new module `tls/`, the only place that includes `<openssl/...>`
`tls` may depend on `base` and `net` (and nothing may depend on `tls` except the composition roots: `apps/wsrvd`, `capi`'s embedded server, examples and tests). OpenSSL types never appear in a public header of `tls/`: `TlsServerContext` and `TlsTransport` hide them behind a pimpl, so no other module needs OpenSSL on its include path, and the include checker (`tools/check_includes.py`) gains the `tls` row plus the existing "no `openssl/` outside `tls`" rule. `server` stays unaware of TLS: it sees only an interface (section 4).

### 2. `TlsTransport`: a decorator that drives OpenSSL with a custom BIO
`TlsTransport` owns an `ITransport` (the `PlainTransport`) and an `SSL` object whose I/O goes through a custom `BIO` that calls the inner transport's `read` / `write`. So the socket is still read and written through `ISyscalls`, the transport is testable with a scripted inner transport (byte at a time, `WouldBlock` at any point), and a TLS record boundary never has to coincide with a TCP read.

The BIO maps the inner `Result` to OpenSSL's retry protocol: `WouldBlock` sets "retry read" or "retry write"; EOF is end of stream; any other error is a failure. The `SSL` is created in accept state (server). The handshake is **not** a separate step: it is driven inside `read()` and `write()`, because `Connection` only knows "read until WouldBlock":
- While the handshake is unfinished, `read()` calls `SSL_do_handshake` and, until it completes, returns `Error{WouldBlock}`; `write()` does the same. Application bytes cannot flow before it completes.
- `SSL_read` returning `WANT_READ` is `WouldBlock`; `SSL_ERROR_ZERO_RETURN` (the peer sent `close_notify`) is the success value 0 (EOF, a clean close, EC-T04); an abrupt TCP close without `close_notify` (`SSL_ERROR_SYSCALL` / unexpected EOF) is `Error{ConnectionClosed}`, which the existing path turns into close code 1006. A peer that is *not speaking TLS* (EC-T02), a failed handshake, a protocol violation and every other OpenSSL failure are a new `ErrorCode::TlsFailure` (category `Tls`, the slot ARCHITECTURE section 8 already reserves). The `Error::context` is a literal chosen by us; the OpenSSL error string is never copied into it (an `Error` may carry only static text), and the OpenSSL error queue is always drained (a stale entry would be blamed on the next, unrelated call).
- **Partial writes.** `SSL_write` has a retry rule: after `WANT_WRITE` the next call must present the same data. `SSL_MODE_ENABLE_PARTIAL_WRITE` and `SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER` are set, and the transport remembers the length it was in the middle of and offers exactly that length again (EC-T06). `Connection` already re-offers the front of its queue, so it needs no change.
- `shutdown_write()` sends `close_notify` (`SSL_shutdown`, best effort and non-blocking) and then the inner FIN. `close()` frees the `SSL` and closes the inner transport; it is idempotent. All memory held for a connection is bounded: OpenSSL's record buffers are the only allocation and are sized by the protocol maximum (16 KiB), not by the peer; `SSL_MODE_RELEASE_BUFFERS` is set so idle connections do not hold them.

### 3. The one change to `ITransport` and `Connection`: "I need to write"
Edge-triggered epoll reports a change, once. During the handshake the server often has to *write* (its flight of certificate and keys) while `Connection` is registered for *read* only. If `SSL_do_handshake` hits `WANT_WRITE` inside `read()`, nothing would ever wake the connection again, and the handshake would hang until the timeout. (TLS 1.3 `KeyUpdate` can do the same later on an established connection.) So `ITransport` gains a non-pure virtual

```cpp
/// True when the last read()/write() returned WouldBlock because the transport needs the descriptor
/// to become WRITABLE (a TLS handshake step). Plain transports never do.
[[nodiscard]] virtual bool wants_write() const noexcept { return false; }
```

and `Connection::apply_interest` includes `Write` interest while it is true, and is called after the read loop and the write path (it is a no-op when the mask is unchanged). The opposite case (a write that needs the socket to be *readable*) cannot happen once renegotiation is off (below), so it is not modeled; the handshake case, where a "write" call needs readable, is covered because `Connection` always reads unless paused by backpressure, and a paused connection is already established.

The handshake budget needs no new timer (EC-T07): `handshake_timeout` is armed at accept and disarmed when the HTTP head is complete, so it covers TLS plus the HTTP request, and a client that stalls inside the TLS handshake is closed by the timer that exists today. The doc string of that option is updated to say so.

### 4. Wiring: an injected wrapper, no TLS knowledge in `server`
`server` defines a small interface `ITransportWrapper { std::unique_ptr<ITransport> wrap(std::unique_ptr<ITransport> plain); }` (an optional member of `ServerConfig`'s runtime inputs, like the handshake decider is injected). `LoopShard::adopt` creates the `PlainTransport` as today and, if a wrapper is set, passes it through. `wrap` can fail (out of memory, `SSL_new`); a null result closes the connection like any accept failure. The composition roots build the concrete `TlsServerContext` and the wrapper that implements the interface (the CLAUDE.md rule: concrete types only in the composition root). `ServerBuilder::tls(...)` takes the interface, so a test can inject a fake.

A listener is either plain or TLS, never both on one port (a "TLS port that also answers plaintext" is a downgrade lever). Running both means two listeners, which the multi-listener work of M10 will give; until then one server has one mode.

### 5. Hardened profile (SECURITY_DESIGN section 5, T12), one place, no knobs that weaken it
Built by `TlsServerContext` and asserted by tests that connect with deliberately weak clients:
- **Protocols:** minimum TLS 1.2, maximum TLS 1.3 (`SSL_CTX_set_min_proto_version`). There is no option to enable SSLv3, TLS 1.0 or 1.1.
- **TLS 1.3 suites:** the OpenSSL defaults restricted to `TLS_AES_256_GCM_SHA384`, `TLS_CHACHA20_POLY1305_SHA256`, `TLS_AES_128_GCM_SHA256`.
- **TLS 1.2:** AEAD with ECDHE only: `ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256`. No CBC, no RSA key exchange, no static DH, no 3DES / RC4 / export. Groups: `X25519:prime256v1:secp384r1`.
- **Options:** `SSL_OP_NO_COMPRESSION` (CRIME), `SSL_OP_NO_RENEGOTIATION` (EC-T05: a ClientHello after the handshake is answered with an alert and the connection is closed), `SSL_OP_CIPHER_SERVER_PREFERENCE`, `SSL_OP_NO_TICKET` plus `SSL_CTX_set_num_tickets(0)` and the session cache off: **no session resumption** in this milestone. Resumption saves a round trip on reconnect but needs ticket-key rotation and a policy for sharing keys between loops; it is a follow-up with its own tests (documented in lesson 22), and "off" is the safe default.
- **ALPN:** WebSocket upgrades ride HTTP/1.1. If the client offers ALPN and `http/1.1` is not in its list, the handshake is refused (alert `no_application_protocol`), which stops cross-protocol confusion (a browser tricked into speaking `h2` at our port, the "ALPACA" class). With no ALPN extension at all, the connection proceeds (plenty of non-browser clients send none).
- **Key and certificate:** PEM files; the private key is loaded with `SSL_CTX_use_PrivateKey_file` and checked with `SSL_CTX_check_private_key`; an unreadable or group/world-readable key file is a startup error (SECURITY_DESIGN rule 7); the key never appears in a log or an `Error`; the chain is loaded with `SSL_CTX_use_certificate_chain_file`. Validity: a certificate that is expired or not yet valid at load time is refused (EC-T03). Key sizes: RSA below 2048 and curves below 256 bits are refused (`SSL_CTX_set_security_level(2)` is set explicitly, not left to the distribution default).

### 6. Certificate reload (FR-T1, EC-T03)
`TlsServerContext::reload()` builds a complete new `SSL_CTX` from the files, validates it (same checks as the first load), and only then publishes it with one atomic shared-pointer store. New connections take the current pointer in `wrap`; connections already in flight keep the old context (OpenSSL reference-counts it), so nothing is torn down mid-handshake. **A failed reload leaves the old context in place** and reports an error (log now; the alert metric arrives with M10 metrics); the process keeps serving. No lock is taken on a connection path: the swap is an atomic pointer exchange and each `wrap` call takes one atomic load. Triggers: `SIGHUP` in `wsrvd` (through the existing `SignalWatcher`) and an explicit call for embedders.

### 7. SNI (several certificates on one port), bounded
A configuration may carry a default certificate plus up to 64 name entries (exact host names, or one leading `*.` wildcard label). The SNI callback picks the entry by case-insensitive match and falls back to the default; an unknown name gets the default certificate, never an error (clients that send an IP literal, or none, still work). The name from the wire is length-checked (RFC 6066: at most 255 bytes, host-name type only) before it is compared and is never logged unescaped. Each entry reloads with the rest.

### 8. Configuration and build
- `ServerConfig` carries no TLS fields (the server core does not know TLS). `TlsConfig` (certificate chain path, key path, optional SNI entries) belongs to `tls/` and to the composition roots: `wsrvd` and the C embedded config get the keys `tls_cert`, `tls_key` and `tls_sni` (repeatable `name=cert:key`), parsed by the one `EmbeddedConfig` parser, so the library and the daemon behave alike, as for the handshake hook (ADR-0018 amendment 3).
- CMake: `WSRV_ENABLE_TLS` (already declared, default OFF) becomes **ON by default**, because the owner made TLS a built-in; `find_package(OpenSSL 3.0 REQUIRED)` runs only when it is ON. `-DWSRV_ENABLE_TLS=OFF` still builds the zero-dependency core (ADR-0005) and a config with `tls_cert` then fails at start with a clear `InvalidConfig`. `OpenSSL::SSL` and `OpenSSL::Crypto` are linked **privately** to `wsrv_tls`; libraries link OpenSSL dynamically so the system's security updates reach us (documented in OPERATIONS and the packaging notes).
- The CI jobs run on `ubuntu-24.04`, whose image has the OpenSSL development package; if a job lacks it, the workflow installs it (CI, not a developer machine).

### 9. Tests that need no download
- Certificates are made **in the test process** with the OpenSSL API (`tests/support/tls_fixtures`): a self-signed server chain, an expired one, a not-yet-valid one, a key that does not match, a weak RSA key, a wildcard name. No `openssl` CLI, no files in the repo, no fixed secrets to leak.
- `TlsTransport` unit tests use a scripted inner transport and an OpenSSL **client** in the same process, joined by a pair of in-memory pipes whose delivery is controlled: byte at a time (EC-T01), a `WouldBlock` at every step, a short write at every size (EC-T06), a plaintext HTTP request into the TLS side and a ClientHello into the plain side (EC-T02), `close_notify` versus a cut connection (EC-T04), a renegotiation request (EC-T05), a weak client per protocol / cipher (T12).
- An end-to-end test runs the real `Server` with TLS on loopback and an OpenSSL client that performs the WebSocket handshake, echo and close (EC-T07: a client that connects and stays silent is closed at `handshake_timeout`).
- A fuzz target `fuzz_tls_server` feeds arbitrary bytes to a server-side `TlsTransport` (certificate generated once in the process) and asserts it never crashes, never returns data before the handshake completes, and terminates. Added in slice 3 with the CI target list and corpus replay, like the earlier targets.
- **Grade A with `testssl.sh` and Autobahn over `wss`** (the M8 exit criteria) need tools that are not in the dev image. They are run in CI or when the owner approves a download (metered connection rule), and are not claimed before they run.

### 10. Slices
1. This ADR and the doc updates (`ARCHITECTURE`, `REQUIREMENTS`, `SECURITY_DESIGN`, `PROTOCOL_EDGE_CASES`, `BUILD_AND_TOOLING`, `PROJECT_PLAN`).
2. `tls/` skeleton: build option, `ErrorCode::TlsFailure`, `TlsServerContext` (hardened profile, load and validate, tests with generated certificates), include-checker row.
3. `TlsTransport` + the BIO bridge + `ITransport::wants_write` + `Connection` interest + unit tests + fuzz target.
4. Server wiring: `ITransportWrapper`, `LoopShard::adopt`, `ServerBuilder::tls`, the end-to-end `wss` test.
5. `wsrvd` and C config keys, SIGHUP reload, OPERATIONS section.
6. SNI, ALPN policy tests, lesson 22, plan status; then the `testssl.sh` and Autobahn runs when allowed.

## Consequences
+ `wss://` with a hardened default profile, using a library maintained by people who do nothing else; our code owns only the integration, which is small and testable.
+ The protocol, HTTP, hook and IPC layers are untouched (FR-T2); `server` gains one interface and a few lines.
+ Certificate rotation without downtime, with a failed rotation unable to take the service down.
+ The zero-dependency core stays available (`WSRV_ENABLE_TLS=OFF`).
− A new dependency in the default build (mitigated: dynamic link to the system library, optional, behind one module).
− OpenSSL's behavior and error model are large; the BIO bridge and its retry rules are the part most likely to hide bugs, so they carry the most tests (every WouldBlock interleaving) and a fuzz target.
− No session resumption yet, so each reconnect pays a full handshake (a deliberate, documented cost).
− `ITransport` grows a method (default implemented, so `PlainTransport` and test fakes are unaffected).
− One mode per listener until M10 adds multiple listeners.
- Not covered here (follow-ups): client certificates (mTLS), OCSP stapling, session resumption with rotating ticket keys, kernel TLS offload, and TLS for outbound connections (the project has no client mode).

### Amendment 1 (2026-11-04, slice 2: the context)
1. **Key file permissions: "others" only.** Section 5 said "group/world-readable". Refusing group-readable would break the Debian `ssl-cert` convention (mode 0640) and many container secret mounts, so only a key file that "others" can access (any `o` bit) is refused. Group access is the operator's call.
2. **The reload swap uses a mutex, not an atomic pointer.** Section 6 said "an atomic pointer exchange". `std::atomic<std::shared_ptr>` is missing from libc++ 18 (a CI compiler) and the free `std::atomic_load` overloads are deprecated, so `current()` and `reload()` share a mutex that is held only for a pointer copy. It is taken once per accepted connection (in `wrap`), never inside connection or protocol code, so the "no locks in connection code" rule stands.
3. **ALPN moves to slice 2.** The policy is a callback on the context, so it is built and tested with the context (section 10 listed it under slice 6).
4. **Passphrase-protected keys are refused.** OpenSSL would prompt on the terminal; the context installs a password callback that supplies none, so such a key fails to load with "cannot read the private key". Operators who need a passphrase terminate TLS in front (a follow-up if wanted).
5. **Validation order:** certificate chain, then validity dates, then the key (readable, permissions, matches the leaf). The leaf is the first certificate of the chain file.
6. **`ErrorCode::TlsFailure` is added in slice 3**, with its first user (the transport); slice 2 reports configuration problems as `InvalidConfig`.

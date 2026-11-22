# Learning Track

We teach as we build. Each lesson answers three questions: **What** are we building? **Why** does it exist? **How** do we build it? Lessons are written alongside the code they explain, so they never go stale.

| # | Lesson | Milestone | Status |
|---|---|---|---|
| 00 | [From wire to WebSocket: the packet-level picture](00-wire-to-websocket.md) | before M0 | written |
| 01 | [Build system and quality gates](01-build-system-and-quality-gates.md) | M0 | written |
| 02 | [`Result`, bytes, and checked arithmetic](02-result-bytes-and-checked-arithmetic.md) | M1 | written |
| 03 | [SHA-1 and Base64 from scratch](03-sha1-and-base64.md) | M1 | written |
| 04 | [UTF-8 as a state machine](04-utf8-as-a-state-machine.md) | M1 | written |
| 05 | [Parsing HTTP strictly](05-parsing-http-strictly.md) | M2 | written |
| 06 | [The handshake rules](06-the-handshake-rules.md) | M2 | written |
| 07 | [The frame codec: bits, masking, lengths](07-the-frame-codec.md) | M3 | written |
| 08 | [Messages: fragmentation, control frames, streaming](08-messages-fragmentation-and-streaming.md) | M3 | written |
| 09 | [The protocol session: Pong, close handshake, timers](09-the-protocol-session.md) | M4 | written |
| 10 | [Sockets, epoll, and the event loop](10-sockets-epoll-and-the-event-loop.md) | M5 | written |
| 11 | [Connections and the server](11-connections-and-the-server.md) | M6 | written |
| 12 | [Streaming a message](12-streaming-a-message.md) | M7 | written |
| 13 | [Threads without locks](13-threads-without-locks.md) | M7 | written |
| 14 | TLS as a decorator | M8 | planned |
| 15 | [The C ABI: a thin door for every language](15-the-c-abi.md) | M11 | written (the PHP half arrives with M12) |
| 16 | [The sidecar: moving events without losing any](16-the-sidecar-pump-and-backpressure.md) | M11 | written |
| 17 | [The PHP sidecar client: a binding that only translates](17-the-php-sidecar-client.md) | M12 | written |
| 18 | [Asking the application who may connect](18-asking-the-application-who-may-connect.md) | M11 | written |
| 19 | [The question crosses the C boundary](19-the-question-crosses-the-c-boundary.md) | M11 | written |
| 20 | [Asking the application through a socket](20-asking-through-a-socket.md) | M11 | written |
| 21 | [Authenticating a WebSocket connection from PHP](21-authenticating-from-php.md) | M11 | written |
| 22 | [A hardened TLS context](22-a-hardened-tls-context.md) | M8 | written (slice 2; extended as M8 lands) |
| 23 | [Pumping TLS through memory](23-pumping-tls-through-memory.md) | M8 | written (slice 3) |
| 24 | [Wiring TLS into the server](24-wiring-tls-into-the-server.md) | M8 | written (slice 4) |
| 25 | [TLS from configuration: failing closed, reload](25-config-reload-and-failing-closed.md) | M8 | written (slice 5) |
| 26 | [Choosing a certificate by name (SNI)](26-choosing-a-certificate-by-name.md) | M8 | written (slice 6) |
| 27 | [Compressing messages safely (permessage-deflate)](27-compressing-messages-safely.md) | M9 | written (slices 1-4) |
| 28 | [Lifetimes across callbacks (a use-after-free found by review)](28-lifetimes-across-callbacks.md) | M9 | written (security review) |
| 29 | [Who reports the end? (four small bugs from a code review)](29-who-reports-the-end.md) | M10 | written (slice 0) |
| 30 | [One client, one budget (per-IP limits)](30-one-client-one-budget.md) | M10 | written (slice 1) |
| 31 | [Fail at start, not under load (descriptor budget)](31-fail-at-start-not-under-load.md) | M10 | written (slice 1b) |
| 32 | [Counting without a lock (metrics)](32-counting-without-a-lock.md) | M10 | written (slice 2) |
| 33 | [One message, many readers (the pub/sub broker)](33-one-message-many-readers.md) | M10 | written (slice 3) |
| 34 | [An application on the broker (chat example)](34-an-application-on-the-broker.md) | M10 | written (slice 4a) |
| 35 | [Evidence, not adjectives (release readiness)](35-evidence-not-adjectives.md) | M10 | written (slice 4c) |
| 36 | [A C host proves the ABI (the C consumer passes Autobahn)](36-the-c-host-proves-the-abi.md) | M11 | written (exit criterion) |
| 37 | [PHP calls the C ABI through FFI](37-php-calls-the-c-abi-through-ffi.md) | M12 | written (slice 2) |
| 38 | [The login check that never runs (handshake hook, embedded)](38-the-login-check-that-never-runs.md) | M12 | written (slice 3) |
| 39 | [A chat where the bounds are yours (PHP chat)](39-a-chat-where-the-bounds-are-yours.md) | M12 | written (slice 4) |
| 40 | [Declining an offer, remembering a window (deflate edge cases)](40-declining-and-remembering-in-deflate.md) | M12 | written |
| 41 | [Shipping a native library with a PHP package](41-shipping-a-native-library-with-a-php-package.md) | M12 | written |

Lesson template: *Goal → Background (the problem) → Design (why this way) → Walkthrough (code, small steps) → Tests (what proves it works) → Pitfalls → Exercises.*

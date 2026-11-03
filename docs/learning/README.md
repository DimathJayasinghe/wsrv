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

Lesson template: *Goal → Background (the problem) → Design (why this way) → Walkthrough (code, small steps) → Tests (what proves it works) → Pitfalls → Exercises.*

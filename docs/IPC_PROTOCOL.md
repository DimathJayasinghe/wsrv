# IPC protocol v0.3 (`wsrvd`)

The wire contract between the `wsrvd` sidecar and ONE application (ADR-0017). Any language that can open a Unix socket can implement a client in a few hundred lines. The reference implementation of the codec is `src/ipc/` (sans-I/O, fuzzed); the behaviour described here is implemented in `apps/wsrvd/`.

Terms: **daemon** = `wsrvd`; **application** = the program that connected to its Unix socket; **connection** = one WebSocket connection, identified by a `u64` id that is never reused.

## 1. Transport and security
- A Unix domain stream socket at the path given by `ipc_socket`. The daemon creates it with mode `0600`.
- After `accept`, the daemon reads the peer's uid from the kernel (`SO_PEERCRED`). A peer whose uid differs from the daemon's is dropped **without any reply**.
- Optional shared token (`ipc_token_file`, a file of mode `0600`), sent in `HELLO`, compared in constant time. With no token configured, only the uid check protects the socket; the daemon warns at startup.
- One application at a time. A second connection receives `STATUS(request 0, BACKPRESSURE)` and is closed.

## 2. Framing
Every frame, both directions:

```
u32 length    big-endian; counts the bytes AFTER this field (type + flags + body), at least 2
u8  type
u8  flags     reserved; MUST be 0
... body      layout per type; all integers big-endian; no padding; no trailing bytes
```

- The format is canonical: one message has exactly one encoding.
- A frame whose declared length exceeds the limit is a protocol error. The receiver closes the connection **before** allocating or waiting for the body. Before `HELLO` is accepted the limit is the size of the largest legal `HELLO` (264 bytes); afterwards it is `max_frame` from `HELLO_OK` (default `max_message_size` + 64 KiB, hard cap 64 MiB).
- An unknown type, non-zero flags, a wrong body size, or trailing bytes is a protocol error. **The receiver closes the connection with no reply**: a length-prefixed stream cannot be resynchronised, so unknown frames are never skipped.

## 3. Frame types

| type | name | direction | body |
|---|---|---|---|
| 1 | `HELLO` | app to daemon | `u16 major, u16 minor, u16 token_len, token` (token at most 256 bytes) |
| 2 | `HELLO_OK` | daemon to app | `u16 major, u16 minor, u32 max_frame` |
| 3 | `SEND_TEXT` | app to daemon | `u32 request, u64 conn, payload` (UTF-8 text) |
| 4 | `SEND_BINARY` | app to daemon | `u32 request, u64 conn, payload` |
| 5 | `CLOSE` | app to daemon | `u32 request, u64 conn, u16 code, reason` (at most 123 bytes) |
| 6 | `PING` | app to daemon | empty |
| 7 | `PONG` | daemon to app | empty |
| 8 | `EVENT` | daemon to app | `u8 kind, u64 conn, u16 close_code, u8 clean, data` |
| 9 | `STATUS` | daemon to app | `u32 request, u8 status` |
| 10 | `HANDSHAKE_ACCEPT` | app to daemon | `u32 request, u64 conn, subprotocol` (empty = none; at most 256 bytes) (0.2) |
| 11 | `HANDSHAKE_REJECT` | app to daemon | `u32 request, u64 conn, u16 http_status` (0.2) |
| 12 | `SUBSCRIBE` | app to daemon | `u32 request, u64 conn, topic` (topic = the rest of the frame) (0.3) |
| 13 | `UNSUBSCRIBE` | app to daemon | `u32 request, u64 conn, topic` (0.3) |
| 14 | `PUBLISH` | app to daemon | `u32 request, u8 kind (1 text, 2 binary), u64 except (0 = nobody), u16 topic_len, topic, payload` (0.3) |

`conn` is never 0. Sending a daemon-only type (2, 7, 8, 9) is a protocol error, as is a second `HELLO`.

### `EVENT`
`kind`: 1 Open (data = peer address `host:port`), 2 Text (a whole, valid UTF-8 message), 3 Binary, 4 Close (data = close reason, at most 123 bytes), 5 Error (data = a short error name; best effort), 6 Handshake (0.2, only when the daemon runs with `handshake_hook`; see section 9).
`close_code` and `clean` are meaningful only for Close and are 0 / 0 otherwise. `close_code` may be 1005 or 1006 (reported by the core, never sent on the wire); `clean` is true if the closing handshake completed.
Per connection the order is always Open, then messages, then exactly one Close. A Handshake event comes before the Open of the same `conn` if the upgrade is accepted; a refused upgrade produces neither an Open nor a Close.

### `STATUS`
Answers every command (`SEND_*`, `CLOSE`, `HANDSHAKE_ACCEPT`, `HANDSHAKE_REJECT`) with the command's `request` id. `status` uses the values of `wsrv_status` in `include/wsrv/wsrv.h` (0 OK, 2 INVALID_ARGUMENT, 5 NO_SUCH_CONN, 6 BACKPRESSURE, 7 INVALID_UTF8, ...). **OK means accepted for sending, not delivered** (ADR-0016 section 5).
- `request` is chosen by the application; number commands from 1. **Request id 0 is reserved**: a command with id 0 is refused with `INVALID_ARGUMENT` and not executed, and a `STATUS` with request 0 can only be about the handshake.
- A command for a connection the application was not told about, or that has ended, gets `NO_SUCH_CONN`. (Handshake answers have their own rule, section 9.)

### Broker commands (0.3, ADR-0027 and ADR-0028)
Need a daemon started with `broker = true` (otherwise every one answers `INVALID_ARGUMENT`) and a `HELLO` that says minor 3 or higher: an application that said less and sends frame 12, 13 or 14 is a protocol error (the link closes with no reply).
- **`SUBSCRIBE` / `UNSUBSCRIBE`**: put one of *your* connections in a topic, or take it out. The same rule as `SEND_*`: a connection the application was not told about is `NO_SUCH_CONN`. The decision to let a browser join a topic is the application's (the daemon never reads a topic from a browser). A bound refused (topics, subscribers per topic, topics per connection, total) is `LIMIT` (12), not `BACKPRESSURE`: waiting does not help until someone leaves. A bad topic name is `INVALID_ARGUMENT`. A subscription ends when its connection closes, and also at once when the application disconnects (the daemon drops them for every connection that application knew; they close with 1012 after the grace period as before).
- **`PUBLISH`**: send to every subscriber of `topic` except `except`. `OK` means the publish was valid and handed to the broker; the per-subscriber report (how many were accepted or refused) is not in this answer (the HTTP publish endpoint of ADR-0028 returns it). Text that is not UTF-8 is `INVALID_UTF8`. Request id 0 is refused as everywhere.
- The topic is not length-prefixed in 12 and 13 (it is the rest of the frame); in 14 it has a `u16` length because the payload follows.

## 4. Session
1. The application connects and sends `HELLO` within `ipc_hello_timeout_ms` (default 5000).
2. The daemon answers `HELLO_OK`. A wrong major version, wrong token, or any other first frame gets one `STATUS(request 0, INVALID_ARGUMENT)` and the connection is closed; the reply is the same for every cause.
3. Events start to flow. The application sends commands at any time and reads `STATUS` answers and `EVENT`s interleaved.
4. `PING` is answered with `PONG` at any time.

## 5. Backpressure and failure
- The daemon's output buffer is bounded. When it is full, the daemon **stops taking events** from the WebSocket server. Events then wait in the server's bounded queue, whose limits pause the WebSocket clients (their TCP windows fill). Nothing is dropped. The daemon also stops reading commands from a socket whose output it cannot drain, so an application cannot make it buffer unbounded answers.
- If the application leaves the daemon's output unread for `ipc_stall_ms` (default 10000, range 100 to 600000), counted from the last byte the socket accepted, the daemon closes the link and closes every connection the application knew with **1001**.
- Applications must therefore read continuously (or use a dedicated reader thread).

## 6. No application connected, and application restarts
- **Before any application connects**, the daemon takes no events. WebSocket clients complete their handshake and are held behind backpressure; the first application receives everything in order.
- **When the application disconnects** (without stalling), the connections it knew are closed with **1012 (Service Restart)** after `shutdown_grace_ms` (default 10000), because their application state is gone. Their remaining events are discarded: this is the only place the daemon discards events. A new application connecting within the grace period still sees none of their traffic and gets `NO_SUCH_CONN` for their ids.
- Connections the application never saw are served by the next application.
- Delivery is at most once across a disconnect: events already written to a dying link are lost with it.

## 7. Versioning
Major version in `HELLO` must equal the daemon's (currently 0); minors are additive. `HELLO_OK` carries the daemon's own minor (currently 3). **Freeze (decision for 1.0, 2026-11-23): the frames and fields of `0.3` are the contract of the whole wsrv 1.x line.** Nothing is removed or changed; a new minor only adds frames, fields at the end of a frame, or status values; a major bump (an old client would refuse to start) happens only with wsrv 2.0. Before 1.0 the frame set could change between minors with an ADR.

Minor 3 added the broker frames (12 to 14) and the status value `LIMIT` (12). Minor 2 added the handshake hook. A daemon started **with** `handshake_hook` refuses (like any other bad `HELLO`: `STATUS(request 0, INVALID_ARGUMENT)`) an application whose `HELLO` says minor below 2, because that application could not read a Handshake event and every browser would wait for a 503. Without `handshake_hook` an older minor is fine.

## 8. Reference clients
- PHP: `bindings/php` (`Wsrv\Client`, `Wsrv\Worker`).
- C++ (tests): `tests/support/ipc_client.hpp`, a blocking client with a receive timeout. It speaks the protocol through the same `ipc/` codec the daemon uses; copy it as the starting point for a test of your own application against a real `wsrvd`.

## 9. The handshake hook (0.2, ADR-0018)
Off by default. With `handshake_hook = true` (plus `handshake_forward_header`, `handshake_decision_timeout_ms`, `max_pending_handshakes`; see `docs/OPERATIONS.md`), no upgrade is accepted until the application says so. This is how an application authenticates a WebSocket connection with the cookie or token the browser sent.

1. A browser's upgrade passes the daemon's own checks (syntax, Origin, version). The daemon then sends the application an `EVENT` of kind 6 with `conn` = **the id the connection will have if accepted**, and sends the browser nothing yet.
2. The application answers **once** within `handshake_decision_timeout_ms` (default 3000): `HANDSHAKE_ACCEPT` (optionally with one of the subprotocols the client offered) or `HANDSHAKE_REJECT` with an HTTP status. Each answer is itself answered with a `STATUS`:
   - `OK`: handed to the connection's loop (as for every command, not "delivered").
   - `NO_SUCH_CONN`: no such question is open: already answered, the daemon gave up (timeout, the browser left, shutdown), or never asked. Late and repeated answers are harmless.
   - `INVALID_ARGUMENT`: request id 0, a subprotocol the client did not offer, or a status outside 400, 401, 403, 404, 429, 500, 503. The question stays open; answer again.
3. Accept: the browser gets `101` and the application then receives the usual `Open` with the same `conn`. Reject: the browser gets that status with a fixed reason text (nothing the application sends reaches the browser); 429 and 503 carry `Retry-After: 5`.
4. **Fail closed.** No answer in time, a full queue, shutdown, or no application connected means `503`, never an accepted connection.

`HANDSHAKE_ACCEPT` and `HANDSHAKE_REJECT` are not checked against "connections the application was told about" (the connection does not exist yet); the daemon asks the WebSocket server, which knows which questions are open.

### The Handshake event body
`data` is one blob, all integers big-endian, and every string is `u32 length` + that many bytes (not NUL-terminated; any byte value except what HTTP itself forbids):

```
string peer          "host:port"
string path          request target up to '?'
string query         after '?', raw (not percent-decoded); empty if none
string origin        the Origin header (it already passed allow_origin); empty if none
string host          the Host header
string subprotocol   what the server's own rules chose; may be empty
u16 offered_count,  then that many strings: every subprotocol the client offered
u16 header_count,   then that many (name string, value string): the listed forward headers
```
The blob ends exactly after the last header: trailing bytes mean a corrupt event. Headers are only those listed in `handshake_forward_header`, in request order, **every** value of a repeated header as its own entry, with the name spelled as configured (`Cookie` and `Authorization` are never sent unless listed). A value over 4 KiB, or more than 16 KiB in total, makes the daemon refuse the upgrade with 431 itself.

### While nobody can answer
The daemon asks questions only while an application is connected. With none (before the first, or after one left), every upgrade is refused with `503` **at once**, and when an application disconnects the questions it had not answered are refused with `503` too. This replaces the "held behind backpressure" behaviour of section 6, which still applies to daemons without the hook. If a question would not fit one frame (the client made it big), the daemon refuses that upgrade with `503` and the link stays up.


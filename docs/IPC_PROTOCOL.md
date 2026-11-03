# IPC protocol v0 (`wsrvd`)

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

`conn` is never 0. Sending a daemon-only type (2, 7, 8, 9) is a protocol error, as is a second `HELLO`.

### `EVENT`
`kind`: 1 Open (data = peer address `host:port`), 2 Text (a whole, valid UTF-8 message), 3 Binary, 4 Close (data = close reason, at most 123 bytes), 5 Error (data = a short error name; best effort).
`close_code` and `clean` are meaningful only for Close and are 0 / 0 otherwise. `close_code` may be 1005 or 1006 (reported by the core, never sent on the wire); `clean` is true if the closing handshake completed.
Per connection the order is always Open, then messages, then exactly one Close.

### `STATUS`
Answers every command (`SEND_*`, `CLOSE`) with the command's `request` id. `status` uses the values of `wsrv_status` in `include/wsrv/wsrv.h` (0 OK, 2 INVALID_ARGUMENT, 5 NO_SUCH_CONN, 6 BACKPRESSURE, 7 INVALID_UTF8, ...). **OK means accepted for sending, not delivered** (ADR-0016 section 5).
- `request` is chosen by the application; number commands from 1. **Request id 0 is reserved**: a command with id 0 is refused with `INVALID_ARGUMENT` and not executed, and a `STATUS` with request 0 can only be about the handshake.
- A command for a connection the application was not told about, or that has ended, gets `NO_SUCH_CONN`.

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
Major version in `HELLO` must equal the daemon's (currently 0); minors are additive. Version 0 is not frozen: until 1.0, the frame set may change between minors with an ADR.

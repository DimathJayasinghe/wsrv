# Lesson 00 — From wire to WebSocket: the packet-level picture

## Goal
Understand exactly which bytes our server will read and write, and which layer owns each part. Everything we build is a consequence of this picture.

## 1. The stack: who handles what

```
Your PHP code / any app            ← messages ("hello")
─────────────────────────────────
WebSocket frames (RFC 6455)        ← WE parse/build these       (M3, M4)
HTTP/1.1 Upgrade handshake         ← WE parse/build this        (M2)
TLS (optional, wss://)             ← WE drive OpenSSL           (M8)
─────────────────────────────────
TCP byte stream                    ← the KERNEL (we use sockets)
IP packets / Ethernet frames       ← the KERNEL + network card
```

"Packet level" for us means **WebSocket frame level**: we decode every bit of every frame ourselves. We do *not* craft raw IP/TCP packets — the operating system's TCP implementation does retransmission, ordering, and congestion control for us, and re-implementing it would be a different (huge) project with no benefit. We read and write a **byte stream** through a socket.

**Key fact: TCP is a stream, not messages.** If a client sends 100 bytes, our `read()` may return 1 byte, 37 bytes, or 100 bytes, and two messages may arrive glued together. Our parsers must work with *any* chunking. This single fact drives the "incremental parser" design in the whole project (requirement FR-F1, edge cases EC-F13/F14).

## 2. Step 1 — the HTTP handshake

A WebSocket starts as an ordinary HTTP request asking to "upgrade" the connection:

```
GET /chat HTTP/1.1
Host: example.com
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==
Sec-WebSocket-Version: 13
Origin: https://app.example.com
```

The server proves it understands WebSocket by answering:

```
HTTP/1.1 101 Switching Protocols
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
```

**How the accept value is made** (RFC 6455 §4.2.2):
`Base64( SHA-1( key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11" ) )`
For the key above the result is `s3pPLMBiTxaQ9kYGzzhZRbK+xOo=` (we verified this independently; it becomes our first test, EC-H09).

**Why this exists:** it stops a normal HTTP client or a confused proxy from accidentally "speaking WebSocket". It is *not* security against attackers — it only proves protocol awareness. Real protection comes from the Origin check (stops other websites hijacking a user's session, "CSWSH") and TLS.

**Why we need SHA-1 and Base64:** those two algorithms are all the handshake needs. Writing them ourselves keeps the core dependency-free (ADR-0005).

## 3. Step 2 — frames

After the 101 response, both sides send **frames**. Layout (each cell is a bit):

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-------+-+-------------+-------------------------------+
|F|R|R|R| opcode|M| Payload len |    Extended payload length    |
|I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
|N|V|V|V|       |S|             |   (if payload len==126/127)   |
| |1|2|3|       |K|             |                               |
+-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
|     Extended payload length continued, if payload len == 127   |
+ - - - - - - - - - - - - - - - +-------------------------------+
|                               | Masking-key, if MASK set to 1 |
+-------------------------------+-------------------------------+
| Masking-key (continued)       |          Payload Data         |
+-------------------------------- - - - - - - - - - - - - - - - +
```

| Field | Meaning | Why |
|---|---|---|
| FIN | 1 = last fragment of a message | Messages can be split (streaming big data) |
| RSV1–3 | Reserved, must be 0 unless an extension says otherwise | Room for extensions (compression uses RSV1) |
| opcode | `0x0` continuation, `0x1` text, `0x2` binary, `0x8` close, `0x9` ping, `0xA` pong | Says what the frame is |
| MASK | 1 if payload is XOR-masked | Clients **must** mask; servers **must not** |
| Payload len | 0–125 literal; 126 → next 2 bytes; 127 → next 8 bytes | Compact for small messages |
| Masking-key | 4 random bytes | See below |

**Why masking exists:** not for secrecy. Early proxies could be tricked by a malicious page into caching attacker-chosen bytes that looked like HTTP. Per-frame random masking prevents attackers from controlling the exact bytes on the wire. The algorithm is trivial: `payload[i] ^= key[i % 4]`.

### Worked example (RFC 6455 §5.7): client sends text "Hello"

Bytes on the wire: `81 85 37 fa 21 3d 7f 9f 4d 51 58`

| Bytes | Decode |
|---|---|
| `81` = `1000 0001` | FIN=1, RSV=000, opcode=1 (text) |
| `85` = `1000 0101` | MASK=1, payload length = 5 |
| `37 fa 21 3d` | masking key |
| `7f 9f 4d 51 58` | masked payload; XOR with key repeating gives `48 65 6c 6c 6f` = "Hello" |

(We checked the XOR result by script: `Hello` ^ key = `7f9f4d5158`.) This exact vector will be one of our first frame-codec tests.

The server's reply, an *unmasked* "Hello": `81 05 48 65 6c 6c 6f`.

### Fragmentation and control frames
(Two different things are called "fragmentation": TCP delivering bytes in arbitrary chunks, and a *message* split into several frames. Both are handled; the full design is in [../FRAGMENTATION.md](../FRAGMENTATION.md).)

A large message may be sent as several frames: first frame has the real opcode and FIN=0, following frames have opcode `0x0`, the last has FIN=1. **Control frames** (close/ping/pong) may appear *between* fragments, are ≤125 bytes, and are never fragmented. That interleaving is where many implementations break, so our state machine models it explicitly (M4).

### Closing
Either side sends a Close frame (opcode `0x8`) with a 2-byte code (1000 = normal, 1002 = protocol error, 1007 = bad data, 1009 = too big, …) and optional UTF-8 reason; the other side answers with Close; then TCP closes. We enforce a timeout in case the peer never answers.

## 4. Why we design it this way (the consequences)

| Fact from the wire | Design consequence |
|---|---|
| TCP delivers arbitrary chunks | Parsers are **incremental state machines** that can pause mid-field |
| Peer controls every byte and length | **Validate before use, bound everything**: a frame header can claim 2⁶³ bytes — we must never allocate from it |
| Parsing is pure byte logic | **Sans-I/O core** (ADR-0002): feed bytes in, get events/bytes out; no sockets inside, so we can test with byte arrays and fuzz it |
| Text must be valid UTF-8, possibly split across fragments | UTF-8 validator is an **incremental DFA** whose state survives between chunks |
| Slow or malicious peers exist | Timeouts, rate limits, backpressure (ADR-0007) |

## 5. What we build next, and why in this order
1. **M0 skeleton** — build system and quality gates first, so every later line is compiled with strict warnings and sanitizers from day one.
2. **M1 base+crypto** — `Result`, byte helpers, SHA-1, Base64, UTF-8: small, fully specified, perfect for learning TDD with published test vectors.
3. **M2 handshake**, **M3 frames**, **M4 state machine** — the protocol, all testable without a network.
4. **M5+ networking** — only after the protocol is solid do we touch sockets, epoll, threads, TLS.

## 6. Exercises
1. Decode by hand: `88 82 aa bb cc dd a9 51` (hint: opcode 8 is Close; unmask the 2-byte payload; what close code is it?). 
2. A client sends length bytes `7e 01 00`. How long is the payload? Is it a valid minimal encoding? (EC-F04)
3. Why must the server reject an *unmasked* frame from a browser client but send unmasked frames itself?
4. Sketch the states a parser needs if it receives only one byte per `read()`.

<details><summary>Answers</summary>

1. Key `aa bb cc dd`; `a9^aa = 03`, `51^bb = ea` → `03 ea` = 1002 → Close, protocol error.
2. `0x0100` = 256 bytes; valid (≥126 uses the 16-bit form).
3. Masking is a client→server defense (proxy cache poisoning); the RFC requires it from clients, and rejecting unmasked ones also catches non-WebSocket clients speaking the wrong protocol.
4. Roughly: read first 2 bytes → (maybe) read 2 or 8 length bytes → (if masked) read 4 key bytes → read payload in chunks → emit frame/event → back to start.
</details>

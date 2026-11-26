# Fragmentation & Message Assembly

"Fragmentation" means two different things in this project. Mixing them up is a classic source of bugs, so we name them separately.

| Term | Layer | Who decides | Our rule |
|---|---|---|---|
| **Stream chunking** | TCP | The network and kernel: `read()` returns any number of bytes | Every parser must be correct for *any* chunking, including 1 byte at a time |
| **Message fragmentation** | WebSocket (RFC 6455 §5.4) | The sender splits one message into several frames (`FIN=0` … `FIN=1`) | Receive: reassemble, validate, bound. Send: split large messages automatically, support streaming |

A single WebSocket frame can arrive in many chunks, and one chunk can contain many frames and pieces of several. Fragmentation of messages is independent of chunking.

## 1. Receive side

### 1.1 Layers of reassembly

```
bytes (any chunking) ─► FrameParser ─► frame header + payload slices ─► MessageAssembler ─► message / message parts
        (stream chunking handled here)               (message fragmentation handled here)
```

- **FrameParser** keeps only a tiny accumulator (header ≤ 14 bytes: 2 + up to 8 length + 4 mask key). The payload is *streamed* through: unmasked and passed on slice by slice, with the mask offset (`i mod 4`) carried across chunks. It never buffers a whole frame and never allocates from the declared length (EC-F06, EC-F22).
- **MessageAssembler** owns message-level state and never sees chunking.

### 1.2 Assembler state machine

```
              data frame, FIN=1                       control frame (any state):
        ┌────────────────────────────┐                handled immediately,
        │                            ▼                assembler state untouched
     ┌──┴───┐  text/binary, FIN=0  ┌──────────────┐
     │ Idle ├─────────────────────►│ InMessage    │◄─┐ continuation, FIN=0
     └──┬───┘                      │ opcode, bytes│  │ (append, check limits)
        ▲                          │ frags, utf8  ├──┘
        │   continuation, FIN=1    └──────┬───────┘
        └─────────────────────────────────┘ (deliver message)

 Idle     + continuation        → close 1002 (EC-F09)
 InMessage + text/binary frame  → close 1002 (EC-F10)
```

Per-fragment checks, in this order (fail closed at the **first** violation):
1. Opcode/sequence legality (table above).
2. RSV bits: RSV1 allowed only on the *first* frame of a message and only if permessage-deflate is negotiated; never on continuation or control frames (EC-F24, EC-D02).
3. Fragment count ≤ `max_fragments` → else 1008 (EC-F20, EC-F28).
4. Total size (checked arithmetic) ≤ `max_message_size` → else 1009 at the offending fragment (EC-F19, EC-F21).
5. Text only: feed bytes to the incremental UTF-8 validator; invalid ⇒ 1007 immediately, without waiting for `FIN` (EC-U06).
6. Timer: a *fragmented* message, measured from the end of its first fragment, must reach FIN within `incomplete_message_timeout` (default 30 s) → else 1008 (EC-F27). One slow single frame is covered by the idle timer (no complete frame for `idle_timeout`). This stops a peer holding buffers forever by never sending the final fragment.

A zero-length final continuation frame is valid and completes the message (EC-F37). A Close frame received mid-message starts the close handshake and the partial message is discarded, never delivered (EC-F26).

### 1.3 Delivery modes (handler chooses)

| Mode | Handler callback | Memory | Use when |
|---|---|---|---|
| **Buffered** (default) | `on_text(Session&, string_view)` / `on_binary(...)` — complete message | ≤ `max_message_size` per message | Chat, JSON, small/medium messages. Simplest to use correctly |
| **Streaming** (opt-in) | `on_message_part(Session&, MessagePart, bool final)` | O(chunk) — constant | Large uploads, media, proxying. Limit still enforced, but may be set higher |

Rules:
- Buffered mode of a **single-frame** message that is fully present in the read buffer is zero-copy (a view into the buffer). Fragmented or split messages are assembled into one contiguous pooled buffer that grows geometrically but never beyond `max_message_size`, and never pre-sized from peer-declared lengths.
- In streaming mode, **text parts always end on UTF-8 code-point boundaries**: the validator holds back an incomplete trailing sequence until the next chunk. Handlers can therefore decode each part independently (EC-U06, EC-F29). When the held-back bytes are completed by the next chunk, the part is delivered as two views, `data` (the 1-4 byte character, from a fixed internal array) and `tail` (the rest of the chunk, still in the caller's buffer), so no chunk is ever copied and memory is O(1) per connection. Each view ends on a boundary, so they may be passed on as two parts or joined.
- Parts are valid only during the call. If the handler closes the session during a part, the rest of the message is dropped (EC-F30): the assembler's `discard_message()` works even in the middle of a frame; it swallows the rest of that frame and the continuation frames up to FIN without delivering or UTF-8-validating them, but still counts them against the size and fragment limits. The peer may legitimately keep sending them until its own Close arrives (RFC 6455 §5.5.1), so they are not a protocol error; a *new* data frame before that FIN still is (1002).
- Streaming parts respect backpressure: if the handler signals "not ready" (returns `Backpressure` or sets a pause flag), reading from the socket pauses (ADR-0007).
- The mode is per handler (a config choice at build time), not switched mid-message.

### 1.4 Resource accounting
Memory held for partial messages counts against the per-connection and global byte budgets (SECURITY_DESIGN §6). Many connections each holding a nearly-full partial message is a DoS shape we explicitly budget for: since 1.1 a global cap (`max_buffered_bytes`, default 1 GiB, ADR-0040) counts the bytes of partial messages and unwritten output across all connections, and above it **new connections** are refused with a 503 (the ones that exist are left alone) before the process is under memory pressure. See OPERATIONS section 1.2.3.

CPU amplification defence: 1-byte fragments are legal but costly. `max_fragments` (1024) bounds one message; the per-connection **data-frame rate** limit (default 10 000/s) bounds the flood across messages.

## 2. Send side

### 2.1 Auto-fragmentation of large messages
`send_text/send_binary(msg)` is atomic from the caller's view. Internally the `OutboundScheduler` frames the message as:
- one frame if `size ≤ max_outbound_frame` (default 64 KiB), else
- N frames: first has the real opcode, the rest are continuations, last has `FIN=1`; each ≤ `max_outbound_frame`.

**Why:** a 50 MiB message in one frame would monopolise the write path for a long time, delay Pong/Close, and starve other connections on the same loop. Splitting at frame boundaries gives the scheduler places to interleave control frames and yield to the loop (fairness, EC-N08). The peer sees the identical message regardless (EC-F31 round-trip test).

### 2.2 Streaming send (`MessageWriter`)
For data not fully in memory (files, generated streams, proxying):

```cpp
auto w = session.begin_binary();            // Result<MessageWriter>; error MessageInProgress if one is open
w->write(chunk1);                           // may return Backpressure → wait for on_writable
w->write(chunk2);
w->finish();                                // sends FIN=1 (an empty final frame if needed)
```

Rules:
- **One outbound data message at a time per connection** (RFC 6455 §5.4: fragments of different messages cannot interleave). A second `send_*` or `begin_*` while a writer is open returns `Error{MessageInProgress}` and writes nothing (EC-F33).
- A writer destroyed without `finish()` cannot "un-send" earlier fragments, so the connection is closed with 1011 and the event is logged (EC-F34).
- `write()` is non-blocking; when the outbound queue passes its high-water mark it returns `Backpressure` and `on_writable` fires after draining below the low-water mark (EC-F35).
- Text writers: each `write` may split a code point; the writer validates cumulative UTF-8 on `finish()` and on each `write` (fail fast, `Error{InvalidUtf8}`) so we never emit invalid text frames.

### 2.3 Outbound scheduling: control frames jump the queue
```
 control queue (Ping/Pong/Close, small, bounded) ──┐
                                                   ├─► OutboundScheduler ─► write buffer ─► transport
 data queue   (frames of at most one open message) ┘
```
At every **frame boundary** the scheduler picks from the control queue first; a frame already partially written to the socket is always finished first (never interleave inside a frame). So a Pong or Close queued behind a 50 MiB fragmented message goes out after the current fragment, not after the whole message (EC-F32, EC-C01). After a Close frame is queued, further data frames are rejected (`Error{Closing}`, EC-C14) and any not-yet-started fragments are discarded.

### 2.4 Broadcast and compression
- Broadcast frames are built once and shared (`shared_ptr<const FrameList>`); with auto-fragmentation the shared object is the whole fragment list (immutable), so N subscribers cost zero extra framing/copies. A slow subscriber's queue limit applies independently (EC-N10).
- permessage-deflate (RFC 7692): compress the **whole message first**, then fragment the compressed bytes; `RSV1` is set on the first frame only (EC-D02). Messages sent through the streaming writer (`MessageWriter`) are **not compressed** (ADR-0020 section 5): a stream is not known in advance, and the whole-message rule above is what keeps the compressor's history in step with the peer's. Compressing streams, and compressing a broadcast once for all subscribers, are follow-ups (M10). The compressor state of the buffered sends is per connection and memory-budgeted (EC-D05, `docs/OPERATIONS.md` section 1.1).

## 3. API summary (additions)

```cpp
// Handler (all optional; streaming mode selected in ServerBuilder::delivery(Delivery::Streaming))
virtual void on_message_part(Session&, MessagePart part, bool final) {}
struct MessagePart { MessageType type; std::span<const std::byte> bytes; /* text: ends on code-point boundary */ };

// Session
Result<MessageWriter> begin_text();
Result<MessageWriter> begin_binary();
class MessageWriter { Result<void> write(std::span<const std::byte>); Result<void> finish(); ~MessageWriter(); };
```

C ABI equivalents (BINDINGS §3): events `WSRV_EV_TEXT_PART` / `WSRV_EV_BINARY_PART` with an `is_final` flag, and `wsrv_begin_message / wsrv_write_part / wsrv_end_message`. PHP exposes buffered mode by default and an iterator/callback for streaming.

## 4. Configuration

| Setting | Default | Hard ceiling |
|---|---|---|
| `max_message_size` | 1 MiB | 1 GiB (streaming mode only above 64 MiB) |
| `max_fragments` | 1024 | 65 536 |
| `max_outbound_frame` | 64 KiB | 16 MiB |
| `incomplete_message_timeout` | 30 s | 1 h |
| `data_frame_rate` | 10 000 / s / conn | — |
| `delivery` | Buffered | — |

## 5. Test plan (cross-reference)

- Property test: for any message and any partition into fragments and any partition of the byte stream into chunks, delivered message == original (receive), and `reassemble(auto_fragment(m)) == m` (send).
- State-machine matrix test over `(state × opcode × FIN)` (EC-F08–F11).
- Fuzz target `protocol_session` generates fragment sequences with interleaved control frames and arbitrary chunking.
- Autobahn sections 5 (fragmentation), 6 (UTF-8), 12–13 (compressed fragments).
- Edge-case IDs: EC-F08…F11, EC-F19…F20, EC-F24…F37, EC-U06, EC-C01, EC-D02.

## 6. Pitfalls this design avoids
1. Assuming one `read()` = one frame (stream chunking).
2. Allocating from the declared frame length.
3. Validating UTF-8 only after `FIN` (late failure, and buffers held for attackers).
4. Resetting the mask offset or UTF-8 state at chunk/fragment boundaries.
5. Treating control frames mid-message as message data, or answering them late.
6. Letting two outbound messages interleave frames.
7. Large single frames that block Pong/Close and starve other connections.
8. A peer that opens a message and never finishes it.

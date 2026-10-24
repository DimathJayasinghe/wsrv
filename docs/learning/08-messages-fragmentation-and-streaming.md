# Lesson 08 — Messages: fragmentation, control frames and streaming

## Goal
Turn a stream of frames into *messages* correctly, including the awkward cases: messages split into many frames, Ping/Close arriving in the middle, a UTF-8 character cut in half by a fragment boundary, and delivering huge messages without holding them in memory. Code: `src/protocol/message_assembler.*`. Background: `docs/FRAGMENTATION.md`.

## 1. Two kinds of "fragmentation"
- **TCP chunking**: the network delivers bytes in arbitrary pieces. Handled by the frame parser (Lesson 07): its result never depends on chunking.
- **Message fragmentation**: a sender may split one message into several frames: the first carries the real opcode (`Text` or `Binary`) with FIN=0, the following carry opcode `Continuation`, and the last has FIN=1. This lesson is about the second kind.

## 2. The state machine
```text
            data frame FIN=1                       control frame (any time):
      ┌───────────────────────────┐                reported immediately,
      │                           ▼                the message state is untouched
   ┌──┴───┐ Text/Binary FIN=0  ┌──────────┐
   │ Idle ├───────────────────►│ InMessage│◄─┐ Continuation FIN=0
   └──┬───┘                    │ type,size│  │
      ▲                        │ fragments├──┘
      │  Continuation FIN=1    └────┬─────┘
      └─────────────────────────────┘ (message complete)
 Idle + Continuation          -> close 1002 (nothing to continue)
 InMessage + Text/Binary      -> close 1002 (two messages cannot interleave)
```
All checks happen when the frame **header** arrives, before its payload: sequencing, fragment count (more than `max_fragments` → `1008`), and size (`total so far + declared length` over `max_message_size` → `1009`). A client therefore cannot make us read a megabyte just to reject it.

## 3. Control frames in the middle
Ping, Pong and Close may appear between fragments (an application must be able to answer a Ping even during a long upload). The assembler reports each as a `Control` event and leaves the message state alone. Two consequences we test:
- Control frames do not count as fragments and do not count toward the message size.
- A Close frame mid-message is reported; the session then calls `discard_message()` (the partial message is dropped, never delivered). That call is legal at any moment, even in the middle of a frame: the assembler enters a *draining* state and swallows the rest of that frame and the continuation frames up to FIN, still counting them against the limits. Without it, the tail of a half-delivered frame would surface as a "complete" truncated message, or the peer's still-arriving fragments would look like a continuation without a message (1002), when the peer is behaving correctly. A review found exactly that hole; `DiscardInTheMiddleOfAFrame...` and friends now pin it.

## 4. UTF-8 across fragments, with a Ping in the middle
Text must be valid UTF-8, validated **as the bytes arrive** (so an attacker's garbage is rejected at the first bad byte with `1007`, not after the final fragment). The validator state lives across frames, so this works:

```text
Text  fin=0  "a" E2 82        (the euro sign E2 82 AC is cut in half)
Ping  fin=1  "x"              (reported; message state untouched)
Cont  fin=1  AC "b"           (completes the character)
=> Control(Ping "x"), Message "a€b"
```

A message that ends in the middle of a character (`..E2 82` with FIN) is `1007`.

## 5. Two delivery modes
- **Buffered** (default): the application receives the whole message. Memory is bounded by `max_message_size` (default 1 MiB), enforced by the bounded `ByteBuffer` from Lesson 02.
- **Streaming**: the application receives `Part`s as data arrives, with memory proportional to one chunk. Binary parts are zero-copy views of the read buffer. **Text parts always end on a character boundary**: when a chunk ends inside a character, the 1-3 unfinished bytes are held back and delivered with the next part. Delivered byte-at-a-time, `a€b` produces exactly three parts (this is a test): `"a"`, then nothing for `E2` and `82`, then `"€"` when `AC` arrives, then `"b"` with `final=true`. Every part decodes on its own, so the application can process parts independently. Exactly one part per message has `final=true` (an empty one if the last frame had zero length).

## 6. Evidence
- 28 tests through the real parser: each rule above, limits exactly at and one over, and a property test that fragments random messages at random points with interleaved Pings and chunks the wire at random sizes: the original messages always come out.
- Six mutation checks (continuation without start accepted, a new data frame inside a message accepted, no size check, no fragment cap, no hold-back of partial characters, no end-of-message boundary check): each makes tests fail.
- `fuzz_message_pipeline` asserts that buffered and streaming mode, two independent code paths, agree on every message, control frame and failure for arbitrary input, and that every streamed text part is valid UTF-8. When we planted a bug in the hold-back logic, the fuzzer found a 12-byte counterexample in seconds; it is now a permanent regression seed.

## 7. Pitfalls we hit
1. **Order of checks matters for DoS.** Checking `declared length + total` before reading the payload is what prevents a huge allocation; the check uses subtraction (`declared > max - total`) so it cannot overflow.
2. **A view is only valid until the next call.** The `Message` event's data lives in the assembler's buffer, which is cleared lazily at the *next* `handle()`; delivering and clearing in the same call would hand the application a dangling view.
3. **Streaming text never copies the chunk.** A character that spans chunks is completed in a fixed 4-byte array and delivered as `data`; the rest of the chunk follows as `tail`, still a view into the read buffer. (The first version joined held bytes and chunk in a growing vector: peer-influenced allocation of the size of a whole read, kept for the connection's life. The review flagged it.)

## 8. Exercises
1. Frames: `Text fin=0 "ab"`, `Ping "p"`, `Continuation fin=1 "cd"`. Which events come out, in what order?
2. The first frame of a connection is a `Continuation`. Result?
3. `Text fin=0 "ab"` then `Binary fin=1 "x"`. Result?
4. In streaming mode, what are the parts for the text `é` (C3 A9) delivered one byte per read?
5. Why is a Close frame in the middle of a message not simply "the message ends"?

<details><summary>Answers</summary>

1. `Control(Ping, "p")` first, then `Message(Text, "abcd")`.
2. Close code 1002: there is no message to continue.
3. Close code 1002: a new data frame cannot start while a fragmented message is in progress.
4. One part `"é"` with `final=true`: the first byte alone (C3) is held back, so nothing is delivered until A9 completes the character.
5. The message was never completed: delivering half of it would be wrong. The session drops it with `discard_message()` and runs the closing handshake.
</details>

## 9. What's next
M4: the `ProtocolSession`: the state machine that combines parser, assembler, automatic Pong replies, ping/pong heartbeat, the closing handshake, and outbound fragmentation.

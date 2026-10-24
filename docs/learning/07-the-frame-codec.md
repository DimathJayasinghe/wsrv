# Lesson 07 — The frame codec: bits, masking, lengths

## Goal
Read and write WebSocket frames bit by bit, understand why every field exists, and see how a parser that *never trusts a length* stays safe against an 8-byte header that claims a terabyte. Code: `src/protocol/` (`opcode`, `close_code`, `masker`, `frame`, `frame_writer`, `frame_parser`).

## 1. The header, with real bytes
After the handshake everything is frames. A masked client frame carrying the text "Hello" (RFC 6455 §5.7):

```text
81 85 37 fa 21 3d 7f 9f 4d 51 58
│  │  └─ mask key ─┘ └─ masked payload (5 bytes) ─┘
│  └─ 0x85 = 1000 0101: MASK bit set, payload length 5
└─ 0x81 = 1000 0001: FIN=1, RSV=000, opcode 0x1 (text)
```

| Bits | Field | Rules we enforce |
|---|---|---|
| 1 | FIN | control frames must have FIN=1 |
| 3 | RSV1-3 | RSV2/3 never; RSV1 only for a negotiated extension, only on the first frame of a data message |
| 4 | opcode | only `0 1 2 8 9 A`; `3-7` and `B-F` are reserved |
| 1 | MASK | client→server frames **must** be masked, server→client **must not** |
| 7 | length | `0-125` literal; `126` → next 2 bytes; `127` → next 8 bytes |

### Minimal length encoding
A payload of 5 bytes *could* be written as `05`, as `7E 00 05`, or as `7F 00 .. 05`. If two implementations accept different forms, an attacker can exploit the difference (the same disagreement problem as HTTP smuggling in Lesson 05). So the rule is strict: the 16-bit form is only for lengths ≥ 126 and the 64-bit form only for ≥ 65,536, and the top bit of the 64-bit length must be 0 (so no length ≥ 2⁶³). Our encoder always emits the shortest form (tested at 0, 125, 126, 65535, 65536 and 2⁶³−1); the parser rejects anything else with close code `1002`.

Header sizes: 2 bytes (≤125), 4 bytes (≤65535), 10 bytes (larger), plus 4 bytes if masked. Exercise 1 below uses this.

## 2. Masking
`payload[i] ^= key[(offset + i) % 4]`. It is not encryption (the key travels in the frame); it exists so a malicious web page cannot choose the exact bytes that appear on the wire, which could trick caching proxies that mis-parse the stream as HTTP. Our `apply_mask` works 8 bytes at a time using `memcpy` (no alignment or aliasing assumptions; XOR is bytewise so it is endian-neutral), and a differential test compares it to the byte-by-byte definition for every length 0-130, every start offset 0-7 and several buffer alignments.

The bug to know about: a payload arrives in chunks, and each chunk must continue the key where the last stopped. `Masker` remembers the position. When we sabotaged that (restarting the key for every chunk), two parser tests and the randomized round-trip test failed at once.

## 3. The parser: validate early, buffer nothing
`FrameParser::next(input)` is a *pull* API: call it until it says "need more bytes".

```text
Header event  -> the header is complete AND already validated
Payload event* -> slices of the payload, already unmasked, as views into YOUR buffer
End event
```

Design decisions and why:

1. **Validate when the header is complete, before any payload.** A header can declare a payload of 2⁴⁰ bytes. We compare it with `max_frame_payload` (default 1 MiB) at once and answer `1009`. The parser never allocates for payloads, so the declared length costs nothing (a test sends such a header and checks no error and no allocation).
2. **Fail at the earliest byte that proves a violation.** Reserved opcode, RSV bits, a missing mask and "control frame claims more than 125 bytes" are all decidable from the first two bytes, so the failure is reported after two bytes, not after the mask key or payload.
3. **Unmask in place, return views.** The server unmasks directly in the read buffer: zero copies. The cost is a lifetime rule: the views are valid until the caller reuses the buffer (documented on the API).
4. **Chunking never changes the result.** Every split point of a multi-frame stream, byte-at-a-time, and random chunk sizes are tested, plus 500 random encode→parse round trips.

## 4. Close codes
Close frames carry a 2-byte status and an optional reason (≤123 bytes, valid UTF-8). `is_valid_on_wire` accepts 1000-1003, 1007-1014 and 3000-4999. Codes such as 1005 ("no status"), 1006 ("abnormal closure") and 1015 ("TLS failure") are for local reporting only: a peer that *sends* them violates the protocol. We tested the predicate against an independent definition for **all 65,536** values. `encode_close_payload` truncates a long reason at a code-point boundary, because cutting a multi-byte character in half would make our own Close frame invalid.

## 5. Evidence
- Unit tests: RFC vectors, EC-F01…F08, F12…F17, F21/F22, F24, close payload rules.
- Mutation checks: removing the frame limit, the minimal-length rule, or mask continuity each makes tests fail.
- Fuzzing: `fuzz_frame_parser` asserts chunk invariance, header invariants and exact payload accounting; about 540,000 executions in 45 s, clean.

## 6. Pitfalls we hit
1. **A class-level `using Result = ...` shadowed the project's `Result<T>` template** inside `MessageAssembler` and produced baffling errors; renamed to `StepResult`.
2. **Complexity budgets forced better structure.** `parse_first_two_bytes` scored 18 against a limit of 15; splitting off `validate_flags()` made the "which rule failed" logic readable.
3. **Helper placeholders must match.** A test failed on 13 cases because the expected value used one "not applicable" placeholder opcode and the code another. When many tests fail identically, look at the test helper first.

## 7. Exercises
1. How many header bytes does a *masked* frame with a 70,000-byte payload have? An unmasked 200-byte frame?
2. Is `81 7E 00 05 ...` a valid frame header? What does the server do?
3. A server's parser receives `81 05 48 65 6c 6c 6f` (unmasked "Hello"). Result?
4. A client's parser receives `88 02 03 E8`. What is it?
5. Why does the parser report a reserved opcode after two bytes and not wait for the whole frame?

<details><summary>Answers</summary>

1. 2 + 8 + 4 = 14 bytes; 2 + 2 = 4 bytes.
2. No: `7E` announces a 16-bit length but 5 is below 126, so it is non-minimal: close `1002`.
3. Protocol error `1002`: client frames must be masked.
4. A valid unmasked Close frame (FIN, opcode 8) with payload `03 E8` = status 1000 (normal closure).
5. Failing as early as possible lets the server stop reading attacker data and send a Close sooner; waiting would only cost memory and time.
</details>

## 8. What's next
Lesson 08 assembles frames into messages: fragmentation, interleaved control frames, UTF-8 across fragments, and streaming delivery.

# Lesson 03 — SHA-1 and Base64, from scratch

## Goal
Understand exactly how the server proves it speaks WebSocket (`Sec-WebSocket-Accept`), implement both algorithms ourselves, and learn how to *prove* such code correct: published vectors, boundary cases, differential testing, and fuzzing.

## 1. What the handshake needs
```
Sec-WebSocket-Accept = Base64( SHA-1( Sec-WebSocket-Key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11" ) )
```
For the RFC's sample key `dGhlIHNhbXBsZSBub25jZQ==` the answer is `s3pPLMBiTxaQ9kYGzzhZRbK+xOo=`. SHA-1 is **not** used for security here (it has known collision attacks that don't matter for this purpose); the RFC simply requires it. We implement it ourselves to keep the core dependency-free (ADR-0005), which means *we* must prove it correct.

## 2. SHA-1 in one page (`src/crypto/sha1.cpp`)
SHA-1 turns any message into 20 bytes by processing it in **64-byte blocks**, keeping five 32-bit state words.

1. **Padding.** Append a `0x80` byte, then zeros, so the message length becomes ≡ 56 (mod 64), then the message length *in bits* as a 64-bit big-endian number. The result is a whole number of blocks.
2. **Message schedule.** Each block's 16 words are expanded to 80: `w[t] = rotl1(w[t-3] ^ w[t-8] ^ w[t-14] ^ w[t-16])`.
3. **80 rounds.** Each round mixes the five state words with a function `f` and a constant `k` that change every 20 rounds, and a 5-bit and 30-bit rotation.
4. Add the result back into the state; after the last block, the five words (big-endian) are the digest.

**Streaming.** TCP delivers bytes in arbitrary chunks, so `update()` buffers a partial block and processes whole blocks as soon as they exist; `finish()` does the padding. Any chunking must give the same digest. A subtle part is the **padding boundary**: a 55-byte message fits in one block (55 + `0x80` + 8 length bytes = 64), but a 56-byte message needs a second block, because the length no longer fits. The 56-byte test vector below exercises exactly that two-block path (a 56-byte message pads to **2 blocks**).

## 3. Proving it right
`tests/crypto/sha1_test.cpp` checks, in layers:
- RFC 3174 vectors: `""`, `"abc"`, the 56-character two-block message, and **one million `a`** (hashed both in one call and in 1000-byte chunks).
- The padding boundary lengths 55, 56, 63, 64, 65, 119, 120, with expected digests produced by an *independent* implementation (Python `hashlib`).
- The actual WebSocket input from RFC 6455 (EC-H09).
- **Streaming properties:** every possible split point of a message, one byte at a time, and random chunk sizes all give the same digest; `finish()` resets so the object is reusable.
- **Differential testing:** 2000 random inputs compared against OpenSSL (test-only, optional dependency).

The lesson: a handful of vectors proves little; *properties* (any chunking, any length) plus an independent oracle prove a lot.

## 4. Base64 (`src/crypto/base64.cpp`)
Base64 turns 3 bytes (24 bits) into 4 characters of 6 bits each from the alphabet `A–Z a–z 0–9 + /`. If the input isn't a multiple of 3 bytes, `=` padding fills the group: `"f"` → `Zg==`, `"fo"` → `Zm8=`, `"foo"` → `Zm9v`. We checked another by hand: `"Ma"` → `TWE=`.

### Why our decoder is strict
Many decoders are lenient: they skip newlines, accept missing padding, and ignore leftover bits. Leniency is dangerous when two programs parse the same text differently (a *parser differential*): one accepts what the other rejects, and an attacker slips something through. Our decoder accepts exactly one form:
- length a multiple of 4; only the 64 alphabet characters (no whitespace, NUL, `-`/`_`, non-ASCII);
- `=` only as the final one or two characters;
- **canonical**: the unused trailing bits must be zero. In `Zg==`, the character `g` carries 6 bits but only the top 2 belong to data; `Zh==` sets one of the unused bits, so it decodes to the same byte `f` as `Zg==`, but it is rejected. That makes the mapping between byte strings and valid encodings one-to-one.

Tests cover the RFC 4648 vectors both ways, 17 malformed inputs, every one of the 256 possible byte values in the last position, and a round trip of every length from 0 to 300.

## 5. Fuzzing turns "strict" into a checkable property
The fuzz target `tests/fuzz/fuzz_base64.cpp` asserts: **whatever `base64_decode` accepts, `base64_encode` reproduces exactly.** A lenient decoder would break that. We *proved the fuzzer can catch it*: we deliberately removed the canonical-bits check, and within seconds libFuzzer reported the minimal counterexample:

```text
WSRV_ASSERT failed: wsrv::base64_encode(*decoded) == text (tests/fuzz/fuzz_base64.cpp:21)
crashing input: T T = =
```

(`TT==`: `T` = 19 = `010011`, whose low 4 bits are not zero.) We restored the check and kept `TT==` in the seed corpus as a permanent regression test. Unmodified, the targets ran roughly 1.5 to 5 million executions each in 20 seconds with no findings. Run them yourself: `cmake --preset fuzz && cmake --build --preset fuzz`, then `build/fuzz/tests/fuzz/fuzz_base64 tests/fuzz/corpus/base64 -max_total_time=30`.

## 6. Pitfalls we hit
1. **Complexity budget.** The first `base64_decode` was one 40-line function; clang-tidy measured cognitive complexity 29 (limit 15). Splitting it into `parse_quartet` (validate the characters and padding of a group) and `append_bytes` (emit bytes and check canonical bits) made it easier to read, not just to lint.
2. **A sabotage test is only valid if the sabotage applies.** Our first attempt silently failed to modify the file (clang-format had re-wrapped the line), so the "fuzzer found nothing" result was meaningless. We checked the diff before trusting it.
3. **Toolchain:** libFuzzer's runtime needs `libstdc++` while our Clang builds use `libc++`, and Clang silently rewrites `-lstdc++` into `-lc++`. The fuzz targets link `-l:libstdc++.so.6` explicitly.

## 7. Exercises
1. Encode `"Ma"` to Base64 by hand. (Hint: `M`=0x4D, `a`=0x61.)
2. Is `Zm9=` valid? Why or why not?
3. How many 64-byte blocks does SHA-1 process for messages of 55, 56 and 64 bytes?
4. Why is it enough for the fuzz target to check `encode(decode(x)) == x` to detect a lenient decoder?

<details><summary>Answers</summary>

1. `01001101 01100001` → sextets `010011 010110 0001(00)` = 19, 22, 4 → `T W E` + `=` → `TWE=`.
2. No. `9` is sextet 61 = `111101`; with `=` only the top 4 bits belong to data, so the low 2 bits (`01`) must be zero. They are not, so it is non-canonical.
3. 1, 2 and 2 blocks (64 bytes fill a block completely, so the `0x80` marker and length need another).
4. A lenient decoder accepts non-canonical text such as `Zh==`; re-encoding gives the canonical `Zg==`, which differs from the input. Strictness is exactly the claim that this can never happen.
</details>

## 8. What's next
Lesson 04: the UTF-8 validator, a tiny state machine that must work no matter how a message is chopped up.

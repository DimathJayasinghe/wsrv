# Lesson 04 — UTF-8 as a state machine

## Goal
Understand why WebSocket text frames need strict UTF-8 validation, how a four-line table becomes a tiny state machine that works across arbitrary chunk boundaries, and how we gained confidence in it with an independent reference, exhaustive tests, and fuzzing.

## 1. Why the server validates text
RFC 6455 §8.1: a text message MUST be valid UTF-8; if it is not, the endpoint fails the connection with code **1007**. Validation must happen **as bytes arrive**, not after the last fragment: otherwise an attacker could make us buffer megabytes of garbage before we reject it (EC-U06). And the validator must cope with a character being **split** between two fragments or two TCP reads.

## 2. How UTF-8 encodes characters
| Code points | Bytes | Byte 1 | Byte 2 | Byte 3 | Byte 4 |
|---|---|---|---|---|---|
| U+0000–007F | 1 | 00–7F | | | |
| U+0080–07FF | 2 | C2–DF | 80–BF | | |
| U+0800–0FFF | 3 | E0 | **A0**–BF | 80–BF | |
| U+1000–CFFF, U+E000–FFFF | 3 | E1–EC, EE–EF | 80–BF | 80–BF | |
| U+D000–D7FF | 3 | ED | 80–**9F** | 80–BF | |
| U+10000–3FFFF | 4 | F0 | **90**–BF | 80–BF | 80–BF |
| U+40000–FFFFF | 4 | F1–F3 | 80–BF | 80–BF | 80–BF |
| U+100000–10FFFF | 4 | F4 | 80–**8F** | 80–BF | 80–BF |

(This is Unicode Table 3-7.) The bold ranges are where security bugs live:
- `E0` + `80–9F` would be an **overlong** encoding (a short character written with too many bytes; classic filter-bypass: `C0 80` for NUL, `C0 AF` for `/`).
- `ED` + `A0–BF` would encode **surrogates** U+D800–DFFF, which are not characters.
- `F4` + `90–BF` would exceed **U+10FFFF**, the Unicode maximum.
- Lead bytes `80–C1` and `F5–FF` can never start a character.

For example, `€` (U+20AC) is `E2 82 AC` and `😀` (U+1F600) is `F0 9F 98 80` (both checked).

## 3. The state machine (`src/protocol/utf8.cpp`)
Only the **second** byte has a special range; every later byte is `80–BF`. So the whole state is:
- `remaining_`: how many continuation bytes are still expected (0–3);
- `lo_`, `hi_`: the allowed range for the *next* byte;
- `seen_`: how many bytes of the current character we've consumed.

```cpp
bool Utf8Validator::step(std::uint8_t byte) noexcept {
    if (remaining_ == 0) { if (byte < 0x80) return true; return start_sequence(byte); }
    if (byte < lo_ || byte > hi_) return false;
    --remaining_; ++seen_; lo_ = 0x80; hi_ = 0xBF;          // later bytes use the default range
    if (remaining_ == 0) seen_ = 0;
    return true;
}
```

`start_sequence` classifies the lead byte and sets the special `lo_`/`hi_` for `E0`, `ED`, `F0`, `F4`. Because the state lives in the object, you can call `feed()` with any chunking: `feed(E2 82)` then `feed(AC)` works, and `pending_bytes()` reports 2 in between, which the streaming message delivery will use to avoid cutting a character in half (FR-F8).

Behaviour to remember:
- `feed()` returns `InvalidUtf8` **at the chunk containing the first invalid byte**, and stays failed until `reset()`; valid input afterwards doesn't "heal" it.
- A truncated character at the end of a chunk is not an error *yet*. When the message ends, the caller checks `at_boundary()`.

## 4. How we know it's right
Instead of trusting our table, `tests/protocol/utf8_test.cpp` contains an **independent reference**: a slow decoder written directly from Table 3-7, row by row, using a different structure than the state machine. We compare the two on:
- every 1-byte and 2-byte string (65,792 cases);
- every 3-byte string that starts with a lead byte `C0–F8` (about 3.7 million);
- 4-byte strings at every range boundary;
- 200,000 random strings, also delivered in two chunks at a random split.

Plus targeted tests for each error class (EC-U01..U06), a message split at **every** byte position, and a check that the invalid message fails in the chunk holding the first bad byte, not at the end.

The fuzz target `fuzz_utf8` asserts that one-shot, two-chunk and byte-at-a-time validation always agree. Five million executions in 20 seconds found nothing.

## 5. Pitfalls we hit
1. **Test speed matters.** The first exhaustive test (all 16.7 million 3-byte strings, allocating a vector each time) took 16 s under the sanitizers. Passing a view instead of a vector made it 8 s; restricting the first byte to values that can open a multi-byte sequence made it 1.9 s with the same coverage of the interesting logic. A slow test suite is a test suite people skip.
2. **Sign-conversion warnings found real sloppiness** in the test's reference decoder when we changed `int` loop counters to `std::array` indexing; they showed up on Clang and GCC alike. The multi-compiler CI matrix pays for itself.

## 6. Exercises
1. Why is `ED A0 80` invalid even though each byte looks plausible?
2. After `feed(F0 9F)`, what does `pending_bytes()` return, and what is `at_boundary()`?
3. Hand-encode U+00E9 (é) in UTF-8. (Hint: it is in the 2-byte row.)
4. Why must `Utf8Validator` report the error in the chunk that contains the first bad byte rather than waiting for the end of the message?

<details><summary>Answers</summary>

1. `ED` is a 3-byte lead whose second byte may only be `80–9F`; `A0` would put the code point in U+D800–DFFF, the surrogates, which are not valid characters in UTF-8.
2. 2 and `false`: two bytes of a 4-byte character have been consumed and two continuation bytes are still expected.
3. U+00E9 = `1110 1001`; 2-byte form `110xxxxx 10xxxxxx` with the 11 bits `00011 101001` → `C3 A9`.
4. Failing fast stops us buffering unbounded invalid data for an attacker and lets us send close code 1007 immediately (RFC 6455 §8.1; FRAGMENTATION.md §1.2 step 5).
</details>

## 7. What's next
M2: parsing the HTTP upgrade request and building the `101` response, using SHA-1 and Base64 from this milestone.

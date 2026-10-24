# Lesson 02 — `Result`, bytes, and checked arithmetic

## Goal
Learn the three habits every line of protocol code in this project follows: **return errors as values**, **never trust a number from the wire**, and **bound every buffer**. You will see the actual types (`src/base/`), why each exists, and the traps we hit while writing them.

## 1. The problem: hostile numbers
A WebSocket frame header contains a length. The peer chooses it. Suppose we compute where a frame ends:

```cpp
std::uint64_t end = offset + length;   // length came from the network
if (end <= buffer_size) { /* read [offset, end) */ }
```

If `length` is `0xFFFFFFFFFFFFFFF0` the addition **wraps around** to a small number, the check passes, and we read far outside the buffer. That is a classic remote-code-execution bug. The cure is arithmetic that *refuses to wrap*.

## 2. Errors as values: `Result<T>`
`src/base/error.hpp` defines:

```cpp
template <class T> using Result = std::expected<T, Error>;   // a value OR an error
struct Error { ErrorCode code; std::string_view context; };
```

`std::expected` (C++23) is a box that holds either the value or an `Error`. Functions that can fail return it, and `[[nodiscard]]` means the compiler warns if you ignore the result:

```cpp
auto sum = wsrv::checked_add<std::uint64_t>(offset, length);
if (!sum) { return wsrv::fail(wsrv::ErrorCode::Overflow, "frame too long"); }   // propagate
use(*sum);
```

Why not exceptions? On the hot path of a server facing hostile input, errors are *normal*, not exceptional; exceptions hide control flow, cost time, and can't cross a C API (ADR-0006).

Two design details worth noticing:
- **`context` is a `string_view` that must be a string literal.** It is never built from peer data, so an error can't allocate, dangle, or carry attacker text into a log (log injection, SECURITY_DESIGN T14).
- **`ErrorCategory`** (`Protocol`, `Limit`, `Io`, `Tls`, `Config`, `Internal`) lets the server decide what to do: a `Protocol` error closes the connection with a close code; `Internal` is our bug.

## 3. Checked arithmetic
`src/base/checked.hpp`:

```cpp
template <std::unsigned_integral T>
constexpr Result<T> checked_add(T a, T b) noexcept {
    if (a > std::numeric_limits<T>::max() - b) return fail(ErrorCode::Overflow, "addition overflow");
    return static_cast<T>(a + b);
}
```

Read the condition as "does `a + b` exceed the maximum?" rearranged so it can't itself overflow (`max - b` is always safe). For multiplication the guard is `b > max / a`, and it runs **before** the multiplication: narrow types such as `uint16_t` are promoted to signed `int` in C++, and `65535 * 65535` overflows a signed `int`, which is undefined behaviour. Checking first means the multiplication only happens when it is safe. The tests include the maximum values of every width.

## 4. Bytes, endianness, and why we never cast pointers
Binary data is `std::byte` (type `ByteView` = `std::span<const std::byte>`); text is `std::string_view`. Network integers are **big-endian** (most significant byte first). A tempting shortcut is to cast a pointer:

```cpp
auto v = *reinterpret_cast<const std::uint16_t*>(p);   // WRONG, twice
```

We measured both failures:
- On our little-endian CPU the bytes `01 02` read as `0x0201`, not the `0x0102` on the wire (`naive=0x0201 correct=0x0102`).
- On an odd address UBSan reports `runtime error: load of misaligned address ... which requires 2 byte alignment`.

So `load_be16/32/64` assemble the value byte by byte (`(b0 << 8) | b1`): correct on any CPU, any alignment, no aliasing tricks. The compiler turns it into a single load anyway.

`as_chars()` is the **one** audited `reinterpret_cast` in the code base (between `std::byte` and `char`, which the language allows to alias anything); it carries a `NOLINT` with that justification.

## 5. Bounded buffers
- **`ByteBuffer`** is a queue with a hard maximum. `append` past the bound returns `BufferFull` and leaves the buffer *unchanged*. It never reserves the maximum up front (a test creates one with a 1 TiB bound and appends one byte): a peer must not be able to make us allocate memory just by connecting.
- **`RingBuffer`** has fixed capacity and accepts only what fits, returning how many bytes it took. That return value becomes *backpressure* later: if the outbound ring is full, stop reading from the sender.
- Both are checked in tests against simple models (a randomized 20,000-step comparison with `std::deque`).

## 6. Bugs vs. peer behaviour: `Result` vs `WSRV_ASSERT`
If a *peer* can cause a condition, return a `Result` (close the connection, move on). If only a *bug in our code* can cause it (calling `load_be32` on 3 bytes), use `WSRV_ASSERT`: it prints the failed condition and aborts, in release builds too. Continuing after a broken invariant is worse than stopping. Death tests prove the assertions fire.

## 7. Injected time, logging, randomness
`IClock`, `ILogger`, `IRandom` are tiny interfaces; the real implementations (`SteadyClock`, `NullLogger`, `SystemRandom`) are swapped for `FakeClock`, `RecordingLogger`, `FakeRandom` in tests. A 30-second timeout test then runs instantly: `clock.advance(30s)`. `SystemRandom` (the only OS call, `getrandom(2)`) lives in `net/` so `crypto/` and `protocol/` stay free of OS headers (ADR-0012), and the include checker enforces that.

## 8. Pitfalls we hit while writing this
1. **Unspecified evaluation order.** Our first clock test was `EXPECT_LE(clock->now(), clock->now())`. C++ does not say which argument is evaluated first, so it failed intermittently. A flaky test is a bug: read the values into variables first.
2. **Tidy caught style traps early** (`fprintf` ignoring its result, C arrays, seeding an RNG with a constant in tests). Each got either a fix or a justified, narrowly scoped `NOLINT`.

## 9. Exercises
1. What do `checked_add<std::uint8_t>(250, 6)` and `checked_mul<std::uint16_t>(256, 256)` return?
2. A `ByteBuffer` with `max_size` 4 holds `"abc"`. You append `"de"`. What are the result, the contents, and the size afterwards?
3. Why does `RingBuffer::contiguous_front()` sometimes return fewer bytes than `size()`?
4. Which should be a `Result` and which an assertion: (a) a frame header claims 2⁶⁴ bytes; (b) `consume(n)` called with `n` larger than the buffer holds?

<details><summary>Answers</summary>

1. Both return `Error{Overflow}` (250+6 = 256 > 255; 256×256 = 65536 > 65535).
2. `Error{BufferFull}`; contents still `"abc"`; size 3. A failed append never changes the buffer.
3. When the stored data wraps past the end of the underlying array it is two separate runs in memory; `contiguous_front()` returns the first run so a single `write()` call can send it; consume it and ask again for the second.
4. (a) is a `Result`: the peer controls it, so we close the connection with 1009/1002. (b) is an assertion: only a bug in our code can do that.
</details>

## 10. What's next
Lesson 03 uses these tools to build two algorithms from scratch: SHA-1 and Base64, the heart of the WebSocket handshake.

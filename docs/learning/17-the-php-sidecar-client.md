# Lesson 17 — The PHP sidecar client: a binding that only translates

## Goal
After this lesson you can explain why the PHP package contains no WebSocket code, how a strict incremental decoder is written in a language with no unsigned integers, and the one deadlock a client of a backpressured daemon must design against. Package: `bindings/php/`; protocol: `docs/IPC_PROTOCOL.md`.

## 1. The problem
PHP's usual life is one short request. A WebSocket is one long connection. Instead of fighting that, `wsrvd` (lessons 15 and 16) owns every browser connection and speaks a small framed protocol over a Unix socket. The PHP side is a *binding*: it turns that protocol into PHP calls and nothing else. All the hard rules (RFC 6455 framing, UTF-8 validation both ways, limits, backpressure, "exactly one Close for every Open") stay in the C++ core, tested once. If a WebSocket bug could only be fixed by editing PHP, the design would be wrong.

## 2. Design
Four small layers, each testable alone:

1. **`Codec`**: encodes the frames an application sends. Rejects what no daemon could accept (request id 0, connection id 0, long token or reason).
2. **`FrameDecoder`**: turns a byte stream into messages. Like the C++ decoder it checks the **declared length before waiting for the body**, rejects unknown types, non-zero flags, wrong sizes and inconsistent events, and once it fails it stays failed: a length-prefixed stream cannot be resynchronised.
3. **`Client`**: the socket: handshake, `poll`, commands. One request id per command, numbered from 1, wrapping past 2^32-1 back to 1 (never 0).
4. **`Worker`**: callbacks (`open`, `message`, `close`, ...) and a loop, for people who want that style.

### PHP has no unsigned integers
Lengths and ids are unsigned on the wire (`u32`, `u64`). PHP has signed 64-bit `int`. `pack('N')` / `unpack('N')` handle `u32` fine on 64-bit PHP. A `u64` connection id is read with `J`; if the top bit is set the result is negative, which the decoder treats as a corrupt stream (the daemon counts up from 1, so a real id never gets there). The package therefore needs 64-bit PHP, and says so.

### The deadlock a polite client must avoid
The daemon stops reading from you when its output to you is full (that is backpressure working). Suppose your worker answers a burst of events with a burst of commands, and writes block because the daemon is not reading. If you are blocked in `fwrite` and not reading, the daemon cannot drain its output, so it never reads your commands: **a deadlock until the stall timer fires.** The fix is in `Client::writeAll()`: while a write is waiting, also wait for the socket to become readable, read whatever the daemon sent, decode it, and queue the messages for `poll()` to hand out later.

## 3. Tests: what proves it
- **Golden vectors.** `CodecTest` compares encoder output with bytes written out by hand from the protocol document, so an encoder that is wrong in a self-consistent way still fails.
- **Strictness table** in `FrameDecoderTest`: every malformed header, wrong body size, and impossible event is an error; one byte at a time and a thousand frames in one read both work.
- **The deadlock test** forks a fake daemon that writes 450 KB of events and only then reads our 4 MB command. I disabled the read-while-writing branch and the test failed with "wsrvd did not accept our data within 10 seconds"; restored, it passes. That is how you know it guards the trap.
- **Interop** (`tests/interop.php`): the real `wsrvd`, a `Worker`, and a raw WebSocket client written in the test: text, multibyte UTF-8, all 256 byte values, a 100 KiB message (the daemon fragments it; the test client reassembles), refused second worker, invalid UTF-8 rejected, close with a custom code, browser-initiated close, abrupt disconnect reported as 1006, clean SIGTERM shutdown.

## 4. Pitfalls
- Arrow functions capture variables **by value**. A wait-predicate `fn () => $done` never sees a later change. Share state through an object (the interop test uses one `stdClass`).
- `step()` returns true for any message, including a `STATUS` answer; wait for the thing you want, not "one message".
- A test client must reassemble fragmented messages: RFC 6455 lets the server fragment.
- Do not share the daemon connection across `fork()`.

## 5. Exercises
1. Make `Client` bound the number of messages it queues while writing, and decide what to do at the bound (hint: what does the daemon do to a worker that stops reading?).
2. Add an `at-least-once` mode on paper: what would the daemon have to remember, and for how long?
3. Write the same decoder in a language of your choice from the protocol document only. What was ambiguous?

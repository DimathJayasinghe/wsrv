# Lesson 51 — A log line that cannot be forged and cannot stall the server

## Goal
After this lesson you can say why a server's log is an attack surface, how a log line is built so a client cannot write another one, why a stuck log destination must cost lines and never time, and why the client's address is off by default.

## The problem
Until this lesson the server counted problems (`/metrics`) but never said which, when or why. An operator saw `rejected_ip_limit` rise and had no line to read. The obvious fix, "call `std::cerr << message`", is three bugs waiting to happen.

## 1. A log line is data from an attacker
Take `log("origin refused: " + origin)`. The Origin header is the client's. A client that sends
`Origin: x\nts=... level=info event=app_connected msg="all fine"` has just written a second line into your log, one an alert rule or a person will believe. Add `\x1b[2J` (clear screen) and a terminal viewer is attacked too. This is **log injection** (SECURITY_DESIGN T14).

Two defences, and wsrv uses both:
- **Never log what the peer chose.** The messages are fixed sentences; the Origin that was refused is not in the line at all. The line says *that* the Origin was refused, `conn=17` says which connection.
- **Escape anyway.** `msg` is always inside quotes, and `\`, `"`, every control character, DEL and every byte from 0x80 up becomes `\\`, `\"`, `\n`, `\xNN`. One event is exactly one line whatever reaches the escaper. A later developer who "just adds the path to the message" is then a bug, not a vulnerability.

Look at `tests/base/log_format_test.cpp`: the injection test feeds `"a\nlevel=error event=forged"` and asserts the output has one newline, the last character.

## 2. A line has a shape, and a shape is a contract
```
ts=2026-11-26T12:00:07.884Z level=warn event=origin_refused conn=17 msg="handshake refused: ..."
```
`key=value` pairs, fixed order, `msg` last. `event` is a **stable name**: an alert rule matches `event=slow_consumer`, never the sentence, because sentences get reworded. That is why field and event names are promised stable within 1.x and the text is not. Naming an event is a public API decision; the 37 old call sites each had to get one.

## 3. Bounded, in every direction
CLAUDE.md says every buffer is bounded. A log has three:
- **The line.** At most 512 bytes. The message is cut at 256 escaped characters (never in the middle of a `\xNN`). A constant, checked at compile time: the worst case is 502.
- **The rate.** A hostile client can make the server refuse a million handshakes. One log line each would be a denial of service on the disk, and the one line you need would drown. Each event gets a **token bucket**: a burst of 10, then one a second. What was held back is counted and the next line says `suppressed=412`. The table of buckets holds 64 names; the rest share one, so memory is a constant too.
- **The wait.** Section 4.

## 4. A stuck log must not stall a loop
The I/O loops are threads that must never wait: while one waits on `write()` to a full pipe, every connection on that loop is frozen. Pipes fill up when the thing reading them (a hung log driver, `journald` under load) stops reading.

The two textbook answers are a writer thread with a queue, or a non-blocking write. wsrv takes the second (ADR-0037 explains why not the first: a thread brings start, stop, fork and flush rules to protect against a destination that is wedged, which a guard handles by dropping): `poll(fd, POLLOUT, timeout 0)` asks "can you take this right now?", and only if yes one `write` of the whole line. If not, or the write fails or comes back short, the line is dropped and counted, and the next line that gets out carries `dropped=N`.

A line of at most 512 bytes is below `PIPE_BUF` (4096), so POSIX guarantees a single `write` to a pipe is not interleaved with another thread's. That is why the multi-thread test can assert every line is whole.

The test that matters is `AFullPipeDropsTheLineInsteadOfBlockingTheCaller`. To prove it can fail we removed the `poll` guard and ran it: it went red after 2 s ("log() blocked on a full pipe"). A test you have never seen fail has not been shown to test anything.

(A detail that cost an hour: to fill the pipe the test first used `dup()` plus `O_NONBLOCK`. But `O_NONBLOCK` belongs to the *open file description*, which a `dup` shares, so the logger's own descriptor became non-blocking too and the test could never block. It re-opens the pipe through `/proc/self/fd` to get a separate description.)

## 5. The address is off by default
An IP address is personal data (GDPR treats it so). The default `ip_limit` line therefore says a limit was hit but not by whom. One setting, `log_peer = true`, names the culprit when you decide you need it. The cost of the default is stated in the ADR; the point is that the safe choice is the one you get without reading the manual.

## 6. Where the code lives
`base/` has the pure parts (record, escaper, timestamp, rate limiter: no OS headers, tested without a clock or a pipe). `net/` has `StreamLogger`, the only code that calls `poll` and `write`, through `ISyscalls` so the failures (EINTR, EPIPE, a short write) are injected. The two roots, `wsrvd` and the embedded server, construct it and hand it to the server; nothing else knows it exists.

## Pitfalls
- Escaping is for the sink; it does not excuse logging a peer's text.
- A rate limiter keyed by the *message* is a memory leak with a hostile client's name on it. Key it by a fixed set of event names.
- `write()` on a non-blocking descriptor can still be a short write. Check the count.
- Do not format the line before you know the level is wanted: the `enabled()` check comes first.

## Exercises
1. Why does `msg` come last, and why is it always quoted even when it has no spaces?
2. The limiter allows a burst of 10, then 1 a second. Two events alternate, A then B, forever, A at 100 a second. How many A lines per minute? How many B?
3. A pipe holds 64 KiB. At 200 bytes a line, how many lines can pile up before `dropped=` appears, if nobody reads?
4. Why is one `write` of 512 bytes safe from interleaving, and a `write` of 5000 bytes not?
5. Add an event `publish_slow` for a publish request that took over a second. Which file names it, which test proves its text carries nothing the client sent, and which docs table gets a row?

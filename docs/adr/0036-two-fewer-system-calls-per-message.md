# ADR-0036: Two fewer system calls per message: no read that only blocks, no getpid per call

- Status: **Accepted** (2026-11-24; second slice of the one-CPU echo work, after ADR-0035)
- Date: 2026-11-24
- Deciders: project owner, maintainer assistant
- Extends: ADR-0013 (the connection model and its read loop), ADR-0019 (the transport interface TLS plugs into), ADR-0016 (the embedded API's fork rule); their decisions stand.

## Context
After ADR-0035 the daemon still made about **five system calls per echoed message**, against about three for a single-process PHP server (strace counts, lesson 49): two `read`, two `getpid`, one `sendto`. Two of them carry no information:

1. **The second `read`.** The loop reads a ready socket until the kernel answers `EAGAIN` (edge-triggered epoll needs the socket drained). For a plain TCP socket the read that precedes it already proved it was empty: a stream socket hands out everything it has, up to the buffer, so a read that returns fewer bytes than the buffer holds means nothing more is waiting, and any later arrival raises a new edge. epoll(7) says so for stream sockets.
2. **`getpid()` on every `poll` and `send`.** The embedded API refuses to run in a child created by `fork()` (the I/O thread does not exist there), and it checked by comparing `getpid()` with the pid recorded at creation. Since glibc 2.25 `getpid()` is no longer cached: it is a real system call, twice per message in the daemon.

## Decision
1. **`ITransport::short_read_means_drained()`** (default `false`; `PlainTransport` returns `true`). `Connection::on_readable` returns after a read that came back shorter than the buffer when the transport says so. It does **not** skip when the event carried `Hangup` or `Error` (the FIN may have arrived with the data, and only a read that returns 0 reports it), and it never skips for TLS, which holds decrypted data the kernel will not announce again. The read budget and the re-arm logic are untouched: a full read still loops.
2. **`ForkGuard`** (`src/capi/fork_guard.{hpp,cpp}`) replaces the `getpid()` comparison. It owns one private page marked `MADV_WIPEONFORK` (Linux 4.14), holding a 1 in the creating process; the kernel zeroes it in every child at `fork()`, so `forked()` is one relaxed load of a byte. Where the kernel refuses the flag (older than 4.14) or `mmap` fails, `forked()` compares `getpid()` as before; a constructor flag makes that path testable. No global, no `pthread_atfork`.

## Rejected
- **Cache the pid in a `thread_local` or a plain member.** A child created by `fork()` inherits the cached value, which is the very thing the check must detect.
- **`pthread_atfork` to flip a flag.** Process-wide registration (a global in effect), cannot be undone when the server is destroyed, and a library should not install fork handlers into its host.
- **Skip the second read for every transport.** TLS would lose data; the hint is per transport.
- **Skip it even with `Hangup` set.** A connection that sent its last message and closed would never be noticed (no further edge).

## Consequences
- + One system call fewer per ready socket on the plain path, and none for the fork check.
- + The fork check is now a memory read, also for PHP `step()` loops that poll in a tight cycle.
- - A transport that wrongly claims `true` would lose the tail of a burst; the interface comment states the condition and the only implementation is a stream socket.
- - One page (4 KiB) per embedded server, outside the heap.

## Verification
- `ConnectionTest`: a stream-like transport answers one event with one read; the default transport (like TLS) still reads until `WouldBlock`; a full read is followed by another (42 bytes through a 16-byte buffer: 3 reads); a closing peer is read to the end after a short read; a closing peer with nothing left is seen. Mutation checked: never skipping fails the first and third; ignoring `Hangup` fails the closing-peer test.
- `ForkGuard`: not forked in the creating process; forked in a child and in a grandchild; the `getpid()` fallback answers the same. Mutation checked: a guard that never reports a fork fails the child tests.
- The existing embedded-server test that forks a child and expects `WrongProcess` still passes.

## Result
(measured 2026-11-24, one CPU, one loop thread, 512 connections, 64-byte echo, release build, alternating runs)

| | ADR-0035 build | this change |
|---|---|---|
| messages per second, rounds 2 and 3 | 95 158, 94 877 | **102 847, 103 126** (+8.1%, +8.7%) |
| p50 latency | 5.2 ms | 4.8 to 4.9 ms |

Round 1 was disturbed for both builds (53 206 and 56 840, the same direction). The gain is about 8 percent, not the 20 percent the lower system-call count might suggest: the remaining time is in the kernel's TCP path and the PHP worker's own work.

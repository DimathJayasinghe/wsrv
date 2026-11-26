# Lesson 50 — Two system calls that carry no information

## Goal
After this lesson you can say when a read loop may stop before `EAGAIN`, why `getpid()` became a real system call, and how `MADV_WIPEONFORK` answers "was I forked?" with a memory read.

## The problem
Lesson 49 ended with a count: about five system calls per echoed message, against three for a single-process server. Two of the five told the program nothing it did not know.

## 1. The read that only says "would block"
With **edge-triggered** epoll the kernel reports a socket when it *becomes* readable, once. The loop must therefore read until `EAGAIN`, or data left behind is never announced. The cost: after every message, one extra `read` that returns "nothing".

For **stream** sockets the extra read can be proven redundant. `read(fd, buf, 64 KiB)` returns what the socket has, up to 64 KiB. If it returned 80 bytes, the socket had 80 bytes, and it is empty now. Whatever arrives next raises a new edge. (epoll(7) says this for stream sockets, and says the opposite for datagram sockets and terminals.)

Three things make it safe, and each has a test:
- the **transport** vouches for it (`short_read_means_drained()`): TLS cannot, because it may hold decrypted data the kernel will not announce again;
- a **full** read never stops the loop: the buffer was too small, there may be more;
- a **hang-up** event never stops it: a FIN that arrived together with the data is only seen by the read that returns 0, and no further edge will come.

## 2. The pid that never changes, except when it does
The embedded server refuses to run in a child made by `fork()`: the I/O thread exists only in the parent. It checked with `getpid() != owner_pid_`. Until glibc 2.25 `getpid()` was cached in user space; since then every call enters the kernel. Two per message.

Caching the pid ourselves does not work: a forked child *inherits* the cache, which is exactly what must be detected. `pthread_atfork` works but installs a process-wide handler into the host (and cannot be removed when our object dies).

`MADV_WIPEONFORK` solves it with the kernel's help: mark a private page; the kernel zeroes it in every child at `fork()`. Put a 1 in it at construction. `forked()` is `page[0] != 1`, one load. On kernels without the flag, fall back to `getpid()`.

## 3. What the measurement said
About +8 percent (95k to 103k messages per second, one CPU). Two system calls out of five is 40 percent fewer calls, but the CPU also goes to the kernel's TCP path and the PHP worker, so fewer calls bought less than the count suggests. Always measure; do not assume a count converts to a speed.

## Pitfalls
- "A short read means drained" is a property of stream sockets, not of reads in general.
- A cache of something that a `fork()` duplicates is not a fork detector.
- Mutation-check the guard conditions: delete `!peer_closing` and the test for the closing peer must fail.

## Exercises
1. Which of the three safety conditions would a UDP transport violate?
2. A client sends 10 bytes and closes in the same packet burst. Walk through what the loop does, with and without the hang-up condition.
3. Why does `ForkGuard` use a relaxed load, and what would a child see if the parent were midway through a store?
4. Which kernel versions need the `getpid()` fallback, and how would you test that path? (See the constructor flag.)

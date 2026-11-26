# Lesson 52 — A second door into the daemon: the same protocol over TCP

## Goal
After this lesson you can say what a Unix socket gives a server for free that TCP does not, how the daemon replaces each of those things, why one idle connection can lock a one-application server out (and the rule that prevents it), and what "tested on Windows" has to mean.

## The problem
`wsrvd` talks to your PHP worker over a Unix socket. On a Mac or a Windows PC the daemon runs in Docker's Linux virtual machine and the worker on the host, and a Unix socket cannot cross that boundary. Run the worker in a container too and you lose your editor and debugger; so the same protocol must also run over TCP. The framing, `HELLO`, backpressure and failure rules do not change (the protocol stays 0.3). What changes is everything the *transport* used to guarantee.

## 1. What a Unix socket gave for free
- **Who is calling.** `SO_PEERCRED` makes the kernel tell the daemon the uid of the other end. A peer with another uid is dropped before a byte is read. TCP has no such thing: anyone who can reach the port is "calling".
- **Who can reach it.** A socket file is a path with mode `0600` in a private directory. A TCP port is reachable by every host the network lets through.
- **When the peer is gone.** When a process dies, the kernel closes its Unix sockets at once. A TCP peer can vanish (laptop lid, pulled cable) without a FIN, and the kernel waits for hours.
- **No eavesdropping.** The bytes never leave the machine. On TCP they cross a network unencrypted.

## 2. The design, one answer per loss
| Lost | Answer |
|---|---|
| identity | the token becomes **mandatory** and **at least 32 bytes** (it can now be guessed from the network); compared in constant time; one `INVALID_ARGUMENT` for every failure so the answer teaches nothing |
| reachability | loopback by default; a non-loopback bind logs `ipc_plaintext`; docs say where it is acceptable (loopback, a published loopback port, a VPN, an SSH tunnel) |
| a dead peer | `SO_KEEPALIVE` with 30 s idle, 10 s interval, 3 probes: the slot frees in about a minute |
| privacy | not solved: **said out loud**. TLS on this link is a later decision, and the ADR records why it is not in the first step |

Plus `TCP_NODELAY` on both ends: the link carries small request and answer frames, and Nagle's algorithm with delayed ACK would add tens of milliseconds to each.

## 3. The single slot, and the connection that says nothing
The daemon serves **one** application at a time. On a Unix socket the slot was taken at `accept`, which was fine: the other end was the same user. On TCP, anyone can connect and stay silent for the whole `HELLO` timeout, and the real application then gets `BACKPRESSURE` in a loop. The rule (ADR-0038 section 5): **a connection that has not said `HELLO` holds nothing, so a newcomer replaces it**. An authenticated application is never displaced.

The subtle part is the order. If a real `HELLO` is already sitting in the kernel's buffer when a second connection arrives, "replace the one that has not said HELLO" would throw away a real application. So `admit()` first *looks* at the current connection (reads what has arrived, processes it), and only then, if it is still waiting for `HELLO`, replaces it. Two tests pin the two halves, and each was **mutation-checked**: remove the replacement and the "idle connection" test fails; move the replacement before the look and the "HELLO already arrived" test fails. A test that has never been seen to fail proves nothing.

An automated review pointed out that a flood of connections can still displace the real application's pending connection. True, and written down in the ADR with the alternative (a bounded set of pending connections raises the cost from one connect to N, but does not remove the case, and makes admission multi-peer). The port is a trust boundary like a database port.

## 4. The close that can erase the answer
The first Windows CI run failed one test, and the failure was real. The daemon says "busy" and closes. If the worker's `HELLO` was still unread (or arrives after the close), the operating system answers with **RST** instead of a clean FIN. Linux lets the receiver still read what arrived before the reset; **Windows throws it away**. So a Windows worker would see "connection closed" instead of `BACKPRESSURE`, and could not tell a busy daemon from a dead one. First reaction, "the supervisor retries both, so it costs nothing", was the wrong one: the owner's rule is that what can break will break, and a fixable thing is fixed.

The fix is a *lingering close*: send the status, send FIN (`shutdown(SHUT_WR)`), keep the socket, read and discard until the peer closes or a short deadline passes, then close. Details that mattered: it must never hold the application slot (it is a separate object); it must be bounded (32 at a time, then close at once and log `ipc_linger_full`); it needs a hard deadline whatever the peer does; and it must read what is *already* in the socket when it starts, because epoll is edge-triggered and will not say so again. Linux cannot show the bug directly (the data survives a reset), so the test watches what a *write after the FIN* does: the first write to a fully closed peer provokes the reset, the second then fails with EPIPE; against a lingering daemon both succeed. That probe was **mutation-checked**: turn lingering off and it fails.

## 5. Two details worth the detour
- **The image and the "both is an error" rule.** The official image set `WSRVD_IPC_SOCKET` as an `ENV`, so `-e WSRVD_IPC_PORT=9300` would arrive with both keys set and fail the very rule that protects people. The fix is not to loosen the rule: the image's socket path became a *build-time default* that applies only when neither key is set.
- **A token file from a Windows drive.** Docker Desktop shows a bind-mounted file as root-owned and world-readable, which the daemon correctly refuses. The example copies the token into a named volume with the right owner and mode instead of weakening the check.

## 6. What "it works on Windows" means
PHP's `pcntl_fork` does not exist on Windows, and `stream_socket_pair` there accepts only the INET family. So the client tests were rewritten to use real loopback TCP and a **child PHP process** as the fake daemon (`proc_open` exists everywhere), and a `windows-latest` job runs them. What it does not prove, and the docs say so: Docker for Linux containers on a Windows runner, and the daemon itself. Each claim is tied to the test that backs it, on the platform where it can run.

## Pitfalls
- A new transport is a new *threat model*, not just a new socket type: list what the old one guaranteed before you write the new one.
- Renaming a PHP parameter breaks callers that use named arguments. `connect($socketPath, ...)` kept its name although it now also takes `tcp://...`.
- "The caller retries anyway" is not a reason to leave a platform-specific failure in place: it turns an exact answer into a vague one.
- A test that asserts "the message contains `tcp://`" passes for the *wrong* error too (the "cannot connect to tcp://..." message). Assert the message that proves the right code path ran.
- Checking a claim in your own design document: the first draft of the ADR listed a JavaScript IPC client. There is none (`clients/js` is the browser WebSocket client). Read the code before you write the plan.

## Exercises
1. A worker connects, sends a valid `HELLO` one byte at a time over ten seconds, and a second connection arrives at second five. What does the daemon do, and which test covers the nearest case?
2. Why must `look_now()` run before the replacement and not after?
3. With the keepalive numbers above, how long until a vanished host frees the slot? What breaks if the idle time is two hours?
4. Add TLS to this link on paper: who holds the certificate, who verifies it, and which of the three clients (C++, PHP, a future Node one) is hardest?
5. Why is a 32-byte token acceptable over loopback TCP but a 6-character password is not, even though both are "secrets"?

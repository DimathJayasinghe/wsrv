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
| identity | the token becomes **mandatory** and must be one the daemon's own `--generate-token` made (it can now be guessed from the network, and a length floor cannot tell a random token from a long guessable one: see section 7); compared in constant time; one `INVALID_ARGUMENT` for every failure so the answer teaches nothing |
| reachability | loopback by default; a non-loopback bind logs `ipc_plaintext`; docs say where it is acceptable (loopback, a published loopback port, a VPN, an SSH tunnel) |
| a dead peer | `SO_KEEPALIVE` with 30 s idle, 10 s interval, 3 probes: the slot frees in about a minute |
| privacy | not solved: **said out loud**. TLS on this link is a later decision, and the ADR records why it is not in the first step |

Plus `TCP_NODELAY` on both ends: the link carries small request and answer frames, and Nagle's algorithm with delayed ACK would add tens of milliseconds to each.

## 3. The single slot, and the connection that says nothing
The daemon serves **one** application at a time. On a Unix socket the slot was taken at `accept`, which was fine: the other end was the same user. On TCP, anyone can connect and stay silent for the whole `HELLO` timeout, and the real application then gets `BACKPRESSURE` in a loop.

The first answer was "a connection that has not said `HELLO` holds nothing, so a newcomer replaces it". It closed the silent-connection case and left a hole that an audit found after the feature had merged: connects that arrive faster than the real worker's *connect-to-`HELLO` window* keep replacing the worker's connection before its `HELLO` is read. The window is short on loopback and longer across Docker Desktop's VM, but it is never zero, so a flood always had a chance.

The answer now (ADR-0038 section 5, amended) is a **waiting room**: up to 8 connections wait at once, each with its own `HELLO` deadline. The first to send an accepted `HELLO` becomes the application and the rest are told `BACKPRESSURE`; when a ninth arrives the *oldest* is evicted and told why. An authenticated application is never displaced. To keep the real worker out an attacker needs more than 8 new connections inside its window, instead of one.

Three things to notice:
- **It is a mitigation, and the ADR says so.** A big enough flood still wins, like against any listener. What removes it is who can reach the port (loopback bind, a firewall, a private Docker network), not code. Do not claim more than the design delivers.
- **The order still matters.** A `HELLO` that already sits in the kernel's buffer must be processed before anything is evicted, so `admit_waiting()` first *looks* at the oldest connection (`look_now()`), and only then evicts.
- **Test the property, not the constant.** The tests are written in terms of `kMaxPendingHello`, so a mutation that changes the constant would shrink them too. A `static_assert` pins the documented value (8), and the mutation that matters, "a newcomer evicts whoever is waiting" (the old design), is run by hand and fails four tests. A test that has never been seen to fail proves nothing.

## 4. The close that can erase the answer
The first Windows CI run failed one test, and the failure was real. The daemon says "busy" and closes. If the worker's `HELLO` was still unread (or arrives after the close), the operating system answers with **RST** instead of a clean FIN. Linux lets the receiver still read what arrived before the reset; **Windows throws it away**. So a Windows worker would see "connection closed" instead of `BACKPRESSURE`, and could not tell a busy daemon from a dead one. First reaction, "the supervisor retries both, so it costs nothing", was the wrong one: the owner's rule is that what can break will break, and a fixable thing is fixed.

The fix is a *lingering close*: send the status, send FIN (`shutdown(SHUT_WR)`), keep the socket, read and discard until the peer closes or a short deadline passes, then close. Details that mattered: it must never hold the application slot (it is a separate object); it must be bounded (32 at a time, then close at once and log `ipc_linger_full`); it needs a hard deadline whatever the peer does; and it must read what is *already* in the socket when it starts, because epoll is edge-triggered and will not say so again. Linux cannot show the bug directly (the data survives a reset), so the test watches what a *write after the FIN* does: the first write to a fully closed peer provokes the reset, the second then fails with EPIPE; against a lingering daemon both succeed. That probe was **mutation-checked**: turn lingering off and it fails.

## 5. Two details worth the detour
- **The image and the "both is an error" rule.** The official image set `WSRVD_IPC_SOCKET` as an `ENV`, so `-e WSRVD_IPC_PORT=9300` would arrive with both keys set and fail the very rule that protects people. The fix is not to loosen the rule: the image's socket path became a *build-time default* that applies only when neither key is set.
- **A token file from a Windows drive.** Docker Desktop shows a bind-mounted file as root-owned and world-readable, which the daemon correctly refuses. The example copies the token into a named volume with the right owner and mode instead of weakening the check.

## 6. What "it works on Windows" means
PHP's `pcntl_fork` does not exist on Windows, and `stream_socket_pair` there accepts only the INET family. So the client tests were rewritten to use real loopback TCP and a **child PHP process** as the fake daemon (`proc_open` exists everywhere), and a `windows-latest` job runs them. What it does not prove, and the docs say so: Docker for Linux containers on a Windows runner, and the daemon itself. Each claim is tied to the test that backs it, on the platform where it can run.

## 7. A length floor is not a strength check
The first version required a token of at least 32 bytes. `MyCompanyWebsocketToken2026!!!!!` is 32 bytes and guessable, and over TCP a guesser needs only a network path and time. Throttling guesses does not help: a global back-off lets anyone who can reach the port lock the *real* worker out with one bad attempt a second, and a per-source back-off needs the peer address (which the daemon deliberately never keeps) and fails where every client shares one address (Docker's bridge). The control that works at any attempt rate is a token that cannot be guessed, so the daemon accepts over TCP **only** the form `wsrvd --generate-token` writes: `wsrv1_`, 64 hex digits of 32 random bytes, `_`, a CRC-32. The checksum is not a secret and not a defence against someone who forges a token on purpose; it catches the operator who retypes or edits one, and the start error prints the command to run. Requiring this costs nothing now, because nothing has shipped over TCP; after a release it would break every deployment. The publish endpoint of 1.0 had the same exposure and *had* shipped, so applying the rule to it (ADR-0039) was a breaking change in a minor release. The owner chose that over a warning ("a warning is read by the careful and ignored by the people at risk"), and the CHANGELOG says so first.

## Pitfalls
- A length requirement checks the one thing that is easy to measure. Make the secret *come from a generator* when its strength is the point.
- A defence that locks out the attacker can lock out the owner: check what an attacker gains by triggering it on purpose.
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
5. Why is a generated 256-bit token acceptable over loopback TCP but a 6-character password is not, even though both are "secrets"? And why is a 32-character passphrase *still* not?
6. The waiting room holds 8. Argue for 4 and for 32. What does each number change for the attacker, for the daemon's memory, and for an operator reading `app_replaced`?

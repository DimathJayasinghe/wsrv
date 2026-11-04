# Lesson 20 — Asking the application through a socket (IPC v0.2)

## Goal
After this lesson you can explain how the authentication question travels from the WebSocket server to an application in another process, why an IPC protocol needs a version rule when a feature is added, what "fail closed while nobody is listening" means in code, and why a request that is too big must not be able to cut the application's link.

## 1. The problem
Lessons 18 and 19 gave the C host a way to answer "may this user connect?". `wsrvd` is the second way a language uses the server: a daemon with a Unix socket, and an application (PHP) that connects to it (lesson 16/17). The daemon sits between: it is the *host* of the embedded server, and it must pass the question on to the real application and bring the answer back. Three new problems appear that the C host never had:
1. **A wire format.** The question has to cross a socket, so it needs frames (lesson 16).
2. **Old and new programs mix.** An application written before the hook existed cannot read the new event.
3. **The application may not be there.** A C host is the process; an IPC application can be absent, late or dead.

## 2. The wire format
New in protocol 0.2 (`docs/IPC_PROTOCOL.md` section 9):
- Event kind **6 Handshake**: `data` is the same length-prefixed blob as in lesson 19 (`capi/handshake_blob`). We did not invent a second encoding, so the C library and the daemon cannot disagree about a field.
- Frame **10 HANDSHAKE_ACCEPT** (`request, conn, subprotocol`) and **11 HANDSHAKE_REJECT** (`request, conn, status`). Each is answered by the existing `STATUS` frame with the same `request` number, like every command.

The codec rules did not change: a frame is canonical (one message, one byte sequence), sizes are checked before bodies, an unknown field makes the frame invalid. The tests have *golden bytes* for the new frames (exact expected bytes), so a change of layout is a failing test, not a silent protocol change.

## 3. Versions: additive, but not blind
Minor version 2 is "additive": a 0.1 application keeps working with a 0.2 daemon *unless the daemon asks questions*. An application that does not know kind 6 would never answer, and every browser would wait for a 503. So a daemon with the hook on refuses a `HELLO` with minor below 2. The refusal is the same message as every other refusal (wrong token, wrong major): if the answers differed, a stranger could probe which check failed.

## 4. Fail closed while nobody listens
A daemon with no application used to *hold* browsers: events queue up, the next application receives them. That is right for messages, wrong for questions: a question is only worth asking while someone can answer within three seconds.

The fix is a door, the **intake**: `IBackend::set_handshake_intake(bool)`.
- Closed at start-up; opened when an application completes `HELLO`; closed when it leaves.
- While closed, the bridge's `request()` fails *immediately*, the server answers `503` and nothing is queued.
- Closing it also refuses (503) the questions already waiting, because they went to a host that is gone.

The detail that matters: the "closed" flag and the table of waiting questions change under **one mutex**. With two separate steps, a question could be added between "close the door" and "reject everyone waiting", and would then wait for the 30 s timeout. A test (`WhenTheApplicationDiesWhileAskedTheWaitingBrowserGets503AtOnce`) uses a very long server timeout so that a quick 503 can only be the intake closing.

## 5. A question the client can make too big
The question carries what the *browser* sent: a long path, forwarded cookies. The IPC link has a frame limit (`ipc_max_frame`, at least 4 KiB). If the encoder refused a question, the old code path was `close()`: end the link, because a failed encode of a normal event "cannot happen". For a question it can happen, and it would let any browser disconnect the application by sending a big cookie. So that one case answers the browser `503` and carries on. A test sends a question as big as the frame limit and then checks that the next event still arrives.

## 6. Why the daemon keeps no list of open questions
A natural design: the relay remembers which question ids it forwarded and rejects answers for others. But the server can end a question on its own (timeout, the browser leaves) and the relay is not told, so such a list only grows. The daemon instead forwards every answer to the backend, which already knows exactly which questions are open (lesson 19) and answers `NO_SUCH_CONN` for the rest. One source of truth, no growing set.

## 7. Pitfalls we hit
- **Mutation testing earned its keep again.** Four mutants (no intake close on disconnect, no minor check, `close()` instead of 503 for an oversized question, intake never opened) were each killed by at least one test. A mutant that survives means a test was only checking the happy path.
- **Test timing.** `sleep`-based assertions flake; the tests use `wait_until(condition)` with a generous deadline and a *long* server-side timeout, so a result that arrives early cannot be the timeout.
- **A fuzz target for a codec, not for a connection.** The IPC fuzz target already re-encodes whatever it decodes, so the new frames were covered by adding a seed file. The blob got its own target, because it is decoded by a different layer.

## 8. Exercises
1. Change `kHandshakeHookMinor` to 3 and list every test that fails. Which ones encode the rule, and which only encode the number?
2. An application sends `HANDSHAKE_ACCEPT` for an id it was never asked about. Trace the frame to the `STATUS` it gets back. Which layer says `NO_SUCH_CONN`?
3. Why is `set_handshake_intake` `noexcept` in `IBackend` while `accept_handshake` returns a `Result`?
4. (Design) Two daemons share one application through a load balancer. What breaks in "the id of the question is the id of the connection"?

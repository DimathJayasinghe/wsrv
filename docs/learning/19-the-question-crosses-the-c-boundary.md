# Lesson 19 — The question crosses the C boundary

## Goal
After this lesson you can explain how the server's "may this user connect?" question (lesson 18) reaches a program written in another language, why the answer can be given only once, why a length-prefixed blob is used instead of a struct, and how a C library hands out strings without anyone being able to read freed memory. The decision record is ADR-0018, Amendment 2.

## 1. The problem
Lesson 18 built the server side: a connection waits in `AwaitingDecision` while "the application" decides. In slice 1 the application was a test fake. A real host (PHP, Python, Go...) talks to the library through the **C ABI** (`include/wsrv/wsrv.h`), which has rules the C++ core does not:
- no C++ types (no `std::string`, no `std::vector`, no exceptions);
- a stable layout, because other languages read it directly;
- the host owns the pointers it is given only for a limited, documented time.

So someone must (1) turn the question into something C can hold, (2) remember how to answer each question, and (3) keep the answer honest ("already answered", "too late").

## 2. What we built
```
Connection (loop thread)            HostBridge                     C host
 await_decision ──request()──►  asked_[id] = PendingHandle
                                push Handshake event ─────────►  wsrv_poll → WSRV_EV_HANDSHAKE
                                                                  ...thinks, on any thread...
 decide() ◄──verdict (posted)──  PendingHandle.accept()  ◄──────  wsrv_handshake_accept(conn)
                                asked_.erase(id)
```
- `HostBridge` is both the handler factory (it already was) and now the `IHandshakeDecider`.
- It keeps a map `id -> PendingHandle` under one mutex. This is **host-side bookkeeping**: the core cannot tell the host "that id is unknown", only the bridge knows which questions it still has open.

### 2.1 Why the map is filled before the push
Same rule as `on_open`: once the event is in the queue, the host may answer at once, from another thread. If the entry were added after the push, a fast host would find nothing and get "no such connection" for a question it was legitimately asked. Order: **register, then publish**. If the push fails (queue full), the entry is removed again, and the error goes back to the server, which answers the client 503. A test asserts that the failed push leaves nothing behind; a mutation (deleting that cleanup) makes it fail. Without the cleanup, every refused flood request would leak one map entry: a memory attack.

### 2.2 Answer once
`accept_handshake` finds the handle, calls it, and **only if that succeeded** erases the entry. So:
- a second answer finds nothing → `Closing` (C: `WSRV_ERR_NO_SUCH_CONN`);
- an answer that was a *mistake* (`InvalidArgument`: a subprotocol the client never offered, a status like 302) does not consume the question, so the host can try again;
- when the server ends the wait itself (timeout, client left, shutdown), it calls the new `IHandshakeDecider::withdrawn(id)`; the bridge erases the entry. A late answer is then reported honestly instead of "succeeding" into the void.

### 2.3 The blob
The event queue carries `HostEvent { type, conn, data: bytes }`. A handshake has about a dozen fields, some lists. Instead of adding a pointer-rich struct to the event (ownership nightmare, one more thing to copy across threads) the fields are **serialised into `data`**: every string as `u32 length + bytes`, lists with a `u16` count (`capi/handshake_blob.hpp`).
- One encoder, two decoders-to-be: the C layer (this slice) and `wsrvd` (slice 3, for the IPC protocol).
- `decode_handshake` never trusts a length: a length beyond the buffer, a truncation or trailing bytes make it return nothing. Tests feed every truncation of a valid blob, a declared length beyond the buffer, and trailing bytes.
- Lengths, not separators: a header value may contain any byte except what HTTP forbids, and a separator-based format would need escaping.

### 2.4 Handing strings to C
`wsrv_event` gets a pointer to a `wsrv_handshake` made of `wsrv_str { data, len }` (not NUL-terminated: the client controls the bytes, and a NUL inside would silently cut a C string). All of it lives in an `EventBox` allocated by `wsrv_poll` and freed by `wsrv_event_release`: the strings point into the event's own byte vector, which is never moved after the box is built. "Valid until release" is the whole lifetime rule.
- `data` is never NULL, even for an empty string (documented in the header; `c_str` maps an empty view to `""`).
- ABI 0.2 added a field at the end of `wsrv_event`. A caller built against 0.1 passes a smaller `struct_size` and is refused with `INVALID_ARGUMENT` on the first poll: loud, not memory corruption. That is allowed while the major version is 0; from 1.0 the rule becomes "additive only", and this field is exactly the kind of addition 1.0 would have to plan for with `struct_size`.

## 3. Pitfalls we hit
- **The tiny-queue test.** The smallest legal queue holds 2 events, not 1; with `max_events = 1` the bridge refused to be created (a test setup error, found because the failure was in `make()`, not in the behaviour under test). With 2 events the *third* upgrade is the one refused.
- **A mutation that did not build.** Removing the cleanup left an unused variable; `-Werror` stopped the build and the old binary ran, so the "mutant survived". Always check that the mutant compiled before believing it survived.
- **Queue rules did not change.** `Handshake` needs room for itself like `Text`; it is not followed by a `Close` (a refused upgrade never was a connection), so it reserves nothing.

## 4. Exercises
1. Write the test that proves the bridge's map is bounded by `max_pending_handshakes` (hint: `waiting_handshakes()`).
2. The host calls `wsrv_handshake_accept` from two threads at once for the same id. Walk through `find_asked`, `accept`, `forget_asked` and say which call returns OK and which `NO_SUCH_CONN`. Is there a window where both return OK? What does the core do with the second verdict?
3. Why is the blob decoded in `wsrv_poll` (host thread) rather than in `request()` (loop thread)?
4. (Design) Slice 3 sends the same blob over a socket to a sidecar. What must change if the sidecar can be a different version than the daemon?

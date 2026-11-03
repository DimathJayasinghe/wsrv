# Lesson 18 — Asking the application who may connect

## Goal
After this lesson you can explain why a WebSocket server cannot decide *by itself* whether a user may connect, what an **asynchronous decision that fails closed** is, why the new connection phase `AwaitingDecision` stops reading, how a decision is carried safely from another thread to the right connection, and why a counter shared between threads is still lock-free. The decision record is ADR-0018; the edge cases are EC-H27 to EC-H34 in `docs/PROTOCOL_EDGE_CASES.md`.

## 1. The problem
A browser opens a WebSocket by sending an HTTP request (the *handshake*) with a session cookie or a token. The server can check everything about the *request* (syntax, Origin, version). It cannot check the *user*: the user list lives in the application (PHP, over a socket, on another thread).

The workaround "accept everyone, then wait for a login message" is dangerous: anyone can open thousands of connections and hold slots, send data before being authenticated, and the server cannot answer with a proper `401` or `403` (the 101 was already sent).

So the server must **ask the application before it sends the 101**.

## 2. Why not just call the application?
The server's I/O threads run event loops: one thread serves thousands of connections. If the server called application code directly:
- the language runtime would run on an I/O thread (forbidden, `docs/BINDINGS.md`: PHP is single-threaded and not re-entrant);
- one slow answer (a database lookup) would stall every connection on that loop.

So the question is **asynchronous**: the server asks, the connection *waits*, the answer arrives later, possibly from another thread.

## 3. The design in one picture

```
Handshaking --valid request--> AwaitingDecision --accept--> Established   (101 is written NOW)
                                  |    |    |
                                  |    |    +-- reject(status) -> 4xx/5xx, close
                                  |    +------- timeout / queue full / shutdown -> 503, close
                                  +------------ client left -> finished, nothing owed
```

### 3.1 A new phase, and why it stops reading
While the connection waits, it must not read. If a client sent frames right behind its request, reading them now would mean parsing WebSocket frames for a connection that is not open yet, or buffering them without limit. So reading is switched off and the bytes stay **in the kernel's socket buffer**, which has its own bound.

One subtlety: bytes that arrived in the *same read* as the request head are already in our hands, in a buffer shared by every connection of the loop and reused for the next one. Those bytes (at most one 16 KiB read) are **copied** into the waiting connection. After the accept, they are delivered first, in order (EC-H28). A test with a tiny read buffer proves nothing is read and thrown away.

### 3.2 Noticing a client who leaves
With reads off, we still want to know if the client closes. Linux can report `EPOLLRDHUP` (the peer closed its sending side) separately from "data is readable". The poller used to ask for it only together with Read, so a small change (`Hangup` as an interest) lets the waiting phase listen for a goodbye without listening for data. A test uses `shutdown(SHUT_WR)`, a real FIN, because closing a socketpair raises a different, always-reported flag and would hide the bug.

### 3.3 Failing closed
"Fail closed" means: when in doubt, **refuse**. Every way out of the waiting phase that is not an explicit accept sends a real HTTP answer:

| Situation | Answer |
|---|---|
| No answer within `handshake_decision_timeout` (3 s) | 503 + `Retry-After` |
| The decider cannot take the question (its queue is full) | 503 |
| No decider/gate configured but the phase was entered (wiring bug) | 503 |
| More upgrades waiting than `max_pending_handshakes` | 503, at once, never queued |
| Server shuts down while waiting | 503 |
| A forwarded header is too big | 431 (never cut: a half token is a different token) |

The hook is **opt-in**: with no decider injected, behaviour is exactly as before.

### 3.4 Carrying the answer back: `PendingHandle`
The application gets a small copyable object, `PendingHandle`: the connection's *id* plus the loop's `LoopLink` (the same safe doorway `SessionHandle` uses). It holds **no pointer to the connection**. `accept()` / `reject()` do two things:

1. On the **caller's** thread: check the arguments (is the status one of 400, 401, 403, 404, 429, 500, 503? is the chosen subprotocol one the client offered?). A mistake is reported to the caller as `InvalidArgument` and the connection keeps waiting. No application text is ever copied into the response (header injection, information leaks).
2. `post` the verdict to the owning loop. There, `ConnectionTable::decide(id, verdict)` looks the id up. If the connection is gone, already decided or never asked, the verdict is **dropped**. Ids are never reused, so a late answer can never hit another client.

### 3.5 The bound: a lock-free counter and a slot you cannot forget
At most `max_pending_handshakes` upgrades may wait at once across *all* loops. A mutex in connection code is forbidden, so the bound is an `std::atomic` counter with a compare-and-swap loop:

```cpp
std::size_t seen = waiting_.load();
while (seen < bound_)
    if (waiting_.compare_exchange_weak(seen, seen + 1)) return PendingSlot{*this};
return std::nullopt;
```

`compare_exchange_weak` says: "set it to `seen + 1` only if it is still `seen`; otherwise reload `seen` and try again". Two threads can never both take the last place.

The place comes back as an RAII object, `PendingSlot`. Its destructor gives the place back **exactly once**, however the wait ends (decision, timeout, disconnect, shutdown, abort, even the connection being destroyed). This is the classic spot where counters leak, so every exit has its own test.

## 4. Walkthrough: one accepted upgrade
1. The request head completes; the negotiator says "valid"; `HandshakeOutcome` carries the rendered 101, the subprotocol and (new) the **accept key**.
2. `Connection::await_decision` takes a `PendingSlot`, builds the `HandshakeRequest` for the application (path, query, Origin, Host, offered subprotocols, only the listed headers), parks the outcome and the copied leftover bytes, switches the interest to `Hangup`, arms the decision timer (reusing the handshake timer slot), and calls `decider->request(...)`.
3. The application, on its own thread, calls `handle.accept()`. The verdict is posted to the loop.
4. `Connection::decide` accepts: frees the slot, cancels the timer, optionally re-renders the 101 (an application may pick another offered subprotocol, which is why the accept key was kept), creates the handler and feeds the leftover bytes. The next `settle()` writes the 101 and restores `Read` interest. Changing the mask is what makes the kernel report any data that arrived meanwhile (edge-triggered re-arm; the same trap as lesson 16).

## 5. Pitfalls we hit
- **`pending_.reset()` forgotten in `release()`** would leak a place on abort. A mutation test removes it and two tests fail.
- **A loop guard that looked redundant.** `on_readable` keeps reading in a loop. Without a check for the new phase, a head that completes in the middle of the loop lets the *next iteration* read and discard frame bytes. A mutation proved no test noticed until we added one with a 128-byte read buffer.
- **`max_pending ≤ max_connections` as a hard validation** would have rejected every small server, because the default (256) is bigger. It is a clamp instead (`effective_max_pending_handshakes`).
- **`Server::stop()` is not a graceful shutdown.** It ends the loop at once; `request_shutdown()` is the graceful path that answers waiting clients.

## 6. Exercises
1. Change `kRetryAfterSeconds` to 30 and find which tests encode the old value. Why is it a constant rather than something the application chooses?
2. A client sends 1 MiB of frames right behind its request and the application takes 2 s. Where do those bytes live, and what bounds them?
3. Why does `PendingHandle` validate the subprotocol on the *calling* thread instead of on the loop?
4. Write the test that proves `accept()` called twice from two threads leads to exactly one `on_open`.
5. (Design) Slice 2 will queue `HandshakeRequest` objects for the C host. What should happen when that queue is full, and why is "accept anyway" wrong?

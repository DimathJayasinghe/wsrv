# Lesson 53 — A budget that cannot drift: one counter for every connection's buffers

## Goal
After this lesson you can explain why a limit on each connection does not limit the server, how a shared counter can be made so that it never drifts (the way a leaked reference count does), what a soft cap does and does not promise, and how a test finds a counting bug that reading the code did not.

## The problem
Every connection was already bounded: a message that has started waits in the assembler for at most `max_message_size` (1 MiB), and the output queue holds at most `max_queued_bytes` (1 MiB). `max_connections` bounds how many connections exist. Multiply them and the worst case is about 20 GiB, more than most machines have. Two cheap ways to get there:
- **Many half-sent messages.** Each client sends the first fragments of a 1 MiB message and stops. The server keeps the bytes (it must: the message is not complete, and a text message must be checked as a whole). The attacker pays only what it sends, and per-address limits do not help behind a proxy or from many addresses.
- **Many slow readers.** The application broadcasts and every queue fills towards its 1 MiB limit before the slow-consumer policy closes the connection.

The result is not a refusal. The operating system kills the whole process, and every client is dropped, not only the greedy ones. Our own security document said "total buffered bytes (soft cap → shed new connections)" while no such thing existed: a promise in a document that the code does not keep is a bug in the document or in the code, and here it was in the code.

## 1. The design in one picture
One `std::atomic<std::size_t>` for the server (`Server::buffered_`). Each connection owns a small `BufferLedger` that knows how much *it* has charged to that counter:

```cpp
void set(std::size_t now) noexcept {
    if (now == charged_) return;                       // the common case: no atomic operation at all
    if (now > charged_) total_->fetch_add(now - charged_, relaxed);
    else                total_->fetch_sub(charged_ - now, relaxed);
    charged_ = now;
}
~BufferLedger() { release(); }                          // release() is set(0)
```

Two properties carry the design:
- **By difference.** A connection never says "the total is now X"; it says "I hold `now`, I held `charged_`", and adds the change. Thousands of connections change their own number in any order and the counter is always the sum of what each holds.
- **The destructor refunds.** Whatever ends a connection (the peer hangs up, the server aborts it, a shutdown, an exception path nobody thought of) the object is destroyed, and its destructor gives back exactly what it charged. There is no "remember to decrement on every exit path", which is how a counter drifts upward until the server refuses everyone for ever. The drift cannot be made smaller by being careful; it has to be made impossible by construction.

`Connection::settle()` is the one function every read, write, command and timer passes through, so the ledger is updated in one place (`account_buffers()`), not at every place a buffer might change.

## 2. What happens over the limit, and what does not
Above `max_buffered_bytes` (default 1 GiB) the server answers a **new** client with `503` and `Retry-After: 1`, before it takes a per-address slot. Connections that exist are not touched.

That is a **soft cap**, and the word matters. It does not promise that the sum never exceeds the limit: between two accepts the admitted connections can still add bytes, so a burst overshoots by what they can add. It promises that new work is refused while the server is over, and that it is accepted again as soon as the sum drops. A **hard cap** would close live connections to get back under the limit, which needs a policy (who goes first?) that the owner should choose; it is a follow-up, written down, not a hidden gap.

Why not refuse the *message* that would cross the line instead of the client? Because the check would sit in the protocol code, which is meant to know nothing about the server, and because the refused client would already hold a connection, a slot and a handshake. Refusing at accept is the cheapest place and costs the attacker its connection, not us.

## 3. The bug the test found, not the review
The first design counted output as `queued_output_bytes()`, the bytes waiting in the output queue. The test "a client that reads nothing keeps its echo counted" failed: zero. The reason: as soon as the transport asks the session for something to write, the first message leaves the queue and is framed into the wire buffer. A client that reads nothing keeps *that* message there. The queue is empty, the counter says zero, and the budget would never have seen the very client it exists for.

The fix is `held_bytes()`: the queue plus the wire buffer. The lesson is the order: the test was written before the code, described what the operator needs ("bytes the peer has not taken"), and failed against an implementation that described what the code happened to call "queued". A name in the code is not a specification.

## 4. Proving the counter cannot drift
Three levels, each with its own test, and a **mutation check** for each (change the code so the property is false, and watch the tests fail; a test that cannot fail proves nothing):
- `BufferLedger` alone: two ledgers add up, each gives back only its own, `release()` twice refunds once, and four threads create, change and drop ledgers in any order against one counter, which must be exactly zero at the end (run under TSan in CI).
- `Connection`: a partial message is charged and refunded when the message completes; a queued echo is charged until written; the charge returns when the peer hangs up, when the server aborts, and when the object is destroyed while still holding bytes.
- The real server with three loops: clients that each leave a message half sent, then end in every way a connection can end; the gauge returns to exactly zero, and with the budget small enough, new clients get the 503 while the existing ones keep working.

The mutation "drop the refund" fails tests at all three levels; the mutation "drop the accept check" fails the shed test. Both were run, and both were reverted.

## 5. What is deliberately not counted
The permessage-deflate state (about 96 KiB per connection, bounded by `max_connections`), the shared read buffer, the handshake head, TLS records in flight, and direct sends waiting for a loop (they have their own budget, ADR-0032). The ADR lists each with its reason; a budget that tried to count everything would be a second memory allocator.

## Pitfalls
- A limit per object is not a limit on the sum of the objects.
- A counter that is decremented on exit paths will leak on the one nobody listed. Tie the refund to destruction.
- `memory_order_relaxed` is enough here because the counter orders nothing else; it is read to make a decision that tolerates a slightly old value (a soft cap).
- Do not take a lock for a number that only has to be roughly right and cheap to update: the common case (`now == charged_`) does no atomic operation at all.
- A document that describes a control the code lacks is a defect. Either build the control or correct the document, and say which.
- Name what a counter measures from the operator's side ("bytes the peer has not taken"), then test that, not the name the code used.
- Set the limit from the memory limit of the process, not from `max_connections`: a healthy server holds almost nothing here, so a limit well below the default sheds nothing in normal use and turns an out-of-memory kill into a 503.

## Exercises
1. A burst of 1 000 clients connects in one millisecond, each about to buffer 100 KiB, while the sum is 10 MiB below the limit. How far over can the sum go, and why is that acceptable for a soft cap but would not be for a hard one?
2. `Connection::account_buffers()` returns early when the connection is released. What goes wrong if it does not? (Hint: look at what `release()` does after `ledger_.release()`.)
3. Why is `fetch_add` with `relaxed` order enough, and what would need a stronger order?
4. Sketch the hard cap: which connection do you close first, what close code does the peer see, and what could an attacker do to be the one that is *not* closed?
5. Set `max_buffered_bytes` for a pod with a 1 GiB memory limit, 10 000 connections and permessage-deflate on. Show the arithmetic.
6. Why does the server refuse a new client at `accept` instead of refusing the message that crosses the limit? Name two costs of the second design.

# Lesson 16 — The sidecar: moving events without losing any

## Goal
After this lesson you can explain how `wsrvd` carries WebSocket events to an application over a Unix socket and carries commands back, why a slow application slows its clients instead of losing messages, what an *edge-triggered* wake-up is and the trap it sets, and what happens to connections when the application disappears. The wire format itself is in `docs/IPC_PROTOCOL.md`; decisions are in ADR-0017 (section 4c).

## 1. The problem
The library already has a bounded queue of events (lesson 15). In embedded mode the host pulls from it. In sidecar mode the host is another process, so something has to pull for it and write the events into a socket. That something must answer three questions honestly:

1. What if the application reads slowly? Unbounded buffering in the daemon is a memory-exhaustion bug waiting for a slow or hostile local process.
2. What if there is no application at all (startup, restart)?
3. What if the application dies holding connections it alone knew about?

## 2. Design

### The seam: `IBackend`
`apps/wsrvd/backend.hpp` is a five-function interface: `poll`, `event_fd`, `send_text`, `send_binary`, `close`. The daemon talks to it, never to `EmbeddedServer`. A thin adapter (`EmbeddedBackend`) wires the real server in `main`. Tests use `FakeBackend`, which sits on the *real* `EventQueue`, so bounds and wake-up semantics are real and a test can read `pending()` to prove events stayed queued.

### `Relay`: the rules without sockets
`Relay` (`relay.hpp`) holds two sets of connection ids: *open* (the application was told about the Open and not yet the Close) and *stale* (known to a previous application). It decides what the application sees and what its commands may do: unknown id gives `NO_SUCH_CONN` without calling the backend; request id 0 is refused; backend errors map to `wsrv_status` with the C API's own table. Because it has no sockets, 11 unit tests cover it in milliseconds.

### The pump and backpressure
The pump moves events from the backend into the IPC output buffer, then writes the buffer to the socket. Backpressure is *not stopping the writes*; it is **not taking the next event**:

```
while out buffer < high water:   take one event from the backend, encode it into the buffer
flush the buffer to the socket
```

At high water the daemon stops polling. Events stay in the server's bounded queue; that queue's own high-water mark pauses reading on the WebSocket connections (ADR-0016 section 4); the browsers' TCP windows fill. A slow application therefore slows its clients, and **no event is dropped anywhere**. The check comes *before* the poll, so an event is never taken that cannot be stored.

Commands need the same care. Every command is answered, so an application that sends commands but never reads answers could grow the output buffer. Above high water the daemon also stops *reading* the socket. The buffer is sized `2 * max_frame + 16 KiB`: one frame below high water, one more the pump may add, plus the answers to one read (a command is at least 18 bytes and its answer 11).

### The edge-trigger trap
`epoll` can report a descriptor *level-triggered* ("it is readable now, and now, and now") or *edge-triggered* ("it just became readable"). The loop is edge-triggered. The event queue writes its `eventfd` only when it goes from empty to non-empty. Put together: if the pump stops at high water while events are still queued, **no later push produces another edge**, and the daemon would wait forever with a full queue.

The fix is two rules:
- When the output drains (a write-readiness event, then a flush that empties the buffer), the pump **restarts itself**.
- When the pump stops only because its turn budget ran out, it asks the loop for a fresh edge (`rewatch`).

And one guard: `pump()` refuses re-entry, because pump, flush, drained, resume would otherwise recurse.

### When nobody is connected
The daemon takes no events. WebSocket handshakes complete and clients are held behind the same backpressure until an application arrives; its `HELLO_OK` starts the pump (the edge for those events was spent long ago).

### When the application goes away
- **Stall** (it stops reading): a timer starts when a write blocks with data pending, restarts on any byte of progress, and is cancelled when the buffer empties. On expiry the link is closed and the connections it knew are closed with **1001**.
- **Disconnect**: the connections it knew become *stale*. Their later events are discarded (the one place anything is discarded: nobody can serve them) and after the grace period they are closed with **1012**. Connections it never saw wait for the next application.

## 3. Tests: what proves it
- `ASlowApplicationHoldsEventsInTheQueueInsteadOfLosingThem`: 1500 events of 4000 bytes, the application reads nothing; `pending()` stays high, the backend poll count stops growing, then the application reads and receives all 1500 in order with `dropped() == 0`.
- **A test is only worth its failure.** I deleted the "resume pumping after drain" line and re-ran: both backpressure tests failed (they timed out waiting for events). That is how you know they guard the edge-trigger trap.
- End-to-end: a real WebSocket client through the real `EmbeddedServer` and `IpcServer` to an IPC client: message round trip, application-initiated close, application death (browser sees 1012), early browser held until the application arrives.
- TSan (the loop thread and the WebSocket I/O threads share the queue) is clean.

## 4. Pitfalls
- Polling "just in case" in the stalled state: it turns backpressure into a busy loop. The test asserts the poll count does not grow.
- Treating a full socket as an error. `EAGAIN` means *wait for write readiness*.
- Forgetting that `Hangup` is not reported through reads when you have turned reading off.
- Destroying the IPC server after the backend: it unwatches the backend's descriptor. Order: IPC server, then WebSocket server.
- Docker Desktop's stale bind mount (CLAUDE.md): after replacing a file by rename, compare `wc -l` and run `make restart`.

## 5. Exercises
1. Change the buffer size to `max_frame + 1` and find which test or reasoning shows it is too small.
2. Why must the stall timer restart on *any* progress and not only when the buffer empties? Write the test for an application that reads one byte per second.
3. Sketch what a true "pause accepting" backend call would change in `server/`, and why it needs its own ADR.
4. Add a `STATUS`-less command (fire and forget) to the protocol on paper. What breaks about request id 0?

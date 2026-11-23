# ADR-0031: A small JavaScript client with reconnect (browser and Node)

- Status: **Accepted** (2026-11-23; 1.0 production-readiness work, owner-delegated scope)
- Date: 2026-11-23
- Deciders: project owner, maintainer assistant

## Context
Restarts are normal in production: a deploy, a node drain, the worker restarting. The daemon closes every WebSocket with 1001 (shutdown) or 1012 (the application left), and **browsers do not reconnect on their own**. Every application therefore writes the same loop: exponential backoff with jitter, a fresh single-use ticket for every attempt (docs/LARAVEL.md), and re-joining the rooms the page was in. Without it, the first deploy silently turns every open page into a dead one. The server side already has its half (`Wsrv\Supervisor`, ADR-0030 deployment guidance); this is the client half.

## Decision
A dependency-free ES module in `clients/js` (`WsrvClient`), for browsers and Node 22+ (both have a global `WebSocket`; another implementation can be injected for older Node).
- **Reconnect:** exponential backoff (default 500 ms doubling to 15 s, each delay jittered to 50-100%), reset only after a connection stayed open for `resetAfter` (10 s) so a flapping server is not hammered. Retries stop on `close()`, on a close code of 1008 (policy: the server refused this client, retrying cannot help) or when `shouldReconnect` says so, and after `maxRetries`.
- **Ticket:** `ticket` is a string or an (async) function **called before every attempt**, appended as the `ticket` query parameter. A failing function is an `error` event and a backoff, not a crash.
- **Rooms:** `join(room)` / `leave(room)` remember the room and send the application's message (default `join <room>` / `leave <room>`, the protocol of the examples; `joinMessage`/`leaveMessage` override it). The remembered rooms are re-sent after every reconnect. The daemon never lets a browser name a room (ADR-0028): the worker decides on that message, so the wire format is the application's, not wsrv's.
- **Offline sends:** `send()` returns `false` unless the socket is open; with `queueWhileOffline` up to `maxQueue` (100) messages wait for the next connection. Nothing is queued silently by default.
- **No wire protocol of its own.** It speaks plain WebSocket; it works against any server, and needs no wsrv-specific frames.
- **Out of scope:** client-side heartbeat (browsers cannot send Ping frames; the server pings, ADR-0016), message acknowledgements and replay (at-most-once delivery stays the contract, IPC_PROTOCOL.md), a TypeScript build step (a hand-written `.d.ts` ships instead).

## Consequences
+ A deploy or a restart is a short blip, not dead pages; the ticket and room patterns are solved once and tested against a real daemon.
+ No dependency, no build, one file to audit.
− Messages sent while disconnected are lost unless the application opts in to the bounded queue; replay of messages missed during the gap needs application-level sequencing.
− A second language to test and release: the JS tests run in CI next to the PHP ones, and the package is versioned with the rest.

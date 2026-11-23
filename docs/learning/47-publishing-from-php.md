# Lesson 47 — Publishing from PHP: who decides, who sends

## Goal
After this lesson you can explain the split between the worker (decides who joins a room) and the publisher (sends), why the PHP client checks topics before it sends, and why `PublishFailed` tells you whether to retry.

## The problem
A PHP application has two kinds of code. A long-running worker holds the IPC link and sees every WebSocket connection. Everything else (a PHP-FPM request, a queue job, a cron script) is short-lived and cannot hold a link. Rooms need both: someone must put a browser in a room, and someone else, often a controller, must send to it.

## 1. The worker decides membership
A browser never names a room to the daemon. It sends a message in the application's own protocol, the worker looks at the session, and only then calls `$conn->subscribe('room.7')`. The daemon trusts the worker because the IPC socket is private to one uid; it would not trust a browser. A worker can only subscribe connections it was told about (`NO_SUCH_CONN` otherwise), so one application cannot reach another's connections.

## 2. The publisher only sends
`Wsrv\Publisher` makes one HTTP request per publish: `POST /publish?topic=...` with a bearer token. It needs no extension and no connection to keep. The 200 body has the counts, which `PublishResult` carries: how many were in the room, took the message, or had a full queue.

## 3. Refuse early, in the client
Topic names are checked in PHP against the broker's alphabet before anything is written (`Codec::topic`), and the publish token must have no whitespace. This is not security (the daemon checks everything again) but a clear exception at the call site instead of a `400` from the network, and it keeps newlines out of the HTTP request the client builds. The golden-vector tests write the frame bytes by hand from `docs/IPC_PROTOCOL.md`, so an encoder that is wrong in a self-consistent way still fails.

## 4. Failures that say what to do
`PublishFailed` carries the HTTP status and `isRetryable()`: `429` (every subscriber's queue was full), `503` (shutting down) and "could not reach the daemon" are worth retrying later; `401`, `400`, `413`, `415` are mistakes that fail the same way next time. A retry loop that retries a `401` forever is a bug the type prevents.

## 5. Proof against the real thing
`interop.php` starts a real `wsrvd` with the broker and a token file, connects real WebSocket clients, subscribes over IPC, publishes over HTTP and over IPC, checks `except`, the error statuses, and that a closed browser leaves its rooms (no orphans). The daemon now prints `publish endpoint listening on port N` so a test can use port 0.

## Exercises
1. Why can the worker publish over IPC while a controller cannot? What would it cost to let it?
2. A queue job retries on `isRetryable()`. What must the message tolerate if the first attempt actually reached some subscribers before the answer was lost?
3. Add `https://` support to `Publisher`. What must you decide about certificate verification, and how would you test it?

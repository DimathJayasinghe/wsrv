# M7 — Multi-thread: implementation plan

Goal (PROJECT_PLAN): N loops, connection distribution, cross-thread `SessionHandle`. Exit: TSan clean, soak test, scaling benchmark documented. Design: ADR-0014 (Proposed). Slices are ordered so each one ends green and mergeable; `threads = 1` must stay identical to M6 throughout (the M6 suite is the regression guard).

## Task 1: `LoopLink` and `SessionHandle` (single loop first)
**Files:** `src/server/session_handle.hpp/.cpp`, `src/server/handler.hpp` (`Session::handle()`), `src/server/connection.*`, `src/server/connection_table.*` (lookup by id), `tests/server/session_handle_test.cpp`.
- Tests first: a handle used from another thread delivers a message in order; after the connection closed, after the table dropped it, after the shard's link was closed, and with a full task queue (`BufferFull`) it returns an error and nothing crashes (EC-N13, EC-N14). TSan and `--gtest_repeat`.
- Lifetime rule: the handle never holds a pointer to a connection; the posted task looks the id up on the loop thread.

**Status:** done. A bug the end-to-end test caught: a command posted from a foreign thread queued its message but nothing wrote it, because in-loop callbacks are followed by `settle()` and posted commands were not. `Connection::apply` now runs the command and then settles; `ISessionDirectory::run` replaced `find`. Also worth knowing: `close()` drops every unsent data message (documented in the outbound scheduler), so "send, then close" from one command loses the message unless it was already written; `SessionHandle` runs each call as its own command, so the text is written before the close.

## Task 2: extract `LoopShard`
**Files:** `src/server/loop_shard.hpp/.cpp`, `src/server/server.*`.
- Move loop, host, table, scratch, context out of `Server` into a shard; `Server` owns shard 0. No behaviour change: all M6 tests pass untouched.
- The member-order rule of `server.hpp` becomes the shard's.

## Task 3: N loops
**Files:** `src/server/config.*` (`threads`, ceiling), `src/server/server.*`, `server_builder.*`.
- Tests first: threads = 4, many clients get echoed and are spread over the loops; the global cap holds across loops; ids are unique; a refused client costs one close.
- Worker threads start inside `run()`, construct their shard on their own thread, report ready, then run. Round-robin hand-off by `post` (the descriptor is moved into the task; if the post is refused the descriptor is closed and counted).
- Graceful and forced shutdown across loops; signals on loop 0; join all threads before `run()` returns.

## Task 4: hammer it
**Files:** `tests/integration/multithread_test.cpp`, `tests/bench/`, `tests/soak/`.
- TSan integration: 4 loops, 200 clients, handle sends from 4 foreign threads while connections churn and the server shuts down.
- Scaling benchmark (1, 2, 4 loops, echo of small messages over loopback) with the result and the machine written to `docs/`.
- Soak script (1 h, churn plus traffic, fd and RSS sampled). Run it, or state plainly that it has not run.

## Task 5: `MessageWriter` (FR-F12, EC-F18, F33..F35)
Needs its own small ADR (streaming send while a connection is backpressured: what happens to a half-written message when the connection closes). Decide after Task 3.

## Task 6: lesson 12, docs, gates, PR
Lesson 12 "Threads without locks"; ARCHITECTURE sections 3.6 and 5; EDGE_CASES status (N13, N14); SECURITY_DESIGN T16; PROJECT_PLAN status. Gates: dev, dev-clang, tsan, tidy, release.

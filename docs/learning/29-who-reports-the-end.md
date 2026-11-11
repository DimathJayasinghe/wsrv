# Lesson 29 — Who reports the end? (four small bugs from a code review, M10 slice 0)

## Goal
After this lesson you can explain why "every Open is followed by exactly one Close" has to be kept by the code that *destroys* things, not only by the code that closes them on purpose; why a failed hand-off must be retried instead of forgotten; why a timer must never outlive the object it points at; and why "WouldBlock" does not always mean "the kernel is empty".

## Background: a review that only read code
A second reviewer read the code (nothing was run) and reported five claims. We treat a claim as a *hypothesis*: write a test that fails for the stated reason, only then fix. Four were real (one, the missing per-IP caps, is M10 work and not a bug). Each fix below has a test that was red first, and a mutation check (put the bug back, watch the test go red again).

## 1. The host must see one Close for every Open (EC-N22)
The host (a C program, PHP, `wsrvd`) keeps a table of connections. It adds one on OPEN and removes it on CLOSE, so a missing CLOSE is a leak in *its* memory and a stuck user in *its* UI.

The server has several loops (threads). Each owns a *shard*: its connections, its timers. On a forced shutdown, loop 0 aborted its own connections (which calls `on_close`) and then stopped. The other shards were just destroyed, and destroying a `Connection` closes the socket but never calls the handler. Test result before the fix: 2 of 6 Closes on a forced shutdown, **0 of 6** on `stop()`.

The rule that fixes it: whoever owns the object reports its end. `~LoopShard` now calls `abort_all()` on its own thread before it dies (a handler may only run on the thread of its loop, so loop 0 cannot do it for the others). Order matters and is documented in the class: first close the *link* (a foreign thread holding a `SessionHandle` gets `Closing`), then abort.

## 2. Never forget a hand-off that failed (EC-N23)
When the host's event queue is full the bridge pauses reads of the connection that caused it and writes its id on a "parked" list. When the host drains the queue, the bridge resumes every parked connection by posting a command to its loop. A loop's task queue is bounded (4096), so the post can fail with `BufferFull`. The old code cleared the parked list *before* posting and ignored the result: a refused connection stayed paused until the idle timer.

The pattern: **remove from the to-do list only after success**. Here the list is cleared first (so no call into a loop happens under the mutex), and what the loop refused is put back; the next `poll()` retries. `Closing` is different: the connection is gone, there is nothing to retry.

## 3. A timer must not outlive its object (EC-N24)
`Connection::establish()` ran `on_open`, then armed the keep-alive timer. If `on_open` paused reading and the registration change failed, the connection finished (and was released) *inside* `on_open`, and the timer was armed afterwards. `release()` had already run, so nothing cancelled the new timer: later it would call a member function of a freed object. Two fixes, because one is a patch and one is a rule: `establish()` returns when the connection finished, and `arm()` refuses to arm anything on a released connection.

## 4. WouldBlock is two different answers (EC-N25)
For a plain socket "WouldBlock" means the kernel has nothing more; edge-triggered epoll will wake us when there is. TLS is different: it reads records from the socket into its own memory and decrypts them. To be fair to other connections it stops after 64 chunks and answers WouldBlock *with input left inside it*; the kernel has already handed that data over and will never announce it again. `ITransport::buffered_input()` says "I still hold input"; the connection ignored it on WouldBlock and waited for an event that never came (the peer's data sat until the idle timer closed the connection). Now it asks, and schedules another read on a later turn of the loop.

While writing the test, a fake was found to be unrealistic: it kept saying "I hold input" forever, which made the fixed code loop forever. A fake must stop claiming what the real thing stops claiming (a hang in a test is also a finding about the test double).

## Tests
`MultiLoop.AForcedShutdownReports…`, `MultiLoop.StopReports…`, `EmbeddedServer.ClientsThatIgnoreTheShutdown…` (the host's view), `HostBridgeResume.AResumeTheLoopRefused…`, `ConnectionTest.AConnectionThatEndedInsideOnOpen…`, `ConnectionTest.ATransportThatYielded…`.

## Pitfalls
- Counting only the paths that "close on purpose": the ones that matter for leaks are the paths that end the object some other way.
- `static_cast<void>(x.has_value())` on a result that can say "try later".
- Arming a timer (or registering any callback) after a call that may have ended the object.
- Treating one error code as one meaning; read what each transport promises.

## Exercises
1. Why can loop 0 not simply call `abort_all()` for every shard from `force_shutdown()`? (Hint: which thread may run a handler, and what `post` to a stopping loop does.)
2. `resume_parked()` is also called when the queue is empty and not under pressure. Why is that "safety net" the right place to retry, rather than a timer?
3. Put the `MUTANT` back into `Connection::on_readable` (drop the `schedule_reread()` call) and watch the test fail. Which assertion fires, and why does the test not need a real TLS socket?

# Lesson 55 — A quickstart you can verify, and claims you can check

## Goal
After this lesson you can explain why a project's first five minutes are an engineering artifact (not marketing copy), how to test a quickstart as if you were a stranger, why every claim on an "evidence" page needs a command next to it, and why an "is this for you?" table that sends readers to competitors builds trust instead of losing it.

## The problem
wsrv at 1.0 was correct and well tested, but a newcomer had to read about ten documents to learn that. Three things were missing:

1. **A first success that shows something.** The old quickstart ended in `npx wscat`: a terminal, a typed word, an echo. Nothing a PHP developer could show a colleague.
2. **A way to decide.** Nothing said when *not* to choose wsrv. A reader who suspects that a page oversells will discount everything on it.
3. **A way to check.** The test results existed (Autobahn, fuzzing, soak runs) but were spread over six documents, none saying "run this and you will see it too".

## 1. A quickstart is a test you do not control
The author's machine has the right Docker cache, the right ports free, the right image. A reader has none of that. So the quickstart was *run*, end to end, the way a reader would, and what went wrong is the lesson:

- **A stale cached image** (the PHP worker image had an old `wsrv/client`) crash-looped with `Class "Wsrv\Supervisor" not found`. The fix for the quickstart: both PHP services build from one definition (`x-php-image` anchor in `compose.yaml`), pinned by the `WSRV_CLIENT` argument.
- **A busy port** (8080 was taken). The page therefore takes the daemon's address from an input field, and the hint says which environment variables to set.
- **The Origin check** (RFC 6455 section 10.2) is on by default, so a page on `http://localhost:8000` is refused unless the daemon lists that exact origin. The compose file sets `WSRVD_ALLOW_ORIGIN` to match `WSRV_WEB_PORT`. A quickstart that disables a security default "to make it work" teaches the wrong thing.
- **Restart behaviour is the point.** The page has no special code for it; `clients/js` reconnects and joins its rooms again, and the log shows `closed: code 1001 (server shutting down)`, a retry in about half a second, `reconnected`, `joined room.1`. A thing you can watch is a thing you believe.

How: bring the stack up in a scratch project name, drive the page in a real browser (connect, send, join, publish from a second process, restart the daemon), read the console, tear everything down, and delete any stand-in image you built so it cannot mask the real one later.

## 2. The page itself is a small security exercise
`examples/docker/web/index.html` shows text that came from the network. It uses `textContent` for every line, never `innerHTML`, so a message such as `<img src=x onerror=...>` is printed, not run. The "publish" command shown to the user is rebuilt from the room name only when the name matches the room alphabet the worker accepts, because that string is meant to be pasted into a shell.

## 3. Claims paired with checks
`docs/EVIDENCE.md` has four columns: **claim, how to check it, last result, what that result does not show**. The last column is the one that matters. Three rules kept it honest:

- A number is copied from its source document, with the date, and the source wins if they disagree. Where a number could not be traced (a test count, a download size) it was removed, not rounded.
- A command is only listed if the target, preset or script exists in the repository. They were checked against `CMakePresets.json`, `cmake/WsrvChecks.cmake` and `.github/workflows/ci.yml`.
- What is not evidenced has its own section: no external audit, young project, Laravel glue tried by hand, macOS and Windows end to end not tested on real machines, no multi-node backplane.

## 4. Telling people when to go elsewhere
The README's "Is wsrv right for you?" table recommends Laravel Reverb for an app that wants `broadcast()` and Echo through environment variables, and Workerman or Swoole for a one-process PHP app, because the benchmarks show wsrv behind them per core on echo. It recommends wsrv for the cases where its design pays: connection handling in a separate hardened daemon, a PHP worker that only reacts to events, and throughput that grows with cores. A reader who finds the weakness stated plainly believes the strengths.

## Pitfalls
- **Copying a number into three documents.** Each copy drifts. Link to the source and summarize once.
- **A quickstart that depends on something you pull.** Everything the stack needs is either built from the repository or the one published image. The static page is served by the PHP image already built, so there is nothing extra to download.
- **Documenting the version you are building.** The compose file targets the 1.1 image, which exists only after the release; the docs on the development branch describe 1.1, and the release gates include checking that the image tag exists.
- **A demo server mistaken for a feature.** PHP's built-in server serves the page in the demo, bound to your loopback. It is said to be a demo convenience in the compose file and the README; the daemon never serves files.

## Exercises
1. Change `WSRV_WEB_PORT` to 18000 and start the stack. What breaks if you forget the daemon's `WSRVD_ALLOW_ORIGIN`, and which log line tells you?
2. Add a fifth row to the "Is wsrv right for you?" table for a case you know. What evidence would you want in `EVIDENCE.md` before writing it?
3. Pick one row of `EVIDENCE.md`, run its check on your machine, and compare with "last result". If they differ, which document do you fix?
4. The page prints `data.byteLength ?? data.size` for binary messages. Why does it not try to decode them?

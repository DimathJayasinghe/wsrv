# Lesson 38 — The login check that never runs (handshake hook in the embedded binding)

## Goal
After this lesson you can explain why a security hook must fail closed, why the *configuration* is part of that, and how the embedded PHP server answers a browser's upgrade without ever accepting by accident.

## The problem
A browser sends its session cookie with the WebSocket upgrade. Only the application can say whether that session is valid, so the library asks (ADR-0018): it holds the upgrade, hands the host a HANDSHAKE event, and waits for `accept` or `reject`. The embedded binding (ADR-0025 A1) turns that into the same `authenticate` handler the sidecar has: `true`, `false`, a status, a subprotocol, or `null` ("I will answer later").

The mechanics are small. The lesson is in what can go wrong around them.

## 1. Two ways a login check silently does nothing
1. **The hook is on and nobody answers.** Every upgrade waits out `handshake_decision_timeout_ms` and then gets 503. Not insecure, but every browser is refused and nothing says why.
2. **The handler exists and the hook is off.** The library never asks. The application author wrote `on('authenticate', ...)`, sees it in the code review, and believes logins are checked. Every browser gets in. This is the dangerous one: the code reads secure and is not.

Neither is a bug in any single piece; they are a disagreement between two places (a setting and a handler). So `start()` compares them and throws, naming the missing half. A check that fails when the two disagree is cheaper than a security incident that starts with "but we had an authenticate handler".

## 2. Fail closed, path by path
Every path that does not say "accept" says "refuse":

| What happens | Answer |
|---|---|
| handler returns `true` / a subprotocol it was offered | accept |
| `false` | 403 |
| an int in 400 401 403 404 429 500 503 | that status |
| `null` | wait; the handler answers through the `Upgrade` it kept, or the library answers 503 at the timeout |
| the handler throws | 500, then the exception surfaces from `step()` |
| any other return value (a string it was not offered, an array, 200) | 500 and `UnexpectedValueException` |

The tests check the two failure rows against a real client: a handler that throws and one that returns `'banana'` both produce an HTTP 500 on the wire. "It throws, so surely nothing was accepted" is a claim; the status line is evidence.

## 3. The same two rules as every other event
- **Copy before release.** The request strings (`path`, `Cookie`, offered subprotocols) belong to the event; `step()` copies them into the `HandshakeRequest` before `wsrv_event_release`. The handler may keep the `Upgrade` for later, so it must never hold a pointer into library memory.
- **Everything in the request is the browser's.** `cookie('session')` returns what was sent, undecoded. Validate it; do not trust it because it arrived over a "validated" upgrade (the library validated the *syntax*, not the person).

## Pitfalls
- The test's wait condition `fn () => count($seen) === 1` never became true: an arrow function copies `$seen` when it is created (lesson 37, pitfall 4, made again by the author of the lesson). Closures that wait for a change use `use (&$seen)`.
- An accepted upgrade's `open` event arrives on a *later* `step()` than the accept. A test that counts opens right after the accept races; wait for the open first.

## Tests
`embedded_handshake_interop.php` (CTest `php_embedded_handshake_interop`, release builds): good cookie accepted and readable in the handler, forged cookie 403 with no `open` event, a status, a subprotocol, answering later (and answering twice is a `LogicException`), a throwing handler is 500, an unusable return is 500, and both configuration mismatches throw at `start()`.

## Exercises
1. Return `true` from a handler for a request whose `Origin` you never checked. Which layer already refused a wrong Origin before your handler ran (`allow_origin`), and why is the cookie check still needed?
2. Answer later from a timer after `handshake_decision_timeout_ms`. What does `accept()` throw, and what did the browser already receive?
3. Add a `handshake_forward_header` for `Authorization` and read it with `$u->request->header('Authorization')`. What changes in the log policy for this server (hint: OPERATIONS.md on secrets)?

# Lesson 46 — An endpoint an attacker can reach: order of checks, secrets and bounds

## Goal
After this lesson you can explain why the publish endpoint compares the token before it looks at the declared length, why the token has no config key, and which bound stops which abuse.

## The problem
`POST /publish` is a door into every room. It is reachable over a network (in containers, from other containers), so it must be safe against someone who can send any bytes: guess the token, claim a huge body, send half a request and stop, or hide a second request inside the first.

## 1. The order of checks is the security property
The endpoint does, in this order, and stops at the first failure:
1. **Parse the head** (strict, at most 4 KiB). Nothing is allocated from peer data.
2. **Interpret it** (lesson 45): method, target, framing, query. These refusals reveal nothing secret.
3. **Compare the token** in constant time (`base/constant_time.hpp`, moved from `wsrvd` so both use one function).
4. **Check `Content-Length` against the bound** (413).
5. **Only now reserve the body buffer**, sized by a number that is both authenticated and bounded.

If steps 3 and 4 were swapped, anyone could make the daemon reserve memory by sending `Content-Length: 1000000` and no credential; sixteen of those would pin 16 MB without ever proving who they are. The test `AWrongTokenNeverReachesTheActionAndANeverEndingBodyIsNeverWaitedFor` sends a wrong token with a gigantic length and nothing else, and expects a `401` in a few milliseconds, not a `413`, not a wait for the body.

## 2. A secret is not a setting
Settings travel through config files, command lines, environment dumps and the C ABI's string keys. A secret there ends up in `ps`, in git, in logs. So there is no `publish_token` key: the daemon refuses the word, reads a **file** (mode 0600, owned by the service user), and hands the bytes to the embedded config through a dedicated method that checks the length (at least 32 bytes: a short token is guessable). 1.1 went further (ADR-0039): length does not make a token strong, because `MyCompanyWebsocketToken2026!!!!!` is long and guessable, so the daemon accepts only a token that `wsrvd --generate-token` made, on every address including loopback. It was a breaking change in a minor release, chosen on purpose: the people a warning would not reach are the ones at risk (lesson 52 section 7 has the reasoning). The embedded server's `start()` refuses a publish port without the broker or without a token, so "on but unprotected" cannot happen.

## 3. Every bound has an attack it stops
| Bound | Stops |
|---|---|
| head at most 4 KiB | memory from a never-ending head |
| one 5 s deadline for head and body | slowloris, a body that stops short |
| at most 16 clients (more are closed) | descriptors and memory from many slow clients |
| body at most `broker_max_payload_bytes` | a message the broker would refuse anyway |
| one request per connection; bytes after the body are a 400 | request smuggling by pipelining |
| one non-blocking send for the answer | a client that never reads |

## 4. What `429` means
The broker never returns "queue full" as an error; it returns a report. When every one of the subscribers refused (their loops' queues were full), nothing was delivered, and the honest answer is "try again later": `429`. A partial refusal is `200` with the counts, because most of the room got it.

## 5. Count what you claim
`wsrv_publish_http_total{status}` has one series per status the endpoint can send. A test publishes, fails twice, and reads the page: the counters are checked, not assumed.

## Exercises
1. Which test would fail if the token comparison were a plain `==`? (Hint: none of ours measures time; what would you add?)
2. Why are bytes after the body refused instead of read as the next request?
3. Add a per-token rate limit: which of the bounds above would it join, and where would the counter live?

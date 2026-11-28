# Lesson 54 — A credential for one page, and one answer for every "no"

## Goal
After this lesson you can explain why the admin listener's token protects `/metrics` and not `/healthz`, why a server should give one identical refusal for every kind of bad credential, why two endpoints must share one definition of "the credential of a request", how a constant-time comparison differs from `==`, and why an optional security feature is only as good as its documentation.

## The problem
The admin listener (ADR-0023) serves three pages over plain HTTP: `/metrics`, `/healthz`, `/readyz`. Nothing authenticated it; the control was the network (loopback, a firewall, a NetworkPolicy). That is thin exactly where the daemon is usually run: to let Prometheus in another container scrape it, an operator binds the port to `0.0.0.0`, and from then on anything that can open a connection reads connection counts, message and byte rates and how many clients each limit refused. The labels are a closed set with no peer data, so this is aggregate information, not user data, but it is information the operator did not decide to give away. A review asked for a credential.

## 1. The constraint that shapes the design
A Kubernetes readiness probe is an `httpGet`; the kubelet cannot put a Secret into a header. `wsrvd --healthcheck` (the image's `HEALTHCHECK` and the liveness probe) sends `GET /healthz` with no header. Both pages answer one word. A token on all three pages would break the probes and protect two pages that reveal nothing. So the token guards **`/metrics` only**, and the ADR says why in its context, before the decision, because a reader who sees an unprotected page will ask.

A second decision is hidden in the word *optional*. The owner chose that the token is optional: with no key set nothing changes, including the `admin_exposed` warning. That is honest only if the documentation says so (OPERATIONS 3.1: "nothing changes unless you set it") and if the warning keeps being printed for the operator who has not set it. An optional control that is silent about being off is worse than none.

## 2. One answer for every no
A request can fail the credential check in five ways: no header, the wrong scheme, a malformed value, a repeated header, a wrong token. The server answers all five with the same bytes:

```
HTTP/1.1 401 Unauthorized
WWW-Authenticate: Bearer
Connection: close

credential required
```

Why: every difference between answers is information. If "wrong scheme" and "wrong token" look different, a peer learns which rule it broke and can probe the rules one at a time. The tests do not check the text of each refusal; they compare **the whole byte string** of a refusal for a missing header with that of a wrong token and require them to be equal.

Routing is judged first and is not hidden: `POST /metrics` is `405` and `GET /nothing` is `404` with or without a credential. The credential only decides a request that would otherwise have been served.

## 3. Comparing without telling
`constant_time_equal` does not stop at the first different byte. `std::string::operator==` returns as soon as it finds a difference, so the time it takes says how many leading bytes were right. Over a network the difference is tiny, but it is the textbook way to recover a secret one byte at a time, and a correct comparison costs nothing here. The comparison runs **before** `metrics_text()` is called: a peer without the credential cannot make loop 0 render the page.

A generated token has 256 bits of randomness (`wsrv1_` + 64 hex digits + `_` + a CRC-32 that catches typos, not attackers). That is why there is no lockout and no rate limit on wrong guesses: guessing is not the threat, and a lockout would give an attacker a way to lock your monitoring out.

## 4. One definition of "the credential"
The publish endpoint already read `Authorization: Bearer`. Copying that code into the admin listener would have made two definitions that can drift (one accepts two spaces, the other does not; one accepts a repeated header, the other refuses). So the function moved to `http/bearer.hpp` and both endpoints call it:

```cpp
std::optional<std::string_view> bearer_credential(const HttpRequest& head);
```

Exactly one `Authorization` header (two are ambiguous even when equal: which does a proxy forward, which does the server judge?), the scheme `Bearer` in any case (RFC 9110 section 11.1), exactly one space, a credential with no space and no control character. The move came **first, as its own slice, with the publish tests as the regression guard**: a refactor is only safe when something that already passed keeps passing. One mutation (accept a repeated header) was run against those tests to prove the guard bites.

The admin listener read no headers at all before this. With a token set, and for `GET /metrics` only, the head goes through the project's strict `HttpRequestParser` (CRLF only, no obs-fold, no control characters) and then `bearer_credential`. A head the parser refuses is `400`, as for the publish endpoint. With no token the code path is byte-for-byte the old one, so the existing admin tests guard everyone who does not set the key.

## 5. The token must differ from the other two
A scraper holds the admin token. If it were also the publish token, the monitoring system could publish into rooms; if it were the IPC token, over TCP IPC it could speak as the application's worker. `wsrvd` compares the three at start and refuses a repeat with a message that names which two collide and what to run (`wsrvd --generate-token`), never printing a token. This check cannot live in a unit test of the HTTP layer: it needs the three files, so it is a start-up test.

## 6. What it does not do (and says so)
No TLS: the token travels in clear, so it keeps out whoever can *reach* the port, not whoever can read the traffic. No lockout. The eight-slot exhaustion of the admin listener is unchanged, because the request head is read before the credential is judged; eight slow clients can still hold every slot. Rotation is a restart. Each limit is written in the ADR, in SECURITY_DESIGN T21 and in OPERATIONS, because a security feature that over-claims is a trap.

## Tests worth reading
- `BearerCredential.*`: each rule is one way a looser reading would let a request through.
- `AdminHttpToken.*`: the HTTP layer with fake sources; `AdminServerToken.*`: a running server and real sockets (401 bytes equal for missing and wrong, 200 with the right one, probes open, the log lines, the token never in a log).
- `StartupTest.TheAdminTokenMustDifferFromThePublishAndTheIpcToken`: the three-token rule, with a positive case so the rule cannot pass by refusing everything.

## Pitfalls
- A control that protects a page the probes need will be switched off by the first operator whose pod does not become ready.
- Different refusals for different faults are a probing oracle.
- `==` on a secret leaks length of the common prefix; use the constant-time compare.
- Copy-pasted parsers drift. Share the function and give it its own tests.
- A trailing space in a header value is not part of the value (the parser strips optional whitespace, RFC 9110 section 5.5): a test that expected a refusal for it was wrong, not the code. When a test and the RFC disagree, read the RFC.
- "Optional" must not mean "unannounced": keep the warning for the operator who has not set the key.

## Exercises
1. Why does `bearer_credential` refuse two `Authorization` headers even when both are the same token?
2. A peer sends `POST /metrics` with the right token, and another sends `GET /metrics HTTP/2.0` with none. Which answers do they get (`admin_http.cpp`, `admin_response`), and why does the credential decide neither?
3. The kubelet's readiness probe hits `/readyz` every 5 seconds without a header. Show how a token on `/readyz` would fail a rolling update.
4. Eight clients connect to the admin port, each sends one byte of a request head every 4 seconds. What does a scraper with the correct token see, and why does the token not help? What does help?
5. Write the test that proves a token equal to the publish token stops the start-up, and the test that proves three different tokens start fine. Why do you need both?
6. Prometheus can read the token from `credentials_file`. Why is that better than `credentials` in the scrape config?

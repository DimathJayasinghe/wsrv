# Lesson 26 — Choosing a certificate by name (SNI)

## Goal
After this lesson you can explain why one server needs several certificates, how the client tells the server which one it wants *before* encryption starts, how to treat that name as hostile input, and how wildcards are matched.

## 1. The problem
One IP address and port may serve `app.example.com` and `api.example.org`. The certificate must name the host the browser asked for, or the browser refuses it. But the server has to send the certificate in its *first* reply, before any HTTP `Host` header exists (that is inside the encrypted part). The fix is in the TLS hello itself: the **Server Name Indication** extension (RFC 6066 section 3) carries the host name in clear text, so the server can pick the certificate.

## 2. How OpenSSL lets us pick
We build one OpenSSL context per certificate (all with the same hardened settings, lessons 22–23) and register a callback on the default context. While OpenSSL reads the ClientHello it calls us; we look at the name and call `SSL_set_SSL_CTX(ssl, other)`, which makes this one connection use another context's certificate. Every outcome is "carry on": a client with no name, an address, or an unknown name is not refused; it gets the default certificate. (Refusing would break old clients and leak which names exist.)

## 3. The name is attacker input
Anyone can send any bytes as the "host name". So before comparing:
- only letters, digits, `-`, `_` and dots between non-empty labels (a space, a control byte, `*` or a non-ASCII byte makes the name unusable);
- labels at most 63 and the whole at most 253 characters (DNS limits);
- no trailing dot (RFC 6066 forbids it);
- lower-case it (DNS ignores case);
- never write it to a log (log injection).
This is a pure function (`tls/sni.cpp`) with its own table-style tests, apart from OpenSSL. The mutation check that removed it made the "hostile name" test fail: `bad name.example.com` would otherwise match `*.example.com`, because a space looks like part of "one label".

## 4. Wildcards
`*.example.com` covers **exactly one label** in front of `.example.com`: `a.example.com` yes; `example.com` no; `a.b.example.com` no (RFC 6125 section 6.4.3). We refuse `*.com` (a wildcard over a whole top-level domain is almost certainly a mistake). An exact entry beats a wildcard, whatever order they are listed in; between two wildcards the longer suffix is more specific.

## 5. Limits and failing closed
Each entry costs a whole OpenSSL context, so there are at most 64, checked in the config (so a host cannot grow it without limit) and again in the context. Every entry file is checked like the default (valid now, key matches, key not readable by everyone). One bad entry makes the whole load or reload fail and the old setup stays: a half-applied reload (new default, old entries) is worse than no reload.

## Pitfalls
- A wildcard rule written as `host.ends_with(suffix)` alone also matches `evil-example.com` unless the dot is part of the suffix and exactly one label precedes it.
- A wildcard pattern like `*.localhost` has one label after the star and is refused; our first fuzzing harness used it and the harness itself failed. The refusal was right; the harness was wrong.
- A fuzzer given a single corpus directory *adds new inputs to it*: pass a scratch directory first, the committed one second.

## Exercises
1. Add a test that `tls_sni=*.example.com=...` and `tls_sni=a.example.com=...` give `a.example.com` the exact entry regardless of order in the file.
2. Why is it safe that `normalise_host` lower-cases only ASCII letters? What would go wrong with a Unicode-aware lower-casing (hint: "İ")?
3. SNI is sent in clear text. What does that mean for privacy, and what is Encrypted Client Hello a solution to? (Look up the IETF Encrypted Client Hello specification.)

# Lesson 06 — The WebSocket handshake: a chain of small rules

## Goal
See how a parsed HTTP request becomes an accept or a precise rejection, why the design is a *chain of independent rules*, and why the Origin check is the one defence against a whole attack class. Code: `src/protocol/handshake*.{hpp,cpp}`, `extension_offer.*`.

## 1. The protocol in one picture
The client asks to switch protocols; the server proves it understands WebSocket:

```text
GET /chat HTTP/1.1                         HTTP/1.1 101 Switching Protocols
Host: example.com                          Upgrade: websocket
Upgrade: websocket                         Connection: Upgrade
Connection: Upgrade                        Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==
Sec-WebSocket-Version: 13
Origin: https://app.example.com
```

`Sec-WebSocket-Accept = Base64(SHA-1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))` — the SHA-1 and Base64 from Lesson 03. It proves the server is not an ordinary HTTP server echoing headers; it is *not* authentication.

## 2. The design: Chain of Responsibility
Each requirement is a small function wrapped in an object that implements `IHandshakeRule`:

```cpp
class IHandshakeRule {
    virtual std::optional<HandshakeReject> evaluate(const HttpRequest&, HandshakeContext&) const = 0;
};   // nullopt = "fine, ask the next rule"; a value = "stop, answer with this"
```

`HandshakeNegotiator` runs them in a fixed order and returns the first refusal:

| # | Rule | Refusal |
|---|---|---|
| 1 | method is `GET` | `405` + `Allow: GET` |
| 2 | HTTP/1.1 | `400` |
| 3 | exactly one valid `Host` (`name`, `a.b.c.d` or `[ipv6]`, optional `:port`) | `400` |
| 4 | no body (`Content-Length` absent or `0`, no `Transfer-Encoding`) | `400` |
| 5 | `Upgrade` contains token `websocket` | `400` |
| 6 | `Connection` contains token `upgrade` | `400` |
| 7 | exactly one `Sec-WebSocket-Key` = Base64 of 16 bytes | `400` |
| 8 | exactly one `Sec-WebSocket-Version` = `13` | `400` / `426` + `Sec-WebSocket-Version: 13` + `Upgrade: websocket` (RFC 9110 requires an `Upgrade` header on a 426) |
| 9 | Origin policy | `403` / `400` |
| 10 | subprotocol selection | `400` |
| 11 | extension-offer syntax | `400` |
| 12 | your own rules | whatever they say; a throw becomes `500` |

Why a chain? **Single responsibility** (each rule has one reason to change), **open/closed** (adding authentication means writing a new class, with no edits to existing rules), and **testability** (the 30 handshake tests poke each rule through the negotiator). The order is deliberate and tested: a `POST` with a missing key gets `405`, and your authentication rule never sees a request that failed the Host rule, so it cannot be tricked into spending a database query on garbage.

A conflict we resolved while writing the tests: our own edge-case table said both "duplicate `Sec-WebSocket-Version` header → 400" and "multiple versions → 426". We decided: **duplicate header lines** are ambiguous (`400`); **one header listing several values** (`13, 8`) is a version mismatch (`426`).

## 3. The Origin check (cross-site WebSocket hijacking)
Imagine you are logged in to `bank.example` in one tab. A page on `evil.example` runs `new WebSocket("wss://bank.example/socket")`. The browser attaches your bank cookies automatically, so the bank's server sees an authenticated request. Unlike normal cross-origin fetches, WebSocket connections are **not** blocked by the browser's same-origin policy. The only defence is that browsers always send an `Origin` header naming the page that started the connection, and the server **must check it**.

Our policy is secure by default:
- Origin present, and not on the allow-list → `403`. With *no* allow-list configured, every Origin is refused.
- Origin missing → `403` (browsers always send one; a missing one means a non-browser client) unless `allow_missing_origin`.
- `Origin: null` (sandboxed pages, local files) is just a string that is not on the list.
- Several `Origin` headers → `400` (ambiguous).
- `allow_any_origin` exists but is deliberately loud: it disables the protection.
- Comparison is exact but ASCII case-insensitive (`HTTPS://APP.EXAMPLE.COM` matches).

Note this protects browsers' users; a malicious *non-browser* program can send any Origin it likes, which is why real authentication is a separate rule.

## 4. Subprotocols and extensions
- `Sec-WebSocket-Protocol` lists application protocols the client can speak. We pick by **server preference**: the first of *our* list that the client offered. Offers must be tokens (no quotes, no `;`), and a repeated name is rejected. If nothing matches the header is simply omitted (or `400` with `require_subprotocol`). We only ever echo a value from our own configured list, never raw client text.
- `Sec-WebSocket-Extensions` offers things like compression. M2 only checks the *syntax* with a small quote-aware parser (`extension_offer.cpp`): tokens, `;`-separated parameters, quoted strings, no duplicate parameter names, no whitespace around `=`. Valid offers are collected for M9 (permessage-deflate) and none are accepted yet; the 101 never mentions them.

## 5. Your own rules: a trust boundary
`HandshakeConfig::user_rules` run after all built-in rules, in order. Application code is *untrusted input to the server's output*, and a first version of ours trusted it too much. The independent review of this milestone demonstrated that a user rule could (a) return status 999 so that nothing could be sent, (b) return `101` or `200` as a "refusal", (c) add its own `Content-Length` next to ours (ambiguous framing), or (d) overwrite the negotiated subprotocol with a value the client never offered. We fixed every one, each with a test that failed first and a mutation check showing the test fails if the guard is removed:

- A rule's **refusal is validated**: status 400-599, token header names, clean values, and none of the headers the server owns (`Connection`, `Content-Length`, `Transfer-Encoding`, `Upgrade`, `Sec-WebSocket-*`). Anything else becomes a `500`.
- A rule may **only add annotations** (`context.annotations`, e.g. the user id and claims, handed to the application with the accepted connection and never sent to the peer). If it changes the negotiated subprotocol or extension offers, the result is a `500`.
- Every call is inside an **exception barrier**: a rule that throws *anything* (a `std::exception`, your own type, even an `int`) produces a `500` with `from_exception` set so the connection layer can log it (FR-S6).
- A **bad configuration** (a null rule, a subprotocol that is not a token or is listed twice, an allowed origin such as `*` or `https://a.com/` that could never match) throws `std::invalid_argument` when the negotiator is constructed: config errors are startup errors, found at startup instead of as mysterious 403s in production.
- Rules are `const` and shared between connections, so they must be thread-safe.

## 6. Responses
`to_response` produces the exact `101` text (verified byte for byte against the RFC sample), or the refusal plus `Connection: close` and `Content-Length: 0`. `render_response` always yields bytes to send: if a response ever could not be serialised it returns a fixed, well-formed `500` instead of nothing. The handshake fuzz target now includes a deliberately hostile user rule that builds its refusal (status, header name, header value, including CR/LF/NUL) from the raw fuzz input, and asserts the response always has exactly `headers + 2` CRLFs and never echoes extension offers or annotations. We proved it: removing the refusal validation makes the fuzzer fail within seconds, and that input is kept in the corpus as a regression test.

Two more review-driven limits: subprotocol offers are capped at 32, and extension offers/parameters at 16 each, because duplicate detection is linear and an uncapped 8 KiB header measured about 6 ms of CPU per request (about 6,000x a normal handshake) on the loop thread.

## 7. Exercises
1. The allow-list is `["https://app.example.com"]`. Which of these get `101`: `https://APP.example.com`, `https://app.example.com:443`, `null`, no Origin header?
2. The request has `Sec-WebSocket-Version: 13, 8`. Which status, and which header comes back?
3. The server prefers `chat, superchat`; the client offers `superchat, chat`. Which one is chosen?
4. Where in the chain would you put a rule that checks an API token, and why there?

<details><summary>Answers</summary>

1. Only `https://APP.example.com` (case-insensitive match). The explicit port is a different origin string, `null` is not listed, and a missing Origin is refused by default.
2. `426 Upgrade Required` with `Sec-WebSocket-Version: 13`.
3. `chat`: the server's preference decides.
4. As a user rule (it runs last): by then the request is known to be a well-formed upgrade with an allowed Origin, so the (possibly expensive) check is not spent on malformed or cross-site requests.
</details>

## 8. What's next
M3: the frame codec. The connection is now upgraded, and what follows is binary: headers, masking, lengths, and the same "strict, incremental, bounded" discipline.

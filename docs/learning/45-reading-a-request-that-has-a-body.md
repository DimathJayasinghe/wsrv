# Lesson 45 — Reading a request that has a body, without being smuggled

## Goal
After this lesson you can say why a request with a body needs rules a WebSocket handshake never did, list the framing ambiguities that become request-smuggling bugs, and explain why the interpreter never compares the token and never sizes a buffer.

## The problem
A handshake is a head and nothing else. `POST /publish` has a body, and the receiver must know exactly where the body ends. HTTP offers two ways (`Content-Length` and `Transfer-Encoding`), and when two programs in a chain disagree about which one counts, an attacker hides a second request inside the first. That is request smuggling (RFC 9112 section 6.3). Our daemon will often sit behind a proxy, so it must be unambiguous, not clever.

## 1. Reuse what is strict, add what is new
`HttpRequestParser` already refuses bare LF, obs-fold, control characters and bad escapes, and it is already fuzzed. `interpret_publish` takes its parsed head and adds only the body rules. No second parser means no second set of bugs.

## 2. The rules, and the attack each one stops
- **Any `Transfer-Encoding` is 501**, even with a `Content-Length`. If we picked a winner, a proxy that picks the other one would disagree with us.
- **A repeated `Content-Length` is 400**, even with equal values. "Equal" is a rule one hop knows and the next may not.
- **`Content-Length` is canonical decimal**: digits only, no sign, no `0x`, no leading zero, 19 digits at most, overflow checked per digit. `atoi` would accept `+5`, ` 5` and wrap on huge values.
- **`Expect` is 417**: we never send `100 Continue`, so promising to would stall a client.
- **No percent-decoding**: the topic is the bytes that were sent. `a%2Fb` is refused instead of being decoded into something the broker never saw.

## 3. What the interpreter does not do
It does not compare the token and it does not allocate from the declared length. The endpoint (next slice) compares the token, then applies the size bound, then reserves memory, in that order. If the length check came first, anyone could make the daemon reserve memory by sending a big `Content-Length` and no credential.

## 4. The fuzzer earned its keep
`fuzz_publish_request` asserts what an accepted request must look like (one canonical length, no framing headers, a topic without separators). Within seconds it accepted `topic=?` and `topic=a=b`: the interpreter let a second separator into a value. The fix was in the interpreter, never in the assertion, and the crasher stays in the corpus as a regression seed.

## Exercises
1. Which of the refusals are 4xx and which 5xx, and why is `Transfer-Encoding` a 5xx?
2. Write the request that makes a proxy see one request and this daemon see two, if `Content-Length` and `Transfer-Encoding` were both accepted.
3. Why is a leading zero in `Content-Length` refused even though `05` means 5?

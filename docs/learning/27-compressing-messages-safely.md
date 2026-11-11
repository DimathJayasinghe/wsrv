# Lesson 27 — Compressing messages safely (permessage-deflate, M9)

## Goal
After this lesson you can explain what `permessage-deflate` changes on the wire, why a server that decompresses hostile input needs three separate limits, why a compressor with a memory (context takeover) turns a failed send into a corrupted connection, and why the feature is switched on in exactly one place and fails closed everywhere else.

## 1. The problem
JSON-heavy traffic shrinks five- to tenfold, and every browser offers `permessage-deflate` (RFC 7692). But the other side of the bargain is dangerous: a few kilobytes of DEFLATE can expand to gigabytes (a *decompression bomb*), and a compressor costs tens of kilobytes of memory *per connection*. So compression is off by default, and everything that touches it is bounded.

## 2. What changes on the wire (slice 1: negotiation)
The client offers it in `Sec-WebSocket-Extensions`; the server answers with the parameters it accepts. Four parameters matter: `server_no_context_takeover` / `client_no_context_takeover` (does the compressor keep its memory between messages?) and `server_max_window_bits` / `client_max_window_bits` (how far back may a match reach, 2^bits bytes). The choice is a **pure function** in `protocol/` (`accept_offer`): offers in, agreed parameters out, no sockets. It tries the offers in order and takes the first acceptable one. Unknown, malformed or contradictory parameters make that offer unacceptable; if none fits, no compression is used and the connection carries on. One quirk lives below: zlib cannot compress with a window of 8 (it silently uses 9), so the server never agrees to 8 but accepts a *client* window of 8, because our inflater always decodes with 15 bits.

A compressed message sets **RSV1 on its first frame only**; the message is compressed whole and *then* fragmented. RSV1 on a continuation or a control frame is a protocol error (1002).

## 3. Inflating hostile data (slices 2 and 3)
The inflater sits between the frame parser and the message assembler, so the assembler's existing limits see the **inflated** bytes. Three layers, because each stops a different attack:
1. **Message size** on the inflated bytes: close 1009, mid-stream, before more output is produced.
2. **Ratio**: inflated bytes may not exceed `max_inflate_ratio` times the compressed bytes *received* (plus a 4 KiB allowance so tiny messages are not refused). Count what the peer **sent**, not what zlib has consumed so far: the second is order-dependent (a long run of zeros consumes almost no input) and refused an honest message in review.
3. **Piece size**: output comes out in pieces of at most 16 KiB into a buffer *we* own, so peak memory is decided by us, never by the peer. No allocation is sized from peer data.
The codecs themselves are behind two interfaces (`IMessageInflater`, `IMessageDeflater`), implemented once in `extensions/` over system zlib; `protocol/` never names zlib (the include-graph checker enforces it). Anything behind an interface is untrusted too, so the session validates what an inflater reports (lesson 28).

## 4. The compressor with a memory (slice 3: sending)
With context takeover, the sender's compressor remembers earlier messages and the peer's inflater must see **every** message the compressor saw. If we compressed a message and then could not queue it (the queue was full), the peer's history would be one message behind and *every later message would decode to garbage*. The rule that prevents it: **check that the queue has room for the worst case before the compressor sees the message**, and if only the worst case does not fit, send the message plain (always allowed, EC-D07): the compressor never saw it, so the histories still agree.

## 5. Switching it on, in one place (slice 4)
- The handshake and the session are two layers that must agree: RSV1 is accepted only if *this connection's* handshake agreed. `session_config_for` copies the server's session settings, puts in the agreed parameters, and otherwise removes deflate, so a client that did not negotiate and sends RSV1 gets 1002.
- `ServerBuilder::permessage_deflate(options)` is the composition root: it builds the zlib factory and sets the handshake policy and the session settings **together**, so the half-configured state (offer compression but cannot do it) cannot exist. `validate()` also checks the pairing for anyone who bypasses the builder.
- **Fail closed:** a build without zlib (`WSRV_ENABLE_DEFLATE=OFF`) refuses to start when compression is requested, in the library and in the C embedded server, the same pattern as TLS (lesson 25). The C/`wsrvd` keys (`permessage_deflate`, `deflate_level`, ...) only parse numbers; the ranges are checked once, in `build()`.

## Tests
Negotiation table (every parameter permutation), codec round trips for every window and both takeover modes, bombs at the size and ratio limits, a byte-at-a-time input equal to one-shot, the desync pitfall (a refused send never reaches the compressor), end-to-end tests on a real loopback socket with real zlib, the no-zlib builds, `fuzz_inflate`, and Autobahn sections 12 and 13.

## Pitfalls
- Counting the ratio on consumed input (above), or on a frame's declared length: both are wrong bases.
- Allocating a buffer "big enough" for the inflated size announced by anything the peer controls.
- Letting a configuration flag open RSV1 by itself: the flag must follow the negotiated state.
- Keeping a compression scratch buffer per connection "for speed": 10 000 connections times 64 KiB.

## Exercises
1. Work out, with zconf.h's formula, the memory of one connection at window 15 / memLevel 8, and compare with `docs/OPERATIONS.md` section 1.1.
2. Send a compressed message split over three frames with a Ping between the fragments. Which frame carries RSV1, and why is the Ping not compressed?
3. Why is `deflate_max_ratio=1000` still a ceiling and not "off"? What would a ratio of 1,000,000 let a client do to one event-loop turn?

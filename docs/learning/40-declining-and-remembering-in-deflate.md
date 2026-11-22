# Lesson 40 — Declining an offer, remembering a window (permessage-deflate edge cases)

## Goal
After this lesson you can explain why a repeated extension parameter must decline one offer and not fail the handshake, why a message that ends with a BFINAL block must not make the receiver forget its window, and how a test proves a suspicion before a fix.

## The problem
Three loose ends from ADR-0020 A2 were open. Two were guesses about bugs, one was a missing test.

## 1. Declining is not failing
A client may send several extension offers in one header: `permessage-deflate; a, permessage-deflate; b`. RFC 7692 section 7 says a server MUST decline an offer that repeats a parameter name, and section 5 says declining every offer means "carry on without the extension". Our offer parser used to treat a repeated name as a syntax error, which failed the whole header with 400. That is stricter than the RFC and wrong when a second, good offer was waiting.

The fix moves the judgement. The parser keeps the repeated parameters in order (it still caps their number, so work stays bounded); the negotiation scans one offer for repeats and skips that offer. Syntax errors (a dangling `;`, an open quote) still fail the handshake, because there is no way to tell what the client meant.

## 2. Remembering a window
With context takeover both sides keep the last 32 KiB of data as a dictionary, so message 2 can point back into message 1 (section 7.2.2). zlib's `inflate()` stops at a block marked BFINAL (final) and reports `Z_STREAM_END`; our code then called `inflateReset()`, which throws the dictionary away. Section 7.2.3.4 of the RFC itself shows a message that ends this way.

We did not call it a bug first. The test comes first: message 1 is the RFC's own bytes `f3 48 cd c9 c9 07 00 00`, message 2 is a stream made with Python's `zlib.compressobj(..., zdict=b"Hello")`, which only decodes if the receiver still has "Hello" in its window. Both failed: the RFC's own example was rejected ("data after the final block"), and the second message failed with invalid distance. The fix saves the window with `inflateGetDictionary`, resets, and puts it back with `inflateSetDictionary` (zlib 1.2.9 and later). With `no_context_takeover` the reset stays complete, and a test checks that too.

## 3. A test that can fail
`TheSendScratchBufferIsReleasedAfterEverySend` asserts that an idle connection holds no compression memory (the buffer's capacity is 0 after each send). A test that cannot fail proves nothing, so we removed the release line, saw the test go red, and put the line back.

## Pitfalls
- Reading "MUST decline" as "MUST reject the connection".
- Fixing a suspected bug without a failing test: you cannot tell afterwards whether the fix did anything.
- `inflateReset` keeps the allocation but forgets the dictionary; the two are separate.

## Exercises
1. Send `permessage-deflate; client_max_window_bits, permessage-deflate; server_max_window_bits=10; server_max_window_bits=11` to the server. Which offer wins, and what is the response?
2. Why does `restart_keeping_window` allocate a 32 KiB copy, and when is it freed? Could it use the stream's own buffer?
3. Build the second test vector yourself with Python and check it fails on a receiver that reset the window.

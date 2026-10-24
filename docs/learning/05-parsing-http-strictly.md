# Lesson 05 — Parsing HTTP strictly (the handshake request)

## Goal
Understand why the first code any stranger's bytes touch must be *boring and strict*, how a byte-at-a-time state machine makes the result independent of how TCP chops the data, and how limits turn an open-ended attack into a bounded cost. Code: `src/http/` (`token`, `request`, `request_parser`, `response`).

## 1. What arrives
A WebSocket starts as an ordinary HTTP/1.1 request. The server must read the *head* (request line + headers + a blank line), decide, and answer. The head is text, delimited by `\r\n`:

```text
GET /chat HTTP/1.1\r\n
Host: example.com\r\n
Upgrade: websocket\r\n
...
\r\n
```

Anything after the blank line is *not HTTP*: for a WebSocket the client may already have sent its first frame bytes in the same TCP segment (EC-H19). The parser therefore stops **exactly** after the final `\n` and reports how many bytes it used; the rest belongs to the next stage. Our flow test feeds the head in three chunks with two frame bytes glued to the end, and checks that exactly those two are left over.

## 2. Why strictness is a security feature
Most HTTP vulnerabilities come from two components *disagreeing* about where a request or header ends:

- **Request smuggling.** A front proxy honours `Content-Length`, the back end honours `Transfer-Encoding`; an attacker sends both and hides a second request inside the first. Our rule: an upgrade request has no body, so any `Transfer-Encoding`, a non-zero or repeated `Content-Length` is a `400`.
- **Response/header splitting.** If a value containing `\r\n` is copied into a response, the attacker writes their own headers. The response serialiser rejects control characters in header values.
- **Parser differentials.** One parser accepts a bare `\n` as a line end, another does not; one folds a header continued on the next line (`obs-fold`), another treats it as a new header. Whoever is stricter than the other loses.

So the parser accepts only unambiguous input and answers `400` to the rest: CRLF only (bare CR or LF rejected, even when split across two TCP reads); NUL and other control characters rejected; no folded headers; header names must be RFC *tokens* (so `Host : x`, with a space before the colon, is out); the request target must be `/...` (no absolute URIs, `*`, authority form, fragments, raw spaces, raw non-ASCII, or bad `%XX` escapes); and the version must literally be `HTTP/<digit>.<digit>` (`HTTP/2.0` is `505`, `http/1.1` is `400`).

### Case-insensitive, but only ASCII
Header names are case-insensitive. The tempting `tolower` is **locale-dependent**. We generated a Turkish locale in the dev container and measured it:

```text
C locale          : tolower(0x49 'I') = 0x69
tr_TR.ISO-8859-9  : tolower(0x49 'I') = 0xfd   (0x69 would be 'i')
```

In that locale `UPGRADE` would no longer equal `upgrade`. `iequals` folds only `A–Z`, never looks at the locale, and never folds bytes ≥ 0x80. Tests include `'@'` vs `` '`' `` (they differ by exactly 0x20 but are not letters, a classic bug in naive "OR with 0x20" folding).

## 3. The parser: one byte at a time
`HttpRequestParser::consume_byte` looks at a single byte and updates a tiny amount of state:

```text
state = { line_ (current line so far), pending_cr_, in_request_line_, total_bytes_, complete_ }
byte == CR            -> remember it (a second CR, or anything but LF next, is an error)
byte == LF            -> must follow a CR; the line is finished: parse it
anything else         -> append to the line (after checking limits and forbidden bytes)
empty line in headers -> the head is complete
```

Because the parser only ever sees one byte and remembers the rest, **the result cannot depend on chunking**. The tests prove it: every possible split point of a valid request, byte-at-a-time delivery, and each of 36 malformed inputs delivered in 1-, 2-, 3- and 7-byte chunks must produce the same status.

## 4. Limits: bounded memory and bounded patience
| Limit | Default | Exceeded |
|---|---|---|
| request line | 4096 bytes | `414` |
| whole head | 8192 bytes | `431` |
| header count | 64 | `431` |

They are enforced *while* parsing: a test sends a 1 MiB header and checks the parser gives up within one 1000-byte chunk of the 8192 limit instead of reading it all. Limits are tested at the exact boundary (4096 accepted, 4097 rejected). A byte-count limit alone doesn't stop *slow* attacks (a "slowloris" client trickling one byte every few seconds): that needs a timeout, which lives in the connection layer (M6) because the parser deliberately knows nothing about time.

## 5. Serialising answers safely
`serialize(HttpResponse)` builds `HTTP/1.1 <status> <reason>\r\n<headers>\r\n`. It validates what it is given (status 100–599, token header names, no control characters in values) and returns an error otherwise, which means a rule that accidentally copied attacker text into a header cannot split the response.

## 6. Proof, not hope
- 17 parser tests, 10 token/request-model tests and 8 response tests, written before the code.
- Fuzzing (`fuzz_http_request`) asserts: same outcome for whole / data-derived chunking / byte-at-a-time; only documented statuses; accepted heads obey the limits and contain no CR, LF or NUL in the target or any value. About 1 million executions in 30 seconds found nothing. To prove the harness can fail, we deleted the NUL-byte rejection from the parser and the fuzzer immediately reported `free_of_injection_characters(header.value)` with an input containing `\0` (the first attempt at this sabotage silently did nothing because the script's pattern did not match; we checked the diff before believing the result).

## 7. Pitfalls we hit
1. **Off-by-one in a test.** A literal `"GET / ... a\0b ...\r\n\r\n"` was constructed with length 30 but contained 29 characters, reading the C string's terminating NUL as data. clang-tidy's `bugprone-string-constructor` caught it. Strings with embedded NULs need an explicit length, and the length must be right.
2. **A regex replacement is a string with escapes.** Patching a test file with Python's `re.sub` turned `\n` inside the replacement into real newlines and broke the C++ string literals; the compiler's "missing terminating quote" error pointed straight at it. Use plain `str.replace` for code.
3. **Two limits, two statuses.** Mapping "too long" to the right code (`414` for the request line, `431` for headers) is part of being a good HTTP citizen and part of the tests.

## 8. Exercises
1. The bytes `GET / HTTP/1.1\r\nHost: x\r\n\r\n` are followed by `81 80`. How many bytes does the parser report as consumed?
2. What status do you get for: (a) a 4097-byte request line; (b) 65 headers; (c) `HTTP/2.0`; (d) `GET /a%zz HTTP/1.1`?
3. Why must a bare `\n` be rejected even though many clients send it?
4. Why can't the parser enforce "the client must finish its head within 5 seconds"?

<details><summary>Answers</summary>

1. 27 (16 + 9 + 2). The two frame bytes are left for the next stage.
2. (a) 414; (b) 431; (c) 505; (d) 400 (invalid percent-encoding).
3. A lenient server and a strict proxy in front of it would disagree about where the line ends: that disagreement is exactly what request-smuggling attacks exploit. Being strict removes the ambiguity.
4. It is sans-I/O: it has no clock. The connection layer owns timers (injected `IClock`) and closes slow connections; the parser only bounds *how much* it will accept.
</details>

## 9. What's next
Lesson 06 puts the parsed request through the handshake rules and computes the `Sec-WebSocket-Accept` answer.

# Protocol & Runtime Edge-Case Catalogue

This is a **test index**. Every row must map to at least one automated test (unit, integration, fuzz seed, or Autobahn case). Test names cite the row ID (e.g. `// EC-F07`). A row without a test is a defect in the catalogue.

Column "Expected" is the required server behaviour. Close codes per RFC 6455 §7.4.

## 1. Handshake (EC-H)

| ID | Case | Expected |
|---|---|---|
| EC-H01 | Method not `GET` | 405 + `Allow: GET`, close |
| EC-H02 | HTTP/1.0 or version < 1.1 | 400 / 505, close |
| EC-H03 | Missing `Host` | 400 |
| EC-H04 | Duplicate `Host` / `Sec-WebSocket-Key` / `Sec-WebSocket-Version` | 400 |
| EC-H05 | `Upgrade` missing or not `websocket` (any case: `WebSocket`, `WEBSOCKET` OK) | 400 (426 if otherwise valid upgrade to other protocol) |
| EC-H06 | `Connection` lacks `upgrade` token; token list (`keep-alive, Upgrade`) | Case-insensitive token match; list accepted |
| EC-H07 | `Sec-WebSocket-Key` missing / invalid base64 / decoded ≠ 16 bytes | 400 |
| EC-H08 | `Sec-WebSocket-Version` ≠ 13 (or multiple values) | 426 + `Sec-WebSocket-Version: 13` |
| EC-H09 | Accept computation known vector (`dGhlIHNhbXBsZSBub25jZQ==` → `s3pPLMBiTxaQ9kYGzzhZRbK+xOo=`) | Exact match |
| EC-H10 | Origin present, not allowed | 403 |
| EC-H11 | Origin absent (non-browser client) | Per policy (default allow only if `allow_missing_origin`) |
| EC-H12 | Subprotocol: none requested / none match / multiple offered / duplicate tokens / invalid token chars | Select first server-preferred match; otherwise omit header (or 400 per strict policy); never echo invalid token |
| EC-H13 | Extension offer unknown / malformed / duplicate params | Ignore unknown; reject malformed with 400; never 101 with partially-parsed extension |
| EC-H14 | Request line > limit; header line > limit; total > 8 KiB; > 64 headers | 431 / 414, close |
| EC-H15 | Bare LF, bare CR, NUL byte, obs-fold, space before colon, empty header name, non-ASCII in header name | 400 |
| EC-H16 | Request has body (`Content-Length>0`) or `Transfer-Encoding` | 400 |
| EC-H17 | Request arrives 1 byte at a time | Works; timeout still enforced |
| EC-H18 | Request split inside `\r\n\r\n` | Works |
| EC-H19 | Frame bytes pipelined in same TCP segment after the request | Delivered to frame parser after 101 is queued |
| EC-H20 | Handshake never completes | Closed at handshake timeout (no response needed) |
| EC-H21 | Client disconnects before/while 101 is written | No crash; resources freed; no `on_open` |
| EC-H22 | Non-HTTP garbage / TLS ClientHello on plain port / empty connection | Close, counted in metrics, no log spam (rate-limited log) |
| EC-H23 | Absolute-URI or `*` request target; fragments; invalid percent-encoding in path | Reject 400 (path routing only on origin-form) |
| EC-H24 | Very long idle keep-alive after 101 failure | Close |
| EC-H25 | Auth hook rejects / throws / times out | 401/403/500 mapped; exception contained |
| EC-H26 | `/healthz` request without Upgrade (if enabled) | 200, then close |

## 2. Framing (EC-F)

| ID | Case | Expected |
|---|---|---|
| EC-F01 | Any of RSV1–3 set without negotiated extension | Close 1002 |
| EC-F02 | Reserved opcodes 0x3–0x7, 0xB–0xF | Close 1002 |
| EC-F03 | Unmasked frame from client | Close 1002 |
| EC-F04 | Payload length 126 encoded form for value < 126, or 127 form for value < 65536 (non-minimal) | Close 1002 |
| EC-F05 | 64-bit length with MSB set | Close 1002 |
| EC-F06 | Length > `max_frame_payload` (declared before payload arrives) | Close 1009 immediately; do not buffer |
| EC-F07 | Control frame payload > 125 | Close 1002 |
| EC-F08 | Fragmented control frame (FIN=0) | Close 1002 |
| EC-F09 | Continuation frame with no message in progress | Close 1002 |
| EC-F10 | New text/binary frame while fragmented message in progress | Close 1002 |
| EC-F11 | Control frames interleaved between fragments | Handled; data message intact |
| EC-F12 | Zero-length text/binary/continuation/ping/pong frames | Valid; no state corruption; rate counted |
| EC-F13 | Header split across reads at every byte offset (property test) | Same result as unsplit |
| EC-F14 | Payload split across reads at every offset | Same result |
| EC-F15 | Multiple frames in one read | All processed in order, within fairness budget |
| EC-F16 | Mask key all zeros / repeated / unaligned start offset | Correct unmasking |
| EC-F17 | Masking across chunk boundaries (mask index continuity) | Correct |
| EC-F18 | 1 MiB+ single frame, 16 MiB message within configured limits | Works, bounded memory growth |
| EC-F19 | Message > `max_message_size` via many small fragments | Close 1009 at the exceeding fragment |
| EC-F20 | Fragment count > `max_fragments` | Close 1008 |
| EC-F21 | Length arithmetic overflow (`offset + len`, `total + len`) | Detected via checked math; close 1009/1002 |
| EC-F22 | Declared length huge but connection sends nothing | No pre-allocation; idle/timeout applies |
| EC-F23 | Declared header says N bytes, peer closes TCP mid-frame | Clean teardown, `on_close` code 1006 |
| EC-F24 | Extension bits (RSV1) valid with permessage-deflate on control frames | Close 1002 (RSV1 not allowed on control/continuation) |
| EC-F25 | Ping/Pong arrives between fragments while a UTF-8 sequence is split | Control frame handled; UTF-8 validator state preserved; message completes |
| EC-F26 | Close frame received mid-fragmented message | Close handshake starts; partial message discarded, never delivered |
| EC-F27 | Fragmented message started, final fragment never sent | Close 1008 at `incomplete_message_timeout`; buffers released |
| EC-F28 | Flood of 1-byte fragments | Close 1008 at `max_fragments`; data-frame rate limit also applies |
| EC-F29 | Streaming mode: text part boundaries | Each part ends on a code-point boundary (incomplete tail held back) |
| EC-F30 | Streaming mode: handler closes session mid-message | Remaining frames of that message dropped; no further `on_message_part` |
| EC-F31 | Send: message > `max_outbound_frame` | Auto-fragmented; peer reassembles byte-identical (round-trip property) |
| EC-F32 | Send: Ping/Pong/Close queued while large fragmented message is being written | Control frame emitted at next frame boundary, before remaining fragments; never inside a frame |
| EC-F33 | Send: second data message while a `MessageWriter` is open | `Error{MessageInProgress}`; nothing written |
| EC-F34 | `MessageWriter` destroyed without `finish()` | Connection closed 1011; logged |
| EC-F35 | `MessageWriter::write` when outbound queue above high-water | `Backpressure`; resumes via `on_writable` below low-water |
| EC-F36 | Streaming text writer given invalid UTF-8 | `Error{InvalidUtf8}`; nothing invalid is emitted |
| EC-F37 | Zero-length final continuation frame (`FIN=1`, len 0) | Completes the message |

## 3. UTF-8 (EC-U)

| ID | Case | Expected |
|---|---|---|
| EC-U01 | Valid multi-byte sequences (2/3/4-byte), U+0000, U+10FFFF | Accepted |
| EC-U02 | Overlong encodings (e.g. C0 80, E0 80 80) | 1007 |
| EC-U03 | Surrogates U+D800–DFFF (ED A0 80…) | 1007 |
| EC-U04 | Code points > U+10FFFF (F4 90…, F5–FF) | 1007 |
| EC-U05 | Lone continuation / truncated sequence at end of message | 1007 |
| EC-U06 | Sequence split across fragments at every position | Accepted if valid; 1007 if invalid at *first invalid byte* (fail-fast, don't wait for FIN) |
| EC-U07 | Invalid UTF-8 in binary message | Accepted (no validation) |
| EC-U08 | Invalid UTF-8 in close reason | Close 1007 / 1002 per policy |
| EC-U09 | Differential test vs reference validator over all 1–4 byte strings and random fuzz | Identical verdicts |

## 4. Control frames & close (EC-C)

| ID | Case | Expected |
|---|---|---|
| EC-C01 | Ping → Pong with identical payload | Sent promptly, even mid-fragmented message |
| EC-C02 | Multiple pings queued before write | Pong for the latest may suffice (RFC allows); ordering of other frames preserved |
| EC-C03 | Unsolicited Pong | Ignored (optionally surfaced) |
| EC-C04 | Ping flood > rate limit | Close 1008 |
| EC-C05 | Close with empty payload | Echo Close (code 1005 semantics internally; send empty) |
| EC-C06 | Close with 1-byte payload | Close 1002 |
| EC-C07 | Close codes 0–999, 1004, 1005, 1006, 1015, 1016–2999 unassigned, ≥ 5000 | 1002 |
| EC-C08 | Valid codes 1000–1003, 1007–1011, 1012–1014, 3000–4999 | Accepted, echoed per RFC |
| EC-C09 | Close reason > 123 bytes | 1002 |
| EC-C10 | Data frames after peer Close | Ignored; no events |
| EC-C11 | Both sides send Close simultaneously | Single clean close; `on_close` once |
| EC-C12 | Peer never replies to our Close | Force close at close-timeout |
| EC-C13 | Peer sends Close then closes TCP immediately | Clean close, no error log |
| EC-C14 | App calls `send` after `close()` | `Result` error `Closing`; no bytes written |
| EC-C15 | App calls `close()` twice / from within `on_close` | Idempotent |
| EC-C16 | Heartbeat: no pong within timeout | Close 1001/1002, metric incremented |
| EC-C17 | Idle timeout with no frames | Close 1001 |
| EC-C18 | Server shutdown with open connections | 1001 to all, drain until deadline, force close; no new accepts |

## 5. Network & OS (EC-N)

| ID | Case | Expected |
|---|---|---|
| EC-N01 | `read` returns `EAGAIN` / `EINTR` | Retry/arm appropriately; no spin |
| EC-N02 | Partial `write` (kernel buffer full) | Remainder queued; `EPOLLOUT` armed; disarmed when drained |
| EC-N03 | `write` → `EPIPE` / `ECONNRESET` | Close as 1006; no SIGPIPE |
| EC-N04 | `read` = 0 (EOF) mid-message / after close | Handled per state |
| EC-N05 | `accept` → `EMFILE`/`ENFILE` | Reserve-fd trick: accept+close to shed; back off; metric; no busy loop |
| EC-N06 | `accept` → `ECONNABORTED`, `EAGAIN` | Ignore/continue |
| EC-N07 | `epoll` edge-triggered: data arrives while handling | Read-until-`EAGAIN` ensures no lost wakeup; fairness budget re-queues connection |
| EC-N08 | One connection floods; others starve? | Per-iteration budget; latency test for neighbours |
| EC-N09 | Slow consumer fills outbound queue | Policy applied (close 1008 default); other connections unaffected |
| EC-N10 | Broadcast to 10k subscribers with one slow | Slow one dropped/closed; others unaffected |
| EC-N11 | Half-open connection (no FIN/RST) | Heartbeat/idle timeouts detect |
| EC-N12 | Fd reuse hazard (close then immediate accept same fd number) | Registry keyed by connection ID + generation, not raw fd |
| EC-N13 | Cross-thread `SessionHandle::send` after session closed / loop stopped | Returns false; no UAF (TSAN/ASAN) |
| EC-N14 | `post()` flood from many threads | Bounded queue; back-pressure or error, no unbounded growth |
| EC-N15 | Timer storms (all connections' timers fire same tick) | Work budgeted; no loop stall > threshold |
| EC-N16 | System clock jumps | Only monotonic clock used for deadlines |
| EC-N17 | SIGTERM/SIGINT/SIGHUP | Graceful shutdown / reload via signalfd |
| EC-N18 | IPv6, dual-stack, `IPV6_V6ONLY`, IPv4-mapped peer addresses in per-IP limits | Correct normalisation |
| EC-N19 | Listen backlog full / SYN flood | Kernel-level; document `somaxconn`, `tcp_syncookies` |
| EC-N20 | Port already in use / permission denied at startup | Clear config error, non-zero exit |
| EC-N21 | PROXY protocol / X-Forwarded-For (if enabled) | Only from trusted proxies list; otherwise ignored |

## 6. Handler / application (EC-A)

| ID | Case | Expected |
|---|---|---|
| EC-A01 | Handler throws in any callback | Caught; close 1011; server continues |
| EC-A02 | Handler blocks (sleeps) | Loop-lag metric; watchdog log; documented as misuse |
| EC-A03 | Handler sends within `on_message`, `on_open`, `on_close` | Defined: allowed in first two; in `on_close` returns `Closed` error |
| EC-A04 | Handler stores `string_view` beyond callback | Documented UB; debug builds poison buffers after callback to catch |
| EC-A05 | Handler destroys server from callback | Detected/forbidden; assert in debug |
| EC-A06 | Re-entrancy: callback triggers `close()` which triggers `on_close` | Deferred to after current callback unwinds |

## 7. TLS (EC-T) — M8

| ID | Case | Expected |
|---|---|---|
| EC-T01 | Handshake with `WANT_READ`/`WANT_WRITE` interleavings | Works non-blocking |
| EC-T02 | Plaintext client on TLS port, TLS client on plain port | Closed cleanly |
| EC-T03 | Expired/mismatched cert at load | Startup error; on reload keep old |
| EC-T04 | Peer sends `close_notify` / abrupt TCP close | Mapped to clean / 1006 |
| EC-T05 | Renegotiation attempt | Refused |
| EC-T06 | Large write with partial `SSL_write` retry semantic (same buffer) | Correct |
| EC-T07 | Slow TLS handshake (slowloris) | Handshake timeout |

## 8. permessage-deflate (EC-D) — M9

| ID | Case | Expected |
|---|---|---|
| EC-D01 | Negotiation permutations of `server_/client_max_window_bits`, `*_no_context_takeover` | Per RFC 7692 §7 |
| EC-D02 | Compressed message split over fragments | Correct, RSV1 only on first frame |
| EC-D03 | Decompression bomb (high ratio) | Close 1009 at cap/ratio |
| EC-D04 | Invalid deflate stream | Close 1007 or 1002 per RFC; no crash |
| EC-D05 | Context takeover memory per connection | Bounded, budgeted |
| EC-D06 | Empty compressed message | Valid |
| EC-D07 | Uncompressed message when extension negotiated | Valid |

## 9. Test mapping

| Source | Covers |
|---|---|
| Autobahn sections 1–10 | EC-F, EC-U, EC-C core |
| Autobahn 12–13 | EC-D |
| Unit + property tests | splitting, checked math, state machine |
| Fuzzers | EC-H14/15, EC-F21/22, EC-U09, EC-D04 |
| Integration (real sockets, fault injection) | EC-N, EC-A, EC-H20/21/26 |
| Soak / chaos | EC-N09–N15 |

## 10. Maintenance

New bug ⇒ new row (or extend one) + regression test in the same PR.

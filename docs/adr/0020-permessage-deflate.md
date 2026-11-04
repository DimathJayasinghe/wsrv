# ADR-0020: permessage-deflate (RFC 7692) over system zlib

- Status: **Accepted** (2026-11-04; the project owner asked to continue with M9 after the M8 evidence, ADR-0005 already names zlib as the one optional compression dependency, REQUIREMENTS FR-E1)
- Date: 2026-11-04
- Deciders: project owner, maintainer assistant

## Context
Browsers offer `permessage-deflate` on every WebSocket and servers that do not accept it send more bytes than they need to; JSON-heavy traffic shrinks by a factor of 5 to 10. RFC 7692 defines it: each message is compressed with raw DEFLATE, the first frame of the message carries RSV1, and the sender removes the trailing `00 00 ff ff` of the last block. Two parameters per direction control memory and compression: `*_no_context_takeover` (reset the compressor between messages) and `*_max_window_bits` (8..15, the history size).

The code base was prepared: `FrameParser` already accepts RSV1 on the first frame of a data message when told to (`allow_rsv1`), `parse_extension_offers` already parses and bounds the header, `HandshakeAccept` already carries the offers, and the `extensions/` module exists (empty, depends on `base` and `protocol`). FRAGMENTATION.md section 2.4 says: compress the whole message first, then fragment; RSV1 on the first frame only. SECURITY_DESIGN T6 (decompression bomb) and the REQUIREMENTS limits (message size after inflate, ratio 100:1 then close 1009) are fixed. EC-D01 to EC-D07 are the edge cases.

Two facts shape the design. (1) A compressor costs memory per connection: zlib's deflate state is about `2^(windowBits+2) + 2^(memLevel+9)` bytes (about 256 KiB at 15/8, about 64 KiB at 11/6) plus about 44 KiB to inflate, so 10 000 connections with context takeover at the zlib defaults would hold 3 GB. (2) The input is hostile: a few kilobytes of deflate can expand to gigabytes.

## Options considered
1. **System zlib, linked dynamically, in `extensions/`, optional** (this ADR). Same shape as OpenSSL for TLS: a distribution library with security patches from the OS, one module includes its header.
2. **Write DEFLATE ourselves.** Inflate is about 400 lines and well specified, deflate is much more (match finding, Huffman coding, block heuristics). A hand-written compressor that is slower and larger than zlib's, and a new place for memory bugs on hostile input, for no user benefit. Rejected for deflate; for inflate the same argument applies (zlib's inflate has had the most fuzzing of any parser).
3. **Vendor zlib (or zlib-ng / libdeflate) in the tree.** Conflicts with ADR-0005's goal of small supply-chain surface and means we track CVEs ourselves. libdeflate cannot stream, which compressed fragmented messages need.
4. **Do not implement; terminate compression in a proxy.** nginx does not compress WebSocket messages, so the feature would simply not exist. Rejected.

## Decision

### 1. Dependency and module boundaries
- `WSRV_ENABLE_DEFLATE` (default ON, like `WSRV_ENABLE_TLS`): `find_package(ZLIB REQUIRED)` when ON; with it OFF the server never negotiates the extension and the module is not built. Only `extensions/` includes `<zlib.h>`; `tools/check_includes.py` gets that rule (`zlib.h` outside `extensions` is an error) and the `extensions` row stays `base protocol`.
- **`protocol/` stays free of zlib and of I/O.** It defines the seams: (a) the RFC-level parameter type and the negotiation function (section 2), pure and table-tested; (b) two small interfaces, `IMessageInflater` and `IMessageDeflater` (section 4), that the session calls. `extensions/` implements them with zlib; `ProtocolSession` receives them through its configuration (`std::shared_ptr` factory, one instance pair per connection). The composition roots (`wsrvd`, the C embedded server, examples, tests) choose the factory, like the TLS wrapper.

### 2. Negotiation (RFC 7692 sections 5 to 7)
`negotiate_permessage_deflate(offers, policy)` is a pure function in `protocol/`: it walks the offers in order and accepts the first acceptable `permessage-deflate` offer (others, including unknown extensions, are ignored, never echoed). Parameters:
- `server_no_context_takeover`, `client_no_context_takeover`: accepted; the server sets `server_no_context_takeover` in its answer whenever its policy says so, even if the client did not ask (allowed by section 7.1.1.1).
- `server_max_window_bits=N`: N in 9..15 or the offer is skipped (N outside 8..15 is malformed, section 7.1.2.1; N = 8 is skipped because zlib's raw deflate cannot do 8 and silently uses 9, and answering 9 to an offer of 8 would break the client's bound). The server's window is `min(policy, N)`; the policy itself is limited to 9..15.
- `client_max_window_bits` (with or without value): if the client offers it, the answer may carry a value (policy bound); if the client did **not** offer it, the answer must not contain it (section 7.1.2.2). The server then decompresses with a window of 15, which accepts any client window.
- A parameter with a bad value, a duplicate parameter, or an unknown parameter makes that **offer** unacceptable (the next offer is tried); if none is acceptable the extension is simply not used and the connection proceeds uncompressed (never an error). `client_max_window_bits=8` is fine (our inflater always uses a 15-bit window, which decodes any smaller window).
- The answer header is built by one function (`render_extension_answer`) and is the only extension text ever sent.

### 3. Policy and defaults (all in one `DeflateConfig`)
`enabled=false` (off by default: SECURITY_DESIGN section on CRIME/BREACH-class risk, below); `server_no_context_takeover=true`, `server_max_window_bits=12`, `memory_level=6`, `level=6`, `min_compress_size=128` (smaller messages go out uncompressed, which is always allowed: EC-D07), `max_inflate_ratio=100`. Per-connection memory with these defaults is about 40 KiB + 44 KiB, allocated lazily on the first compressed message and freed with the connection; no-context-takeover reuses the allocation (`deflateReset`/`inflateReset`) rather than freeing and allocating per message. Operators who want better ratios raise the window and allow takeover and accept the documented memory (`docs/OPERATIONS.md` table). Hard ceilings are validated like the other limits.

### 4. Inbound: inflate between the parser and the assembler, bounded
The assembler already enforces `max_message_size`, fragment counts and UTF-8 on the bytes it is given. For a compressed message the bytes it should see are the **inflated** ones, so the inflater sits in front of the assembler's payload path:
- The first frame of a data message with RSV1 starts a compressed message; continuation frames inherit it (RSV1 on a continuation or a control frame stays a 1002 error in the parser). Control frames are never compressed.
- Each payload chunk is fed to the inflater, which yields output in bounded pieces (at most 16 KiB each, never one allocation sized from peer data). Every piece goes through the same assembler accounting as uncompressed bytes, so **the message limit applies to the inflated size**, the UTF-8 check runs on inflated text, and streaming mode delivers inflated Parts.
- At the message's final frame the inflater is given the four bytes `00 00 ff ff` that the sender removed (section 7.2.2) and flushed; a stream that does not end cleanly at that point is invalid.
- **Bomb defence, three layers:** (a) the message limit on inflated bytes (1009 when exceeded, mid-stream, before more output is produced); (b) a ratio limit: total inflated bytes may not exceed `max_inflate_ratio` times the compressed bytes received for the message plus a 4 KiB allowance (so tiny valid messages are not refused), checked after every piece; exceeding it closes with 1009; (c) work per `read` call is bounded by the existing read budget, since inflate output is produced per piece in the same loop as ordinary payload. An invalid DEFLATE stream closes with 1007 (invalid frame payload data); a missing or extra RSV1 closes with 1002 as today.
- With `client_no_context_takeover` the inflater is reset after each message; otherwise the window carries over.

### 5. Outbound: compress the whole message, then fragment
`send_text` / `send_binary` (buffered sends) compress the payload when the extension is on, the message is at least `min_compress_size`, and compression made it smaller (otherwise the original goes out uncompressed, EC-D07); the scheduler queues the compressed bytes with a flag so that the first frame carries RSV1 and later fragments do not (EC-D02). The trailing `00 00 ff ff` is removed (RFC 7692 section 7.2.1). Compression is deterministic and happens when the application calls `send_*` (on the owning loop thread, as the rest of the session), never on an I/O thread other than the connection's own, and costs CPU proportional to the message, which the message and queue limits already bound; the output is bounded by `compressBound(n)`. `begin_message`/`write_part` streaming sends are **not compressed in this milestone** (valid under EC-D07; documented): their value is bounded memory, which compression with a flush per part would trade away; a later ADR may add it for `no_context_takeover` connections as FRAGMENTATION.md describes.

### 6. Configuration surface
`ServerConfig::deflate` (section 3); text keys for the embedded config and `wsrvd`: `permessage_deflate` (bool), `deflate_server_max_window_bits`, `deflate_server_no_context_takeover`, `deflate_level`, `deflate_min_size`, `deflate_max_ratio`. No ABI change: the keys are strings (ADR-0016 section 6). The PHP and other bindings see inflated messages and send uncompressed buffers; nothing in them changes.

### 7. Security notes (SECURITY_DESIGN T6 plus a new line)
- Compression of attacker-influenced text together with a secret in the same message lets an attacker who can observe message sizes guess the secret (CRIME/BREACH class). The feature is off by default; the docs say so; applications that mix secrets and user input in one message should leave it off or send those messages in a way that avoids compression (a per-message opt-out is a follow-up).
- Memory: lazily allocated, freed with the connection; the connection limit times the documented per-connection figure is the worst case, shown in OPERATIONS.
- The ratio limit and the inflated-size limit are not configurable above the hard ceilings.

### 8. Tests
Negotiation table (EC-D01: every parameter permutation, duplicates, bad values, order, unknown extensions, the "client did not offer client_max_window_bits" rule); codec round trips for every window size 9..15 and both takeover modes, with and without fragmentation; inflate limits (bomb at the ratio and at the size, byte-at-a-time input equals one-shot, truncated stream, garbage, trailing data after the final block, empty compressed message EC-D06); session-level tests with a scripted client (RSV1 only on the first frame, 1002/1007/1009 outcomes, uncompressed message while negotiated EC-D07); end-to-end over TCP and TLS; a libFuzzer target `fuzz_inflate` (arbitrary bytes, arbitrary chunking, asserts bounded output and no crash); Autobahn sections 12 and 13 in the full run (the exclusion in `EXCLUDED.md` is removed); a memory check that the per-connection allocation matches the table.

## Consequences
+ Real bandwidth savings for browsers and JSON workloads, opt-in, with documented memory and security trade-offs.
+ The protocol layer keeps its sans-I/O, library-free shape; zlib is confined to one module and can be swapped (zlib-ng, libdeflate for one-shot) behind the interfaces.
+ Limits apply to what the application sees (inflated bytes), so a compressed message cannot sneak past `max_message_size`.
− One more optional dependency (system zlib; dynamic link; present on every Linux distribution).
− CPU: compression is not free; the default threshold and level keep it modest, and the feature is off by default.
− Streaming sends are uncompressed for now; broadcast-style fan-out compression (compress once, send to many) is a follow-up with M10.

## Slices
1. This ADR; pure negotiation and answer rendering in `protocol/`, wired into the handshake (`HandshakeAccept` carries the agreed parameters), tests.
2. `extensions/`: zlib inflater and deflater behind the interfaces, limits, tests and `fuzz_inflate`.
3. Session integration: RSV1, inflate in the receive path, compress in `send_*`, scheduler flag, tests.
4. Server, config keys (library, C embedded config, `wsrvd`), end-to-end tests over TCP and TLS, Autobahn 12 and 13, OPERATIONS and lesson.

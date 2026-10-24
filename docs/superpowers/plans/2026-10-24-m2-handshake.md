# M2 HTTP Handshake Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans (inline). Steps use checkbox syntax.

**Goal:** Turn bytes from a new connection into either an accepted WebSocket upgrade (`101` response with the right `Sec-WebSocket-Accept`) or a precise rejection (400/403/405/414/426/431/505), strictly and with every limit enforced, using only the sans-I/O building blocks from M0–M1.

**Architecture:** `http/` (base only) holds a strict, incremental HTTP/1.1 request parser, token utilities and a response serialiser. `protocol/` holds the handshake: a Chain-of-Responsibility of rules (`IHandshakeRule`) run by `HandshakeNegotiator`, the accept-key computation, and Origin/subprotocol/extension-offer policy. No sockets, no clocks.

**Tech Stack:** C++23, GoogleTest, libFuzzer.

**Spec:** PROJECT_PLAN M2; REQUIREMENTS FR-H1..H9, §4 limits; PROTOCOL_EDGE_CASES §1 (EC-H01..H26); SECURITY_DESIGN T2, T8, T9, T10, T11; ARCHITECTURE §3.3–3.4; DESIGN_PATTERNS (Chain of Responsibility); ADR-0002, ADR-0006.

## Global Constraints
- All M0/M1 gates stay green (Werror, tidy, include graph: `http`→base only, `protocol`→base/crypto/http, no OS headers, banned functions, header-check, sanitizers, hardening, fuzz in CI).
- Parser limits (REQUIREMENTS §4): handshake ≤ 8 KiB total, ≤ 64 headers, request line ≤ 4096 bytes; violations → 431/414. Memory bounded by these limits; nothing sized from peer data.
- Strictness: CRLF only; reject bare CR/LF, NUL and control characters, obs-fold, whitespace before the colon, empty header names, non-token names, non-origin-form request targets, invalid percent-encoding, fragments. Header values keep obs-text (≥ 0x80) but never control characters.
- Never echo raw peer values into responses; response header names/values are validated (no CR/LF/NUL).
- Errors: parser returns `std::expected<_, HttpError{status, reason}>` (reason is a string literal); handshake returns a decision value; user hooks run inside an exception barrier.

## Review Focus
- Request smuggling / header injection shapes: duplicate `Host`, `Content-Length` + `Transfer-Encoding`, CR/LF in values, folded headers, `Content-Length: 1` on an upgrade request.
- Chunking: identical result for every split point and for one byte at a time; leftover bytes after the blank line returned untouched (EC-H19).
- Limits: exactly-at-limit accepted, one over rejected; many tiny headers; a single huge header.
- Origin policy defaults (secure): Origin present & no allow-list ⇒ 403; `Origin: null`; multiple Origin headers.
- Accept key never computed from a key that failed validation; 426 carries `Sec-WebSocket-Version: 13`.

---

### Task 1: HTTP token utilities and request model
**Files:** Create `src/http/token.hpp/.cpp`, `src/http/request.hpp/.cpp`; modify `src/http/CMakeLists.txt`; Tests `tests/http/token_test.cpp`, `tests/http/request_test.cpp`.
**Produces:**
`bool is_tchar(char) noexcept; bool is_token(std::string_view) noexcept; bool iequals(std::string_view, std::string_view) noexcept` (ASCII-only fold, never locale);
`std::string_view trim_ows(std::string_view) noexcept` (SP/HTAB);
`std::vector<std::string_view> split_list(std::string_view)` (comma list, OWS-trimmed, empty elements dropped per RFC 9110 §5.6.1);
`bool contains_token(std::string_view list, std::string_view token)`;
`struct HttpHeader { std::string name; std::string value; }`; `struct HttpRequest { std::string method; std::string target; int version_minor = 1; std::vector<HttpHeader> headers; std::vector<std::string_view> values(std::string_view name) const; std::size_t count(std::string_view name) const; }`.
- [ ] RED tests: tchar table (all 256 values vs RFC 9110 definition), `is_token("")` false, `iequals("WebSocket","websocket")`, no locale effects for bytes ≥ 0x80, `trim_ows` leaves inner whitespace, `split_list("a, b ,,c")` → a b c, `contains_token("keep-alive, Upgrade","upgrade")`; `HttpRequest::values` case-insensitive, preserves order, `count`.
- [ ] GREEN; commit.

### Task 2: Strict incremental request parser
**Files:** Create `src/http/request_parser.hpp/.cpp`; Test `tests/http/request_parser_test.cpp`.
**Produces:** `struct HttpLimits { std::size_t max_request_line = 4096; std::size_t max_total_bytes = 8192; std::size_t max_headers = 64; };` `struct HttpError { int status; std::string_view reason; };` `template <class T> using HttpResult = std::expected<T, HttpError>;` `struct ParseProgress { std::size_t consumed; bool complete; };`
`class HttpRequestParser { explicit HttpRequestParser(HttpLimits = {}); [[nodiscard]] HttpResult<ParseProgress> feed(ByteView); [[nodiscard]] const HttpRequest& request() const noexcept; [[nodiscard]] bool failed() const noexcept; void reset() noexcept; }`
Semantics: `feed` consumes bytes up to and including the final LF of the blank line and reports `complete=true`; bytes after it are NOT consumed (EC-H19). After an error the parser stays failed. Status mapping: malformed → 400, request line > limit → 414, headers/total > limit → 431, major version ≠ 1 → 505.
- [ ] RED tests (cite EC): valid request parsed; EC-H17 byte-at-a-time; EC-H18 every split incl. inside `\r\n\r\n`; EC-H19 leftover bytes untouched; EC-H15 bare LF, bare CR (also split across chunks), NUL, other control chars, obs-fold, space before colon, empty name, non-token name; EC-H14 limits at and one over (request line, total bytes, header count, one huge header); EC-H23 absolute-URI, `*`, authority form, fragment, bad percent-encoding, non-`/` start; method token rules; version syntax (`HTTP/1.1` ok, `HTTP/1.0` parsed as minor 0, `HTTP/2.0` → 505, `HTTP/1.` and `http/1.1` → 400); duplicate headers preserved; value trimming; obs-text in values accepted; `reset()` reuse; feed-after-failure keeps failing; feed-after-complete consumes 0.
- [ ] GREEN (char-by-char state machine; no unbounded growth); commit.

### Task 3: Response serialiser
**Files:** Create `src/http/response.hpp/.cpp`; Test `tests/http/response_test.cpp`.
**Produces:** `struct HttpResponse { int status; std::vector<HttpHeader> headers; }`; `std::string_view reason_phrase(int status) noexcept` (101,200,400,403,404,405,408,414,426,429,431,500,503,505); `Result<std::string> serialize(const HttpResponse&)`: `HTTP/1.1 <status> <reason>\r\n` + headers + `\r\n`; rejects names that are not tokens and values containing CR, LF, NUL or other controls (`InvalidArgument`), status outside 100–599.
- [ ] RED tests: exact 101 text; 4xx adds nothing implicitly; header injection attempts (`"x\r\nSet-Cookie: a"`) rejected; unknown status rejected; empty header list.
- [ ] GREEN; commit.

### Task 4: Handshake rules, negotiator, accept key
**Files:** Create `src/protocol/handshake.hpp/.cpp`, `src/protocol/handshake_rules.cpp`; Test `tests/protocol/handshake_test.cpp`; modify `src/protocol/CMakeLists.txt`.
**Produces:** `std::string compute_accept_key(std::string_view client_key)`;
`struct HandshakeConfig { std::vector<std::string> allowed_origins; bool allow_any_origin=false; bool allow_missing_origin=false; std::vector<std::string> subprotocols; bool require_subprotocol=false; std::vector<std::shared_ptr<IHandshakeRule>> user_rules; };`
`struct ExtensionOffer { std::string name; std::vector<std::pair<std::string,std::string>> params; };`
`struct HandshakeContext { std::string subprotocol; std::vector<ExtensionOffer> extension_offers; };`
`struct HandshakeReject { int status; std::vector<HttpHeader> headers; };` `struct HandshakeAccept { std::string accept_key; std::string subprotocol; std::vector<ExtensionOffer> extension_offers; };` `using HandshakeDecision = std::variant<HandshakeAccept, HandshakeReject>;`
`class IHandshakeRule { virtual std::optional<HandshakeReject> evaluate(const HttpRequest&, HandshakeContext&) = 0; };`
`class HandshakeNegotiator { explicit HandshakeNegotiator(HandshakeConfig); HandshakeDecision negotiate(const HttpRequest&) const; };` and `HttpResponse to_response(const HandshakeDecision&)`.
Built-in rule order: method (405 + `Allow: GET`) → HTTP/1.1 (400) → exactly one Host (400) → no body (`Content-Length` ≠ 0 / any `Transfer-Encoding` / duplicate Content-Length → 400) → Upgrade token `websocket` (400) → Connection token `upgrade` (400) → exactly one valid Sec-WebSocket-Key = Base64 of 16 bytes (400) → Sec-WebSocket-Version exactly `13` else 426 + `Sec-WebSocket-Version: 13` (missing → 400) → Origin policy (403; multiple Origin → 400) → subprotocol selection → extension-offer syntax (RFC 7692: malformed or duplicate param → 400; unknown names ignored) → user rules in an exception barrier (a throw → 500).
- [ ] RED tests: EC-H01..H13, H16, H25 plus `Origin: null`, secure defaults (Origin present, no allow-list ⇒ 403), allow_any_origin, allow_missing_origin, case-insensitive origin match, subprotocol selection by server preference, invalid/duplicate subprotocol token ⇒ 400, `require_subprotocol`, extension offers parsed into the context and never echoed in the 101, accept key for the RFC sample (`s3pPLMBiTxaQ9kYGzzhZRbK+xOo=`), `to_response` for accept/reject, user rule reject/throw/annotate.
- [ ] GREEN; commit.

### Task 5: Fuzz targets and end-to-end test
**Files:** Create `tests/fuzz/fuzz_http_request.cpp`, `tests/fuzz/fuzz_handshake.cpp`, corpora; `tests/protocol/handshake_flow_test.cpp` (parser → negotiator → serialiser on a byte stream).
- [ ] Harness properties: parser result identical for one-shot vs data-derived chunking vs byte-at-a-time; `consumed ≤ size`; Complete ⇒ headers ≤ limit and every stored name is a token and no stored value/target contains CR/LF/NUL; negotiator on any parsed request returns a decision whose serialised response is well-formed (parses back with `HttpRequestParser`-like checks: single CRLFCRLF at end, no injected header lines).
- [ ] Flow test: browser-style request in 3 chunks with 2 trailing frame bytes ⇒ 101 text exact and leftover bytes returned.
- [ ] Commit.

### Task 6: Lessons, docs, PR
- [ ] Lesson 05 (HTTP parsing: request smuggling, strictness, state machines), lesson 06 (the handshake: rules chain, Origin/CSWSH, subprotocols); update EC table status notes, learning README, PROJECT_PLAN; open PR, CI, independent review, fix, merge.

## Self-review
FR-H1..H9 → Tasks 2–4; EC-H20 (handshake timeout) and EC-H21 (disconnect mid-101) are connection-level and belong to M6 (`Connection`), H26 (/healthz) is a MAY deferred to M10; EC-H22 (garbage/TLS ClientHello on plain port) is covered by parser rejection tests, its rate-limited logging by M6. Ruling: `HttpRequest` owns copies of strings instead of the zero-copy views mentioned in ARCHITECTURE §3.3 — a handshake is parsed once per connection, bounded to 8 KiB, and owning strings remove lifetime hazards; revisit only if profiling shows it matters.

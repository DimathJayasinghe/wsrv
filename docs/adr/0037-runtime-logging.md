# ADR-0037: Runtime logging: one structured line per event, to stderr, bounded and rate-limited

- Status: **Accepted** (2026-11-26; owner-directed scope: the 1.0 release waits for logging; the design is the maintainer assistant's call, and the two choices most likely to be disputed are marked **Call**)
- Date: 2026-11-26
- Deciders: project owner, maintainer assistant
- Extends: NFR-O1, NFR-O3 (REQUIREMENTS section 3.5), SECURITY_DESIGN T14 (log injection), ADR-0030 (environment configuration). Replaces the logging promises of OPERATIONS section 2, which described a design that was never built.

## Context

NFR-O1 asks for "structured logs (key=value or JSON), levels, no secrets/PII by default, sanitized". What exists is a seam, not logging: `ILogger::log(level, message)` with a `NullLogger`, and 37 call sites in the library (7 info, 13 warn, 17 error) that hand it free-form strings such as `"conn=17 slow consumer, closing"`. **Nothing in the repository ever passes a real logger to the server.** The embedded server (what `wsrvd` and PHP's FFI mode use) keeps only the first TLS error line and drops the rest; `wsrvd` prints a few startup lines with `std::cerr`. So an operator sees origin refusals, per-IP limits, slow consumers and IPC disconnects only as counters on `/metrics`, with no line saying which, when or why. A pre-1.0 readiness review (2026-11-26) listed this as a gap; the owner decided the stable release waits for it.

What is missing, concretely: a sink that writes; a line format that cannot be forged by a peer; a stable event name an alert rule can match; a limit on how much a hostile client can make us write; a guarantee that a stuck log destination does not stall the I/O loops; configuration; and the events themselves for the cases above.

## Decision

### 1. A record, not a string
`ILogger::log(const LogRecord&)` with `LogRecord {level, event, message, conn, peer}`. `event` is a stable snake_case token of at most 32 characters (`slow_consumer`, `ip_limit`, `origin_refused`); `message` is human text; `conn` is the connection id (0 = none, NFR-O3); `peer` is the client address, filled in by the connection only when `log_peer` is on. `ILogger::enabled(level)` lets a call site skip building a string that would be dropped. The C++ headers under `src/` are not part of the 1.x promise, so the signature may change; the 37 existing call sites are converted and given event names.

### 2. The line
```
ts=2026-11-26T12:00:00.123Z level=warn event=slow_consumer conn=17 msg="outbound queue over its limit, closing"
ts=2026-11-26T12:00:01.001Z level=warn event=ip_limit peer=203.0.113.9:51234 suppressed=412 msg="per-IP limit reached, refusing"
```
- Fixed key order: `ts`, `level`, `event`, `conn` (omitted when 0), `peer` (omitted unless enabled and known), `suppressed` and `dropped` (only when above 0), `msg` last.
- `ts` is RFC 3339 UTC with milliseconds. It comes from a wall clock injected next to the monotonic one (deadlines never use the wall clock, EC-N16; this is a label, not a deadline).
- `event`, `conn`, `peer` are program-chosen or restricted to `[A-Za-z0-9._:\[\]%-]`; anything else in a peer string is replaced by `?`.
- `msg` is **always quoted** and escaped: `\\`, `\"`, `\n`, `\r`, `\t`, every other byte below 0x20, 0x7f and every byte from 0x80 up as `\xNN`. So one event is exactly one line, whatever a peer sent (T14), and invalid UTF-8 cannot reach a terminal. The escaped message is capped at 256 characters (an escape sequence is never cut in half; a cut message ends in `...`), and the whole line at 512 bytes (the worst case, every field at its cap, is 502; a compile-time assertion keeps it so): bounded by constants, never by peer input.
- Field and event names are stable within 1.x (new ones may be added); the `msg` text is not.

### 3. Levels
`error` (the server cannot do its job), `warn` (a peer or a configuration is misbehaving), `info` (lifecycle: listening, application connected, TLS reloaded, shutdown), `debug` (per connection: opened, closed with its code). `trace` is not offered. `log_level` defaults to `info`. The filter runs first: below the level a call costs one comparison, no formatting, no lock.

### 4. No peer address by default (**Call**)
An IP address is personal data in the GDPR sense, and NFR-O1 says "no secrets/PII by default". So `peer` appears only with `log_peer = true`. The cost is that the default `ip_limit` line says that a limit was hit but not by whom; the metric and the line count still tell you it is happening, and one setting names the culprit. No token, cookie, header value or payload is ever logged, at any level.

### 5. Rate limit per event
**An event name is also its bucket**, so a line any client can cause at will must never share a name with a warning about the server itself (otherwise a flood of the first would hide the second): garbage in place of an upgrade is `handshake_refused` at info, "the application cannot be asked" is `handshake_undecided` at warn. A token bucket per event name: a burst of 10, then 1 per second. What is held back is counted, and the next line of that event carries `suppressed=N`. A table of 64 event names is enough for the catalogue; further names share one bucket (bounded memory whatever the code does). A hostile client that causes a million bad handshakes therefore costs about one line per second, not a million.

### 6. A stuck destination must not stall the loops (**Call**: no thread)
`StreamLogger` (in `net/`) formats the line in the calling thread, then does one `poll(fd, POLLOUT, timeout 0)` and one `write` of the whole line. Not writable, or the write fails: the line is dropped and counted, and the next line that gets out carries `dropped=N`. A line is at most 512 bytes, below `PIPE_BUF` (4096), so concurrent writers on a pipe, a file opened with `O_APPEND` or a tty never interleave inside a line.
Rejected: a writer thread with a bounded queue (what OPERATIONS promised). It adds a thread and its start, stop and fork rules (BINDINGS), a lock plus a condition variable, and a flush at shutdown, to protect against a destination that is wedged: which the poll guard already handles by dropping. With rate limiting the volume is small enough that the synchronous write is cheap. Revisit if measurements ever say otherwise.

### 7. Where it lives (ARCHITECTURE section 2.1)
- `base`: `LogRecord`, `ILogger`, `IWallClock`, the pure line formatter, the escaper, the timestamp formatter, the rate limiter. No OS header.
- `net`: `StreamLogger`, and `poll` added to `ISyscalls` (two implementers: `LinuxSyscalls` and the test fake).
- `server`: `ConnectionHost::log` takes the record; `Connection` fills `conn` (and `peer` when the host allows it). No concrete logger.
- Composition roots: `wsrvd` and the embedded server (`capi`) build the `StreamLogger` on fd 2 from the configuration and hand it to `ServerBuilder::logger`. The embedded server logs from its own I/O threads straight to stderr; it never calls into the language runtime from them (BINDINGS rule), so a PHP process simply sees the lines on its stderr. The TLS context keeps its first-error `DetailLogger` for `start()` and `reload_tls()`, and now also forwards every record to the main logger.
- `wsrvd`'s own lifecycle lines (IPC ready, application connected or disconnected, TLS reloaded) become events through the same logger. Pre-logger diagnostics (usage, a bad setting, `--healthcheck` output) stay plain: they are for a person at a terminal before any logger exists.

### 8. Configuration
`log_level` (`error`, `warn`, `info`, `debug`; default `info`) and `log_peer` (default `false`), as ordinary keys: flags, `--config` lines, `WSRVD_LOG_LEVEL`, `WSRVD_LOG_PEER`, and the embedded text configuration. Stderr only: systemd, Docker and Kubernetes already collect and rotate it, and a file option would need reopen-on-signal and permissions design that 1.0 does not need.

### 9. Events (initial catalogue)
| Event | Level | When |
|---|---|---|
| `listening`, `publish_listening`, `admin_listening`, `shutdown` | info | lifecycle |
| `ipc_ready`, `app_connected`, `app_disconnected` | info | `wsrvd` and its worker |
| `app_rejected` | warn | wrong token, wrong uid, second application |
| `tls_reloaded` | info | SIGHUP picked up a new certificate |
| `tls_reload_failed`, `tls_config` | error | the certificate or key cannot be used |
| `conn_limit`, `ip_limit`, `loop_overloaded` | warn | a limit refused a client |
| `origin_refused` | warn | the Origin header is not allowed |
| `handshake_refused`, `handshake_timeout` | info | the upgrade was not completed (any client can cause these) |
| `handshake_decision_timeout`, `handshake_undecided` | warn | the application did not answer the handshake hook, or could not be asked |
| `slow_consumer`, `peer_stalled` | warn | a connection was closed for not reading |
| `protocol_error` | info | the peer broke RFC 6455 (the code travels in the close) |
| `publish_rejected` | warn | the publish endpoint refused a request (wrong token, bad request) |
| `conn_open`, `conn_close` | debug | per connection, with the close code |
| `internal_error`, `out_of_memory`, `poller_failure`, `timer_failure` | error | the server could not do its job |

## Consequences
+ An operator can answer "why was that client refused, who was closed for being slow, did the worker disconnect" from the log, and alert on `event=`.
+ Bounded in every direction: a line is at most 512 bytes, one event at most one line per second after a burst, memory for the limiter is a constant, and a wedged destination drops lines instead of stalling a loop.
+ Peers cannot forge or hide lines (T14), and by default no personal data is written.
− A drop is silent apart from the counter on the next line: if the destination is wedged for long, the lines in that time are gone.
− The default `ip_limit` and `origin_refused` lines do not name the client (one setting does).
− `LogRecord` changes an internal interface: 37 call sites and the recording test logger change with it.
− The log format becomes something operators build alerts on, so field and event names are now a compatibility surface within 1.x.

## Tests
Escaping (control characters, quote, backslash, DEL, bytes above 0x7f, CR/LF injection, a message at and over the cap); the timestamp (known epoch values, leap day, milliseconds); the rate limiter with a fake clock (burst, refill, suppressed count, table overflow); `StreamLogger` on a real pipe (a line arrives whole, a full pipe drops without blocking and the next line carries `dropped=`, many threads never interleave inside a line, a level below the filter writes nothing); each converted event through the recording logger; `log_level` and `log_peer` in the daemon and embedded configuration (accepted, rejected, environment); and an end-to-end check that a started `wsrvd` writes `event=ipc_ready` and `event=listening` on stderr (CI `container.yml` smoke test).

## Implementation notes (what the code settled)
- `IConnectionHost` gained `log_enabled(level)`: `Connection::log` asks it first, so a line nobody wants costs a virtual call and a comparison, and the peer address is never formatted for it. `conn_close` builds its text with `to_chars` on the stack (no allocation).
- `ServerConfig::log_peer` carries the setting to the connections; the level lives in `EmbeddedConfig::log_level()` because it belongs to the logger, not to the server. Both are keys of `EmbeddedConfig`, so `wsrvd` (flags, `--config`, `WSRVD_LOG_LEVEL`, `WSRVD_LOG_PEER`) and the PHP FFI server get them without extra code.
- `EmbeddedServer` owns the `StreamLogger` (fd 2, the steady and the wall clock), hands it to `ServerBuilder::logger` and to the TLS context (its `DetailLogger` keeps the first error line for `start_error()` and forwards every record), and logs `listening` itself when `start()` succeeded, so PHP's embedded mode says it too. `wsrvd` takes the same logger from `EmbeddedServer::logger()`: one sink for the process.
- `IpcServerConfig::logger` and `PublishDeps::logger` are optional pointers (null = silent) so the many tests that build them stay as they were. The IPC server says `app_connected`, `app_disconnected` (warn when cut off for not reading) and `app_rejected` (another user, a second application, a refused HELLO, no HELLO in time); it never says which part of HELLO failed.
- Added to the catalogue of section 9 while converting: `publish_exposed`, `admin_exposed`, `descriptor_limit`, `descriptor_limit_raised`, `decision_queue_full`, `handler_misuse`, `handler_exception`, `no_handler`, `transport_failed`, `shutdown_failed`, `ipc_no_token`, `tls_reload_skipped`, `start_failed`, `peer_stalled`. The Origin refusal is recognised by the 403 the negotiator renders (it has no other reason for 403); the connection logs `origin_refused` for it and `handshake_refused` for every other refusal, with fixed texts.
- A refused publish request logs `publish_rejected` with its status (three digits, built on the stack); the token, the topic and the body are never in the line.
- Daemon lifecycle lines moved from `std::cerr` to events (`ipc_ready`, `listening`, `shutdown`, `tls_reload_skipped`); only what happens before a logger exists (usage, a bad setting) is still printed plainly.
- **The destination is a private copy of stderr** (found by the automated security review of #97): `EmbeddedServer` takes `fcntl(F_DUPFD_CLOEXEC, 3)` of descriptor 2 in `create()`, before any socket of the server exists, and logs to the copy. Writing to "descriptor 2" later would be wrong for a host that closes its stdio after `create()` (a daemon does): a socket could take the number 2 and the log lines would go into a client connection. If stderr is closed at `create()` the copy fails and the server logs nothing (`enabled()` is false for a negative descriptor).
- **`log_level=off` is not offered in 1.0.** A host that wants silence sets `error`; a host-supplied sink would be a C ABI addition and could never call back into the language runtime from an I/O thread (BINDINGS), so it is a possible 1.x feature, not a 1.0 gap.


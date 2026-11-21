# ADR-0025: PHP embedded binding over FFI (`Wsrv\Embedded\Server`)

- Status: **Proposed** (2026-11-21; the owner moves it to Accepted)
- Date: 2026-11-21
- Deciders: project owner, maintainer assistant

## Context
M12 slice 1 gave PHP the sidecar client (`Wsrv\Client`, `Wsrv\Worker`, ADR-0017): a second process, an IPC hop, works on every stack. BINDINGS.md section 4.1 also plans Mode A, the embedded server, for PHP CLI workers: lowest latency, no second process. M11 made that possible: the C ABI (ADR-0016) is frozen in shape, and the pure-C consumer `echo_c` passes Autobahn, so a binding is "only type conversion" (wsrv.h).

PHP has two ways to call a C library: a compiled extension (`ext/wsrv`) or FFI (`ext-ffi`, built into PHP 7.4+, no compiler on the user's machine). A new language boundary, a new loadable artifact and a new public PHP class are structural, so an ADR first (CLAUDE.md).

Constraints: the language runtime never runs on a wsrv I/O thread (BINDINGS section 1); the library installs no signal handlers and must be created after `fork()` (wsrv.h); PHP-FPM must not start an embedded server (BINDINGS 4.1); the binding stays one Composer package, and the sidecar client keeps working without `ext-ffi`.

## Options considered
1. **FFI binding, pure PHP + `libwsrv.so`** (this ADR). Zero-compile install, one `FFI::cdef`, easiest to adopt, and the same pull model as C.
2. **PHP extension in C++.** Faster, no `ffi.enable` setting, but needs a compiler per PHP version and ZTS/NTS build matrix, and a second place to keep the ABI in step. Deferred: it can replace the FFI class under the same PHP API later.
3. **Callbacks from the library into PHP.** Rejected by the core rule: a callback would run PHP on an I/O thread.

## Decision

### 1. One class per concept, in a separate namespace
`Wsrv\Embedded\Server` (lifecycle, `on()`, `step()`, `run()`, `stop()`) and `Wsrv\Embedded\Connection` (a handle holding the connection id and the server). The sidecar's `Wsrv\Connection` is a different type (its commands are asynchronous and answered by a status reply), so the two never share a name. The handler names and argument lists match `Wsrv\Worker` (`open`, `message`, `close`, `error`, and later `authenticate`), so moving an application between the modes changes the constructor, not the handlers.

### 2. The C declarations are one file, checked against the header
`FFI::cdef` cannot read `wsrv.h` (no preprocessor, no `WSRV_API`). The binding ships `bindings/php/src/Embedded/wsrv.cdef`, the header's declarations with the preprocessor lines removed. A test (`EmbeddedCdefTest`) fails when any `WSRV_API` function in `include/wsrv/wsrv.h` is missing from the cdef, when the cdef declares one the header lacks, or when an enum value differs. Drift is therefore a red test, not a crash.

### 3. The library is found, then its version is checked
`Server` takes the path of `libwsrv.so` (constructor argument, then the `WSRV_LIBRARY` environment variable, then `libwsrv.so` for the loader). Right after loading it calls `wsrv_abi_version()` and refuses a different major or a minor lower than the one the binding was written for (`ABI_MINOR`), with a message that names both. The binding never guesses.

### 4. Pull model on the PHP thread
`step(int $timeoutMs)` calls `wsrv_poll` once, copies what the event holds into PHP strings, calls `wsrv_event_release`, then runs the handler. It zeroes the event and sets `struct_size` before each poll (wsrv.h rule), and releases only after the copy, so a handler that throws cannot leak an event. `run()` loops `step()` until `stop()`. An exception from a handler leaves `run()` and the server stays usable, as in `Worker`. For another event loop, `eventFd()` exposes the library's descriptor (`wsrv_server_event_fd`), left for the loop adapters (ReactPHP, Revolt), which are separate packages.

### 5. Errors
A non-OK status becomes `WsrvException` (new subclasses only where a caller acts differently): `send*` returns `true` when accepted and `false` on `WSRV_ERR_BACKPRESSURE` (the one status a caller retries), `ConnectionClosed` for `NO_SUCH_CONN`, `InvalidArgumentException` for text that is not UTF-8 (nothing is sent). Configuration and start failures throw with the library's `wsrv_error.message`. Nothing is swallowed.

### 6. Process rules
- The constructor refuses any SAPI other than `cli` and `phpdbg` (message names the sidecar as the alternative for FPM).
- The native server is created in `start()`, never in the constructor, so a script can fork between building the object and starting it (wsrv.h: create after fork). The server keeps the creating pid; a use from another pid throws before reaching C.
- PHP signal handling is the script's (`pcntl_signal`); the class offers `stop()`, which is safe to call from a handler.
- `__destruct` calls `wsrv_server_stop` and `wsrv_server_destroy`, in that order, once.

### 7. Memory and threads
Text goes to `wsrv_send_text` as the PHP string itself (FFI passes a `const char *` argument without a copy and the length is explicit). Binary data is copied once into an `uint8_t[]` for the call, because FFI converts a PHP string only to `char *` (`wsrv_send_*` copies the bytes, so the buffer is freed right after). Events are copied out whole before release. The binding never keeps a pointer into library memory across a call. Commands may come from any PHP code on the PHP thread; the binding does not spawn threads.

### 8. Out of this slice
The handshake hook (`authenticate`, `wsrv_handshake_accept/reject`), TLS settings beyond passing config keys through, per-connection backpressure queues, topics and rooms (ADR-0024 section 7 needs its own C ABI design first), the loop adapters, and Composer publication. Each gets its own slice. Autobahn through PHP (the M12 exit criterion) needs this class plus the config keys of ADR-0016 A1, which is why they were added first.

## Consequences
- A PHP CLI worker can serve WebSockets with `composer require` and a shipped `libwsrv.so`; no compiler.
- `ffi.enable` must allow the call: the default (`preload`) allows CLI scripts; hosts that set `ffi.enable=0` get a clear error from the constructor.
- A second place lists the C declarations (the cdef); the drift test keeps it honest. The sidecar client is unchanged.
- The speed of FFI calls (each `wsrv_poll` is one call, copies are small) is measured, not assumed: a benchmark against the sidecar is an exit item before any performance claim.

## Rejected details
- Parsing the header at runtime in PHP: a regex over a C header is a parser we would then have to fuzz.
- One `Wsrv\Server` name for both modes: the two have different failure shapes (status replies vs immediate statuses), and hiding that behind one class would hide the difference from the caller.

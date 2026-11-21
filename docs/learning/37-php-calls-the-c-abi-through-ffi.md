# Lesson 37 — PHP calls the C ABI through FFI (M12 slice 2, ADR-0025)

## Goal
After this lesson you can explain what FFI is, load a C library from PHP, and say which parts of a binding are type conversion and which are decisions. You can also name the five ways an FFI binding crashes or lies, because we hit four of them.

## The problem
The sidecar client (lesson 17) needs a second process. A PHP CLI worker that wants WebSockets in its own process, with no daemon and no compiler, needs to call `libwsrv.so` directly. PHP's built-in way is **FFI** (foreign function interface): `FFI::cdef("C declarations", "lib.so")` returns an object whose methods are the C functions. M11 made this cheap on purpose: the whole API is plain C, pull-based, with ids instead of pointers, so a binding "only converts types" (`wsrv.h`).

## 1. The declarations are the header, minus what FFI cannot read
FFI parses C declarations but has no preprocessor, so `#ifdef`, `WSRV_API` and `extern "C"` break it. Two bad choices: copy the declarations by hand (they drift), or parse the header with a regex at run time (a parser nobody fuzzes). We took a third: `tools/CdefGenerator.php` removes exactly those four things and nothing else, writes `src/Embedded/wsrv.cdef`, and a test fails when the file is stale. The cdef therefore cannot say anything the header does not.

Note what `FFI::cdef` does at load: it resolves **every** declared function in the library at once. A library older than the header fails immediately with "Failed resolving C function". That is why `Server` checks `wsrv_abi_version()` and names both versions: a clear message instead of a loader error.

## 2. The pull loop, in PHP
`step()` is the C loop from lesson 36 with PHP types:

```
zero the event, set struct_size  ->  wsrv_poll  ->  copy fields into PHP values
  ->  wsrv_event_release (in a finally)  ->  run the handler
```

The copy comes before the release because after the release the pointers in the event are dead. The release is in a `finally` so a failure while copying cannot leak the event. The handler runs after both, so a handler that throws cannot leave an event unreleased either.

## 3. Decisions the binding makes (the library does not)
- **Backpressure.** `send()` returns `false` for `WSRV_ERR_BACKPRESSURE`: the one status a caller retries. Every other failure is an exception. A `bool` you forget to check loses a message, so the example checks it and closes the slow client with 1013.
- **Create after fork.** The native server is created in `start()`, not in the constructor, and remembers its pid; a forked child gets an exception instead of the library's `WRONG_PROCESS`, and its destructor does not free the parent's server.
- **No FPM.** The constructor refuses every SAPI but `cli` and `phpdbg`, and the message names the sidecar.
- **Fail closed on what is not built.** `handshake_hook` would make every upgrade wait for an answer this slice does not give, so `start()` refuses it by name instead of hanging every browser until the timeout.

## Pitfalls we hit
1. **`FFI::cast('uint8_t *', $array)` reinterprets the array's bytes as an address.** The first binary send segfaulted inside `memcpy`. An array passed as an *argument* decays to a pointer correctly; just pass the array.
2. **A PHP string converts to `const char *`, not to `const uint8_t *`.** Text is passed as is (the length is explicit, so no NUL is needed); binary data is copied once into a `uint8_t[]`.
3. **`char[128]` struct fields arrive as PHP strings; `char *` returns arrive as `CData`.** `FFI::string()` on the first is a `TypeError`. One small helper accepts both.
4. **An arrow function captures by value.** `pump($server, fn () => $log === [...])` never saw `$log` change and timed out. Closures that must see later changes use `use (&$log)`.
5. **A sanitizer build cannot be loaded into `php`.** PHP opens its extensions with `RTLD_DEEPBIND`, which the ASan runtime refuses, even with the runtime preloaded. The end-to-end test is registered only for builds without a sanitizer (the release preset in CI); the cdef test runs everywhere.

## Tests
- `EmbeddedCdefTest` (3, in `php_unit`): the cdef equals the generator's output for the real header, every `WSRV_API` function is declared, the generator removes the guards and keeps the declarations.
- `embedded_interop.php` (`php_embedded_interop`): the real `libwsrv.so` and a raw WebSocket client in one PHP thread: open, text echo, binary echo, clean close with a code, the server's own close code, `ConnectionClosed` after close, invalid UTF-8 refused before sending, `stop()` from a handler, a bad key named in the error, a missing library named, `handshake_hook` refused.

## Exercises
1. Add `Server::broadcast(array $ids, string $text)`. Where does a partial failure (one id gone, one backpressured) get reported, and what does the caller do about each?
2. `step()` allocates nothing per event except the PHP strings. Measure `wsrv_poll` + copy for 100 000 small messages. What would a native extension save?
3. Write the fork test: create the object, `pcntl_fork()`, call `start()` in the child. What does the parent's `port()` return, and why must the child not destroy anything it did not create?

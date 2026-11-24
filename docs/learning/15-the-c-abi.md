# Lesson 15 — The C ABI: a thin door for every language

## Goal
Let a program written in another language (PHP first, later C#, Java, Node, Python, Go) run the server without ever running that language on an I/O thread, and without that language owning any protocol logic. After this lesson you can explain what an *ABI* is and why C is the language of the door, why events are pulled instead of delivered by callback, what `wsrv_poll`, `event_fd` and `wsrv_event_release` promise, how the shared library hides everything except the functions on its list, and how a C-only program proves the whole thing works.

This is the C half of the lesson planned as "the C ABI and talking to PHP". The PHP half arrives with milestone M12.

## 1. The problem
PHP's normal model is one request, one short life. It cannot sit on thousands of long-lived sockets, and we do not want it to try: socket handling, framing and UTF-8 checks are exactly the places where hand-written code goes wrong. The same is true of every other language we want to serve. So the server stays in C++ (one audited copy), and each language gets a small *binding* that only converts types and calls into it.

Two different things are called an interface here, and the difference matters:

- **API** (application programming interface): the names and types you compile against. If a header changes, you recompile.
- **ABI** (application *binary* interface): the exact machine-level contract of a compiled library: symbol names, how arguments are passed, how structs are laid out in memory. If the ABI changes, an already-compiled program that loaded the new library can crash, with no compiler to warn anyone.

C++ has no stable ABI across compilers or even library versions (name mangling, `std::string` layout, exceptions). C does. Every language can call C (this is called an *FFI*, foreign function interface), so `include/wsrv/wsrv.h` is the one place where our promises must never silently change. Within a major version we may only *add*.

## 2. Design: bindings translate, the core decides
The rule from ADR-0016 section 9, in one sentence: **anything that could be got wrong is implemented once, below the language line.** Protocol rules, UTF-8 checks of both directions, limits, backpressure, connection ids, "exactly one Close for every Open", fork behaviour and error codes all live in C++ and are tested once. A binding contains no state machine. The review test: if a connection bug can only be fixed by changing PHP code, the bug is in our C layer.

This is also why the C functions are one-line translations. The behaviour is the class `EmbeddedServer` (`src/capi/embedded_server.hpp`), which has C++ tests that never touch C. `src/capi/c_api.cpp` only checks pointers, calls one method and turns the `Result` into a status.

### Pull, not push
Most event APIs take a callback: "call me when a message arrives". Here that would run PHP (or Python's interpreter lock, or the JVM) on our I/O thread, in the middle of a connection's read path. That breaks the hard rule *the language runtime is never called from I/O threads*, and a slow or crashing callback would stall every connection on that loop.

So the library copies each event into a bounded queue and the host *pulls*:

```c
wsrv_event ev;
ev.struct_size = sizeof ev;            /* tells the library which version of the struct you know */
while (wsrv_poll(server, &ev, 0) == WSRV_OK) {
    /* ... use ev.type, ev.conn, ev.data, ev.len ... */
    wsrv_event_release(&ev);
}
```

`wsrv_server_event_fd()` returns an `eventfd` that becomes readable when events wait (a message's wake-up is batched per loop iteration, ADR-0035, lesson 49, so it can lag by one iteration), so a host with its own event loop (ReactPHP, Revolt, `stream_select`, asyncio, libuv) can wait on it with everything else and call `wsrv_poll(.., 0)` when it fires. The descriptor and the queue are updated together under one mutex, so a wake-up cannot be lost between "look" and "sleep" (the trap from lesson 13, section 4).

### Commands go the other way, by id
`wsrv_send_text(server, conn, ...)` takes a number, never a pointer. The library finds the connection's `SessionHandle` (lesson 13) and posts the send to the owning loop. An id for a connection that has ended gives `WSRV_ERR_NO_SUCH_CONN`; a pointer would have been a use-after-free. Ids are process-unique and never reused. "OK" means *queued for the loop*, not *delivered*.

### Ownership rules that avoid leaks across a language boundary
- The library never returns memory the caller must free for an error: `wsrv_error` is a fixed struct the caller supplies (`message[128]`, truncated).
- Event payloads are owned by the library until `wsrv_event_release`. Releasing twice is safe.
- No exception, `longjmp` or C++ type crosses the line: every exported function runs its body in `guarded(...)`, which catches only `std::bad_alloc` and `std::exception` (never `catch(...)`) and maps them to `WSRV_ERR_RESOURCE` / `WSRV_ERR_INTERNAL`.
- No signal handlers are installed; the host owns signals. A server must be created *after* `fork()`, because threads do not survive a fork: a forked child gets `WSRV_ERR_WRONG_PROCESS`, and destroying an inherited server there abandons the thread handle instead of joining a thread that only exists in the parent.

## 3. Walkthrough: from header to shared library

**`include/wsrv/wsrv.h`** is pure C99: `typedef enum`, opaque `struct` handles, fixed-width integers, a `size_t` length next to every pointer (text is never assumed NUL-terminated). Each function is marked `WSRV_API`, which expands to `__attribute__((visibility("default")))`.

**`wsrv.symbols`** is a plain list of the exported names, one per line. It is the single source of truth for what the library exports.

**The shared library** (`src/capi/CMakeLists.txt`, target `wsrv_shared`, file `libwsrv.so`) is built from `c_api.cpp` plus the static C++ libraries it needs. Making sure *only* the listed functions leave the library took three attempts, and each failure teaches something:

1. `-fvisibility=hidden` hides symbols of our own code. But the static libraries were compiled without it.
2. `-Wl,--exclude-libs,ALL` makes every symbol from static archives local. Better, but `nm -D` still showed four names such as `_ZTISt11_Mutex_baseILN9__gnu_cxx12_Lock_policyE2EE`: the *typeinfo* (runtime type description) of libstdc++ templates. Those come from standard headers that declare default visibility, and they were instantiated inside our own object file.
3. A **linker version script**: `{ global: wsrv_poll; ...; local: *; };`. The linker then makes everything not on the list local, regardless of attributes. CMake generates it from `wsrv.symbols`, so the list is the only place to edit. It is deliberately *anonymous* (no `WSRV_0 { ... }` node): a named node turns exports into `wsrv_poll@@WSRV_0`, which is symbol versioning, a separate ABI decision that deserves its own ADR.

Even then `nm -D` can show `__bss_start`, `_edata`, `_end`: linker boilerplate that some toolchains export. The check script ignores exactly those.

**`c_status.cpp`** holds the one table from internal `ErrorCode` to the C status values. It lists every code on purpose; a new, unmapped code falls to `WSRV_ERR_INTERNAL`, and a test walks all codes so that is a decision and never an accident.

## 4. Tests: what proves it
| Test | What it catches |
|---|---|
| `capi_c_status_test` | a new `ErrorCode` that maps to OK or to nothing |
| `capi_embedded_server_test` (including the fork test) | behaviour of the whole server through the same calls the C functions make |
| `capi_header_matches_symbols` (`tools/check_c_api.py`) | a `WSRV_API` function missing from `wsrv.symbols` (the linker would hide it) or a listed name the header never declared |
| `capi_abi_symbols` (`tests/capi/check_symbols.sh`) | the real `libwsrv.so` exporting more or fewer symbols than the list |
| `capi_consumer` (`tests/capi/consumer.c`) | everything above, end to end, in C |

The consumer is compiled as **C99 with `-Wpedantic -Wconversion -Werror`**. That is the real test that `wsrv.h` is C: a C++ type, `bool` without `<stdbool.h>` or a C++-only construct would fail to compile. It links the shared library, configures and starts a server, opens a raw TCP socket, performs the handshake by hand (checking the RFC 6455 section 1.3 example `Sec-WebSocket-Accept` value), sends a masked text frame, polls the OPEN and TEXT events, echoes the text through `wsrv_send_text`, reads the server's frame, closes the socket and sees exactly one CLOSE event, then stops and destroys the server. It also checks that bad input is refused: an unknown key, invalid UTF-8, an unknown connection.

The `wsrv.h` file is excluded from clang-tidy on purpose (`HeaderFilterRegex` covers `src/` only): the tidy naming and `modernize-*` rules are C++ rules and would demand `using` instead of `typedef` in a C header.

## 5. Pitfalls
- **Passing the wrong `struct_size`.** The library refuses a `wsrv_event` whose `struct_size` is smaller than its own; this is how a *newer* library can add fields at the end and still serve an *older* program.
- **Freeing the event data yourself.** `ev.data` points into library memory; copy what you need, then call `wsrv_event_release`.
- **Polling from two threads.** `wsrv_poll` is single-consumer by design; it is documented, not locked.
- **Reading `event_fd` yourself.** It is level-triggered by the queue's state (and the wake-up, ADR-0035). Draining it by hand desynchronises it. Only `wsrv_poll` touches it.
- **Calling into a destroyed server from a callback thread of your own.** `wsrv_server_destroy` must be the last call on that server, from one thread.
- **Assuming "OK" means delivered.** It means handed to the connection's loop.
- **Exposing a C++ type "just this once".** One `std::string` in the header and every binding breaks on the next compiler.

## 6. Exercises
1. Add a new status code `WSRV_ERR_FOO` to the end of the enum. List every file that has to change (header, name table, symbols, tests). Which test fails first if you forget one?
2. Comment out one line of `wsrv.symbols`. Which test fails, and with what message? Now add a `WSRV_API` function to the header without listing it: which test catches that, and why would `capi_abi_symbols` alone *not*?
3. In `consumer.c`, remove `wsrv_event_release` after the TEXT event and run the `dev` preset. What does AddressSanitizer report?
4. Write a ten-line C program that waits on `wsrv_server_event_fd` with `poll(2)` instead of calling `wsrv_poll` with a timeout. Why must it still call `wsrv_poll(.., 0)` until `WSRV_TIMEOUT`?
5. Why does the destructor of `EmbeddedServer` in a forked child call `release()` on the thread handle instead of `join()`? What would `std::thread`'s destructor do with a joinable handle?

# Lesson 36 — A C host proves the ABI (M11 exit: the C consumer passes Autobahn)

## Goal
After this lesson you can write a small program that serves WebSockets through the C ABI alone, explain what a "host" has to do that the library cannot do for it, and say why we ran the conformance suite through that program instead of trusting that "the C++ server passed, so the C one will".

## The problem
The C++ echo server passed Autobahn (517 cases, 0 FAILED). The C ABI wraps the same core, so it should pass too. "Should" is the gap this lesson closes. The C layer adds things the C++ server does not have: configuration by text keys, an event queue between the I/O threads and the host, commands by connection id, and a host that polls. Any of these can lose a message, reorder two, or refuse a configuration that the C++ API accepts. A conformance run through the C API is the cheapest way to find out.

It found one such gap before it ran a single case: **the C configuration could not say what the suite needs.** Three limits (one frame of 16 MiB, a message in 65 536 fragments, bursts of small frames) were reachable only through C++ structs. A C host had no way to ask for them, so it could never have passed the cases that depend on them. Adding keys changes the public surface, so it went through an ADR amendment first (ADR-0016 A1).

## 1. What a host does
The library does the protocol. The host does the application. For an echo that is one decision: "send back what came in". Everything else in `examples/echo_c/main.c` is the shape every C host has:

```
config -> server -> start -> loop { poll -> handle -> release } -> destroy
```

- **Configuration is text.** `wsrv_config_set(config, "key", "value", &error)`. The program never parses a number: it passes `"9001"` and the library validates it. One place decides what a valid port is.
- **Zero the event before every poll.** `wsrv_poll` leaves the event untouched on a timeout. If the program released an event that a previous iteration filled, it would free memory twice. Zeroing (and setting `struct_size`) makes "release is always safe" true.
- **Release after the copy.** `wsrv_send_text` copies the bytes, so the event can be released right after the call returns.
- **Signals.** The library installs none (`wsrv.h` says so). The host does, and keeps the handler tiny: it sets one `volatile sig_atomic_t`, the only type C lets a handler write. That flag is the single global in the program, and the comment says why.

## 2. Backpressure is the host's problem to name
`wsrv_send_*` can return `WSRV_ERR_BACKPRESSURE`: "a queue is full, try again later". An echo that ignores the return value silently loses a message when the queue is full. One that retries out of order breaks the order the protocol promises. `echo_c` does the simplest correct thing: this thread is the only sender, so it waits a millisecond and sends the *same* message again; after about a second it closes the connection with 1013 ("try again later"). It never drops and never reorders. A real application would queue per connection; the lesson is that the choice exists and must be made, not defaulted.

## 3. Three sizes must agree
For a 16 MiB message to be echoed, three limits must all admit it:

| Limit | Why |
|-------|-----|
| `max_message_size` (and `max_frame_payload` for a one-frame message) | the protocol layer refuses larger input with 1009 |
| `event_queue_bytes` | the host receives messages whole; an event bigger than the free queue is refused ("event queue: full") and never reaches the host |
| the outbound queue, sized as twice `max_message_size` by the same key | the echo must be able to send back what was accepted |

The first and third come with `max_message_size`. The second is separate, and its default (8 MiB) is smaller than the message. `echo_c --autobahn` sets it to 64 MiB. The smoke test shows the other direction too: without `--autobahn`, a 2 MiB frame gets 1009, because the defaults stayed low on purpose.

## 4. Do not raise a limit without a ceiling
The new keys are `max_frame_payload`, `max_fragments` and `max_data_frames_per_second`. Each accepts `1` up to a ceiling and nothing else. Zero is refused even though the C++ field treats `0` as "off" for the frame rate: a C host should not be able to switch the flood guard off with a typo, and SECURITY_DESIGN says there is no "unlimited" value. The test written first (`TheThreeLimitKeysRefuseZeroAndAnythingAboveTheirCeiling`) pins the edge: the ceiling is accepted, ceiling plus one is not.

## Tests
- `EmbeddedConfig.TheThreeLimitKeys*` (4 tests): defaults unchanged, values stored, zero / sign / junk / above-ceiling refused, a refusal changes nothing.
- `capi_echo_c_smoke` (CTest, label `capi`, 0.3 s): starts the real `echo_c`, handshake with a computed `Sec-WebSocket-Accept`, text and 2 MiB binary echoed, invalid UTF-8 closed with 1007 by the library, the same 2 MiB frame refused with 1009 without `--autobahn`, `SIGINT` exits with status 0. A failed check kills the server it started, so a red test cannot hang CTest.
- `tests/autobahn/run.sh --capi` and `--capi --wss`: the full suite through `echo_c`. Results (2026-11-21): 0 FAILED plain and over wss. The same seven NON-STRICT cases as the C++ server, plus one or two that change from run to run (3.4, 4.1.5, 4.2.5): the echo goes through the host, so it sometimes loses the race against the connection failing on the violation that follows. Both outcomes are accepted by the suite. Details and the honest limit of that explanation: `tests/autobahn/EXCLUDED.md`.

## Pitfalls
- **A test that reads one frame.** The first smoke run failed with "65536 of 2097152 bytes": the library had split the echo into 64 KiB frames (RFC 6455 section 5.4 allows it). The server was right; the test assumed one frame per message. A client must reassemble.
- **`pkill -f` matches its own shell.** The pattern was in the command line of the shell running it, which killed the shell. Anchor with `-x name` for exact process names.
- **Reports from two servers.** Both servers use the same port and the runner wipes the reports directory at start; each server therefore has its own agent name (`wsrv`, `wsrv-c`) so two runs can be compared side by side.

## Exercises
1. `echo_c` retries a send for about a second. Write a client that stops reading and sends large messages; what does the server do, and which close code does the client see?
2. Replace the sleep-and-retry with a per-connection queue. What bounds the queue, and what happens when it is full?
3. Set `max_message_size` to 20 MiB with the default `event_queue_bytes` and send 16 MiB. What do you observe, and where is it counted?

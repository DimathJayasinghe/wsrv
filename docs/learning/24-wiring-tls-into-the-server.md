# Lesson 24 — Wiring TLS into the server (and why the interface lives in `net/`)

## Goal
After this lesson you can explain how a server gains TLS without knowing about OpenSSL, why the wrapper interface sits one module lower than you would first guess, and what an end-to-end test proves that unit tests cannot.

## 1. The problem
`TlsTransport` (lesson 23) encrypts one connection. Something must wrap every accepted socket in it. The obvious way, `if (tls) transport = TlsTransport::create(...)` inside the server, breaks two rules: the server would include `tls/` (and so, through it, OpenSSL's types), and the zero-dependency build (`WSRV_ENABLE_TLS=OFF`) would no longer compile the server.

## 2. Dependency inversion
The server instead depends on a tiny interface it does not implement:

```cpp
class ITransportWrapper {
    virtual std::unique_ptr<ITransport> wrap(std::unique_ptr<ITransport> plain) = 0;
};
```
`LoopShard::adopt` does `transport = wrapper->wrap(std::move(transport))` if a wrapper was injected. The composition root (the one place allowed to name concrete types) builds a `TlsTransportWrapper` and passes it to `ServerBuilder::transport_wrapper(...)`. This is the same pattern as the handshake decider (lesson 21): the core exposes a *hook*, the optional feature plugs in.

## 3. Why `net/`, not `server/`
The first design put the interface in `server/`. But `tls` implements it, and `tls` may depend only on `base` and `net`; if `tls` included a `server/` header the module graph would gain `tls -> server`, and a test (`module_graph`) pins that graph. The interface mentions only `ITransport`, which is a `net/` type, so it belongs in `net/`. **Rule of thumb: put an interface in the lowest module that owns every type it mentions.**

## 4. Failure and threads
- `wrap` may fail (out of memory). Null means "drop this connection": the slot counter is given back (otherwise the connection limit would slowly leak), the socket is closed by the destructor of the object the wrapper consumed, a line is logged, and the server keeps accepting.
- `wrap` runs on every loop thread, so implementations must be thread-safe. `TlsTransportWrapper` asks the context for the current certificate (a short mutex-guarded pointer copy) and does everything else on private objects.

## 5. What an end-to-end test adds
Unit tests (lesson 23) feed bytes to one transport. Here a real client (`TlsClient`, OpenSSL on the test side) talks to a real `Server` over loopback. That is the only way to catch wiring faults: the wrapper never called, the handshake timer not covering TLS, interest flags wrong so the handshake stalls, shutdown not reaching the peer. **Mutation check:** we temporarily disabled the `wrap` call; 12 of 15 tests failed, so the tests are really exercising the wiring.

## Pitfalls
- A test client built on a virtual `send(ByteView)` hides the base class's `send(string_view)` overload; add `using Base::send;`.
- `SSL_set_tlsext_host_name` is a macro containing a C-style cast, which the project's warnings reject; call `SSL_ctrl` directly in test helpers.
- `Server::request_shutdown()` is `[[nodiscard]]`: tests must check it, or a failed shutdown request shows up later as a hang.

## Exercises
1. Add a second wrapper in a test that counts calls and passes the transport through unchanged. Which existing test would fail if `adopt` called it twice?
2. Why can the TLS handshake share the HTTP handshake timeout without a separate timer? What would break if the HTTP handshake timer were cancelled when the first byte arrives?
3. Make `wrap` throw. Which `catch` in `adopt` handles it, and what is released?

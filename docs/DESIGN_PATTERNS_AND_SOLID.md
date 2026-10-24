# Design Patterns & SOLID

Patterns are tools, not decoration. **Every pattern below exists because it solves a named problem in this codebase.** Do not add a pattern without a problem; do not avoid one when the problem is present.

## 1. SOLID — applied rules and review questions

### S — Single Responsibility
*A class has one reason to change.*
- `FrameParser` changes if the frame wire format changes. It does not know about close handshake, sockets, or limits policy beyond what the header validation needs.
- `Connection` glues transport ↔ session ↔ write queue. It contains **no** protocol rules and **no** policy.
- **Review check:** Can you describe the class in one sentence without "and"? Does it have > ~7 collaborators? Split it.

### O — Open/Closed
*Extend by adding code, not editing stable code.*
- New poller, transport, handshake rule, extension, logger = new class implementing an interface; zero edits to `Connection`/`ProtocolSession` logic.
- **Review check:** Does adding feature X require a new `switch` case in a core class? Replace with a strategy/registry.

### L — Liskov Substitution
*Implementations honour the interface contract.*
- Every interface documents pre/post-conditions, error semantics, thread-affinity, and ownership in its header. Example: `ITransport::read` returns `WouldBlock` (never blocks), `0` bytes means orderly EOF, never throws.
- A contract test suite (`tests/contract/transport_contract.hpp`) is run against **every** `ITransport` (plain, TLS, fake). Same for `IPoller`.
- **Review check:** Does a subclass strengthen preconditions, weaken postconditions, or throw where the base doesn't?

### I — Interface Segregation
*Small, role-specific interfaces.*
- `ILogger`, `IMetrics`, `IClock` are separate. `ITransport` has no timer methods. `IWebSocketHandler` methods default to no-ops so handlers implement only what they use.
- Consumers receive the narrowest interface (`IClock&`, not `Server&`).
- **Review check:** Is any implementer forced to stub methods it doesn't need?

### D — Dependency Inversion
*High-level policy depends on abstractions.*
- `ProtocolSession` depends on nothing but `base`; `Connection` depends on `ITransport`, not `EpollPoller`/OpenSSL.
- Wiring happens in one composition root (`ServerBuilder::build()`); no `new ConcreteType` deep inside logic classes. No singletons, no global loggers.
- **Review check:** Does a high-level class `#include` a concrete low-level header? Is a collaborator constructed inside instead of injected?

## 2. Pattern catalogue (where and why)

| Pattern | Where | Problem solved |
|---|---|---|
| **Reactor** | `EventLoop` + `IPoller` | Scale many connections on few threads without blocking |
| **State** | `ProtocolSession`, `Connection` lifecycle, `FrameParser` | Eliminate flag soup; make illegal transitions unrepresentable (`std::variant` of state structs or enum + transition table) |
| **Strategy** | `IPoller`, `ILoopAssigner`, slow-consumer policy, rate limiter algorithm | Swap behaviour without touching callers |
| **Decorator** | `TlsTransport` wraps `PlainTransport`; `RateLimitedTransport`, `LoggingTransport` for diagnostics | Add cross-cutting behaviour while keeping the same interface |
| **Chain of Responsibility** | `IHandshakeRule` chain (host → upgrade → version → origin → subprotocol → user auth) | Ordered, composable validation; each rule independently testable |
| **Observer / Listener** | `IWebSocketHandler`, metrics events | Decouple server from application logic |
| **Builder** | `ServerBuilder`, `SessionConfig::Builder` | Many optional settings, validate once, produce immutable config |
| **Facade** | `Server` | Simple entry point hiding loops, acceptors, registry |
| **Factory / Abstract Factory** | `IHandlerFactory` (handler per connection), `ITransportFactory` (plain vs TLS) | Create per-connection objects without coupling to concrete types |
| **Adapter** | `EpollPoller` over `epoll_*`; `OpenSslTransport` over `SSL*`; `GetRandomAdapter` | Isolate OS/third-party APIs |
| **Command / Task** | `EventLoop::post(std::move_only_function)` | Cross-thread work as queued, owned closures |
| **Publish–Subscribe / Mediator** | `Broker` | Topics and rooms without sessions knowing each other |
| **Flyweight / Shared immutable** | Broadcast frame shared across subscribers | Avoid N copies and N framings |
| **Object Pool** | `BufferPool` | Eliminate hot-path allocation |
| **Null Object** | `NullLogger`, `NullMetrics` | Remove `if (logger)` checks |
| **RAII / Scope guard** | `Fd`, `Socket`, `TlsContext`, `LoopThread` | Deterministic cleanup, exception-/error-safe |
| **Template Method (sparingly)** | Contract test fixtures | Shared test skeleton; prefer composition in production code |
| **Pimpl** | Public headers in `include/wsrv/` | Hide OpenSSL/epoll types, stable ABI/compile times |
| **Policy-based design (templates)** | `Parser<Limits>` compile-time constants where hot | Zero-cost configurability. Use only with benchmark evidence |

### Patterns we deliberately avoid
- **Singleton / global registries** — hidden dependencies, untestable, threading hazards.
- **Deep inheritance hierarchies** — prefer composition + small interfaces.
- **Service locator** — hides dependencies.
- **Visitor over `std::variant` for two cases** — `std::visit` with overloaded lambdas or `if constexpr` is clearer.
- **Over-abstraction** — no interface with a single implementation *and* no foreseeable second, unless it is a test seam (clock, transport, poller).

## 3. Composition root

Only `ServerBuilder::build()` (and `main()` of examples) may name concrete implementations. Everything else receives interfaces via constructor injection. This is a review-blocking rule.

```cpp
auto server = wsrv::ServerBuilder{}
    .listen("0.0.0.0", 8080)
    .threads(4)
    .allowed_origins({"https://app.example.com"})
    .limits(wsrv::Limits::strict())
    .logger(std::make_shared<JsonLogger>(std::cerr))
    .handler_factory([] { return std::make_unique<EchoHandler>(); })
    .build();                      // validates; the only throwing call
server->run();
```

## 4. Design checklist for new classes

1. State the single responsibility in one sentence in the header comment.
2. List dependencies; are they interfaces? injected?
3. Document thread-affinity (`loop-thread-only`, `any-thread`, `immutable`).
4. Document ownership & lifetime of every pointer/view argument.
5. Define error behaviour (`Result` vs `noexcept` vs assert).
6. Provide a fake/test double if it's a seam.
7. Write the unit test before the implementation (protocol layers).
8. Does it need an ADR? (new module, new dependency, new thread, new interface)

## 5. Reviewer cheat-sheet (reject on sight)

- `static` mutable state, `extern` globals.
- `dynamic_cast` / `typeid` in logic code (design smell).
- Class > ~300 lines or > ~7 injected collaborators.
- Functions > 40 lines or > 4 params (introduce a struct).
- Boolean parameters (`send(data, true, false)`) → use enums/options struct.
- Protocol logic inside `net/` or socket calls inside `protocol/`.
- `#include` of a concrete implementation from a higher-level module.
- Catch-all `catch (...) {}` that swallows.

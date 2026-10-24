# CLAUDE.md — rules for AI agents in this repo

Project: **wsrv**, a from-scratch C++23 WebSocket (RFC 6455) server core with a stable C ABI and language bindings (PHP first). Read `README.md`, then the doc that matches your task. Docs are the contract; if code and docs disagree, stop and ask.

## Before coding
1. Find the requirement ID (`docs/REQUIREMENTS.md`) and edge-case rows (`docs/PROTOCOL_EDGE_CASES.md`) your change touches.
2. Check module boundaries (`docs/ARCHITECTURE.md` §2.1). `protocol/` and `http/` never include OS/net headers.
3. Structural change (new module, dependency, thread, interface, ABI)? Write an ADR first.
4. Write the failing test first (protocol/net).

## Hard rules
- C++23, warnings-as-errors; follow `docs/CODING_STANDARDS.md` (naming, RAII, `Result<T>`, no exceptions on runtime paths, no raw `new/delete`).
- All wire-derived integers range-checked; checked arithmetic; no allocation sized from peer data without a configured bound. Never trust declared lengths.
- Every buffer/queue bounded with a defined overflow action.
- No singletons/globals/mutable statics; inject dependencies; concrete types only in the composition root.
- No locks in connection/protocol code; cross-thread only via `EventLoop::post`.
- Banned: `strcpy sprintf gets strtok atoi rand system popen`, C-style casts, `using namespace` in headers, `catch(...)` that swallows.
- The language runtime is never called from I/O threads (`docs/BINDINGS.md`).
- Do not add dependencies without an ADR.
- Cite RFC sections in comments for protocol rules.

## Teaching mode (owner requirement)
The owner is learning while we build. For every step: say **what** we are building, **why** it exists (the problem, the RFC rule, the attack it prevents), and **how** (the technique/pattern), *before* writing code. Keep code small enough to explain. After each milestone slice, add or update a lesson in `docs/learning/` (concept, walkthrough, pitfalls, exercises). Show tests running and explain failures. Define jargon on first use.

## Docker dev container gotcha
Builds run in the `wsrv-dev` container (`make shell`; one-off: `docker compose exec -T dev bash -c '...'`). Docker Desktop's bind mount shows **stale content for host files replaced by rename**. Agents: change existing files with the Edit tool (in place) or `rm` before re-creating with Write; if a build behaves as if a fix is missing, compare `wc -l` of the file on host vs container and run `make restart`.

## Definition of done
Follow `CONTRIBUTING.md` checklist. Run format, tidy, tests with sanitizers before declaring done. Report failures honestly with output.

## Don't
- Don't invent behaviour not in the RFC/requirements; flag ambiguities.
- Don't weaken a limit or a security default to make a test pass.
- Don't expand scope beyond the task; note follow-ups instead.

## Commands (once M0 lands)
`cmake --preset dev && cmake --build --preset dev -j && ctest --preset dev --output-on-failure`

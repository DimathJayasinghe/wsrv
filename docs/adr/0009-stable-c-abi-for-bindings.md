# ADR-0009: Stable C ABI as the single binding surface

- Status: Proposed
- Date: 2026-10-24

## Context
Goal: PHP and many other languages use one core. C++ ABI is unstable across compilers/stdlibs; exceptions and templates don't cross FFI. Runtimes such as PHP (Zend) and CPython cannot safely be re-entered from arbitrary native threads.

## Options considered
1. Per-language C++ bindings (pybind11, node-addon-api, PHP-CPP) directly on C++ API — couples every binding to C++ ABI/standard; duplicated glue.
2. SWIG-generated bindings — large generated surface, awkward async/ownership.
3. Hand-designed C ABI (`wsrv.h`) + thin per-language wrappers; pull-based event queue (`wsrv_poll` + `event_fd`).

## Decision
Option 3 (details: BINDINGS.md §3). The core never calls into a foreign runtime from I/O threads; hosts pull events and push commands via bounded queues. Connection handles are 64-bit IDs.

## Consequences
+ One audited surface; any language with FFI can bind; ABI checkable in CI.
− Slightly less ergonomic than direct callbacks; per-event copy at the boundary.
− ABI evolution discipline (additive-only within a major).

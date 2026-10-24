# ADR-0010: Support embedded and sidecar integration modes

- Status: Proposed
- Date: 2026-10-24

## Context
Project lead proposed running the server on a separate thread within PHP. That works only in long-running PHP processes via a native extension/FFI; PHP-FPM requests are short-lived and cannot own sockets. Many PHP deployments are FPM-based.

## Options considered
1. Embedded only (extension/FFI + background native thread).
2. Sidecar only (`wsrvd` + IPC).
3. Both, sharing the same core.

## Decision
Option 3. Sidecar first (works with every PHP stack and every other language through a documented IPC protocol), embedded second (performance, single-process deployment). PHP userland never runs on multiple threads.

## Consequences
Need a versioned IPC protocol spec (`docs/IPC_PROTOCOL.md`, M11), a `wsrvd` binary, and fork-safety/signal rules documented. Two deployment stories to document and test.

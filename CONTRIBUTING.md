# Contributing

Thanks for helping. This project aims for protocol-correct, secure, maintainable code; the rules below keep it that way. Read [docs/CODING_STANDARDS.md](docs/CODING_STANDARDS.md), [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), and [docs/SECURITY_DESIGN.md](docs/SECURITY_DESIGN.md) before your first PR. By participating you agree to the Code of Conduct (Contributor Covenant 2.1, added with the repo bootstrap).

## Where to start
Small, well-bounded tasks that need no deep knowledge of the core (each has an acceptance test in the same place as its neighbours):
- Docs: a missing example in `docs/OPERATIONS.md`, a typo, a clearer error message in a README (`docs:` PRs skip CI).
- PHP: tests for `Wsrv\Install\LibraryInstaller` over a real local HTTPS server, a `Wsrv\Embedded\Server` example with TLS (`bindings/php`).
- Edge cases: pick a row of `docs/PROTOCOL_EDGE_CASES.md` marked "later" and write its failing test first.
- Tooling: musl (Alpine) build in CI, or a macOS `IPoller` design note (ADR first).
- Bindings: a second language over the C ABI (`include/wsrv/wsrv.h`, `examples/echo_c` is the reference consumer).
Open an issue before starting so nobody duplicates it.

## Workflow

1. **Discuss first** for anything non-trivial: open an issue (bug / feature / design). Structural changes need an ADR ([docs/adr/](docs/adr/)).
2. Fork / branch from `develop` (the integration branch): `feat/<topic>`, `fix/<topic>`, `docs/<topic>`, and open the PR **into `develop`**. `main` only receives `develop` when a milestone is ready to ship (release tags are cut from `main`).
3. **Test first** for protocol/net code (write the failing test or add the edge-case row).
4. Keep PRs small and single-purpose (≈ ≤ 400 changed lines excluding tests/generated).
5. Run locally: `cmake --preset dev && cmake --build --preset dev && ctest --preset dev`, then format/tidy targets.
6. Open the PR using the template; link the issue, requirement IDs (`FR-F5`), and edge-case rows (`EC-U06`).
7. At least one maintainer approval; two for changes in `http/ protocol/ net/ extensions/`, TLS, limit defaults, or the C ABI.
8. Feature PRs into `develop` are squash-merged with a Conventional Commit title. `develop` into `main` is a **merge commit** (never squash: a squash would leave the two branches diverged and every later release PR would re-show old changes).

## Commit messages (Conventional Commits)

`type(scope): summary` — types: `feat fix perf refactor test docs build ci chore security`. Scope = module (`protocol`, `net`, `capi`, `php`…). Imperative mood, ≤ 72 chars, body explains *why*. Sign off with DCO: `git commit -s` (`Signed-off-by:`). Breaking changes: `!` after type/scope and a `BREAKING CHANGE:` footer.

## Definition of Done (every PR)

- [ ] Builds warning-free on GCC and Clang (warnings are errors)
- [ ] `clang-format` and `clang-tidy` clean; banned-function and include-graph checks pass
- [ ] Unit tests for new/changed behaviour; error paths covered; no sleeps/flaky tests
- [ ] Edge-case catalogue updated (new row or test reference)
- [ ] ASAN/UBSAN (and TSAN if threaded) clean
- [ ] Fuzz target/corpus updated if parsing changed; fuzz smoke passes
- [ ] Coverage thresholds maintained
- [ ] No new `static` mutable state, no module dependency violations, no unbounded buffers
- [ ] Public API / C ABI change? → docs, `CHANGELOG.md`, ABI check, semver impact stated
- [ ] Security checklist ticked (see PR template) and SECURITY_DESIGN updated if threats/controls changed
- [ ] Docs/ADR updated in the same PR
- [ ] SPDX header on new files; DCO sign-off on commits

## Code review guidelines

Reviewers check, in order: correctness & protocol compliance → security → design (SOLID, layering; see DESIGN_PATTERNS §5) → tests → readability/naming → performance (only with data). Be kind, specific, and cite the rule or RFC section. Authors respond to every comment; reviewers re-review promptly (target < 2 business days).

## Reporting bugs / vulnerabilities

Bugs: GitHub issue with version, platform, minimal repro, expected vs actual, logs (sanitised). **Vulnerabilities: do not open a public issue** — follow [SECURITY.md](SECURITY.md).

## Adding a language binding

Follow [docs/BINDINGS.md](docs/BINDINGS.md): thin wrapper over the C ABI, no protocol logic, CI job running the Autobahn suite through your binding, README with quick start, and a named maintainer (Tier 2 until accepted as Tier 1 per GOVERNANCE.md).

## Licensing

Contributions are licensed under the project license (proposed Apache-2.0, ADR-0011). Only submit code you have the right to contribute.

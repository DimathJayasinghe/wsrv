## What and why

<!-- One paragraph: the problem, the approach. Link the issue, requirement IDs (FR-F5) and edge-case rows (EC-U06). -->

## Definition of Done (see CONTRIBUTING.md)

- [ ] Builds warning-free on GCC and Clang (warnings are errors)
- [ ] `clang-format` / `clang-tidy` clean; banned-function and include-graph checks pass
- [ ] Tests for new/changed behaviour written first; error paths covered; no sleeps / flaky tests
- [ ] Edge-case catalogue updated (new row or test reference)
- [ ] ASan/UBSan (and TSan if threaded) clean
- [ ] Fuzz target/corpus updated if parsing changed
- [ ] No new mutable global state, no module-dependency violations, no unbounded buffers
- [ ] Public API / C ABI change? docs, CHANGELOG, ABI check and semver impact stated
- [ ] Docs / ADR updated in this PR
- [ ] SPDX header on new files; commits signed off (`git commit -s`)

## Security checklist (required for changes to http/ protocol/ net/ extensions/ TLS, limits, C ABI)

- [ ] Every wire-derived integer is range-checked before use as a size, index or allocation
- [ ] No allocation or loop is sized by untrusted data without a configured upper bound
- [ ] Peer-supplied strings are never logged or echoed unsanitised
- [ ] Failure closes the connection with the most specific close code (fail closed)
- [ ] docs/SECURITY_DESIGN.md updated if a threat or control changed
- [ ] Not applicable (explain):

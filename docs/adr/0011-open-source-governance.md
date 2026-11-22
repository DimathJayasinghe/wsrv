# ADR-0011: Open-source licensing and governance

- Status: Accepted (owner confirmed Apache-2.0, 2026-10-24; project name `wsrv` confirmed by the owner on 2026-11-22; the PHP package keeps the placeholder `wsrv/client` as its Composer name)
- Date: 2026-10-24

## Context
The project will be open to the community. License choice affects adoption, patents, and compatibility with PHP/Python/Node ecosystems and OpenSSL linking.

## Options considered
1. MIT — simplest, no patent grant.
2. **Apache-2.0** — permissive, explicit patent grant, compatible with OpenSSL 3 (Apache-2.0); widely accepted by companies.
3. MPL-2.0 / LGPL — copyleft on library changes; may deter adoption.

## Proposed decision
Apache-2.0, DCO sign-off (`git commit -s`) instead of a CLA, Contributor Covenant 2.1 Code of Conduct, GOVERNANCE.md (maintainers, Tier 1/Tier 2 bindings, decision process via ADRs, release manager), SECURITY.md private disclosure. SPDX headers in every source file.

## Consequences
Need LICENSE, NOTICE, CODE_OF_CONDUCT.md, GOVERNANCE.md, issue/PR templates, "good first issue" labelling and a public roadmap before announcing the repo.

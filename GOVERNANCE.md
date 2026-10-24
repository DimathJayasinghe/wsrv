# Governance

Draft — finalised with ADR-0011 once the owner confirms license and structure.

## Roles
- **Maintainers** — merge rights, release management, security triage. Added by consensus of existing maintainers after sustained quality contributions.
- **Contributors** — anyone submitting issues, PRs, docs, bindings.
- **Binding maintainers** — own one language binding.

## Decisions
- Day-to-day: PR review with lazy consensus.
- Architectural or API/ABI changes: ADR PR; accepted with ≥ 2 maintainer approvals and a 5-day comment window (shorter for urgent security fixes).
- Disputes: maintainers vote (simple majority); project lead breaks ties.

## Binding tiers
| Tier | Meaning | Requirements |
|---|---|---|
| 1 | Maintained by core team, released with core | Autobahn via binding in CI, semver lockstep, security response commitment |
| 2 | Community-maintained, in-repo `bindings/` or linked | Named maintainer, CI green, docs; may lag releases |

## Releases
Semantic versioning; release manager rotates; checklist in docs (created at M10). Security releases may bypass the normal cadence.

## Code of Conduct
Contributor Covenant 2.1; enforcement by maintainers; reports to the conduct contact.

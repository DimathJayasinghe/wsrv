# Security Policy

## Supported versions

Latest minor release of the current major, plus the previous major for 6 months after a new major (policy takes effect at 1.0).

## Reporting a vulnerability

**Do not file a public issue.** Use GitHub's "Report a vulnerability" button on the repository's Security tab (a private security advisory). If that button is not shown, open an issue titled "security contact request" **without any details** and a maintainer will arrange a private channel. Include affected version, platform, impact, minimal reproduction (fuzzer input or script if possible).

## Our process

| Step | Target |
|---|---|
| Acknowledge | 3 business days |
| Triage & severity (CVSS) | 7 days |
| Fix + regression test + fuzz corpus entry on private branch | 30 days (critical: 7–14 days) |
| Coordinated release, advisory, CVE request | with fix; reporter credited unless they decline |

## Scope

In scope: memory safety, protocol-handling flaws, DoS via unbounded resource use, TLS misconfiguration defaults, authentication/origin bypass in the handshake logic, C ABI misuse leading to memory corruption, IPC protocol flaws in `wsrvd`.
Out of scope: vulnerabilities requiring a malicious or compromised *trusted* handler/host application, volumetric network DDoS, issues only in unsupported configurations (e.g. explicitly enabling `allow_any_origin` and then being hit by CSWSH).

## Design reference

Threat model and controls: [docs/SECURITY_DESIGN.md](docs/SECURITY_DESIGN.md).

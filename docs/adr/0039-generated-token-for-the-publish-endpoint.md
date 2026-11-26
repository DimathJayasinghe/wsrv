# ADR-0039: The publish token must be a generated one

- Status: **Accepted** (2026-11-26, by the project owner in chat; the owner also chose the scope: a hard requirement in 1.1, not a warning). Amends ADR-0028 section 3 ("the token is required").
- Date: 2026-11-26
- Deciders: project owner, maintainer assistant

## Context
The publish endpoint (ADR-0028, shipped in 1.0.0) is an HTTP listener whose only authentication is a bearer token. Anyone who can reach the port can try tokens as fast as the daemon answers, and a valid one lets them publish to every room (SECURITY_DESIGN T22). The only rule about the token was a length floor of 32 bytes.

The audit of the TCP IPC link (ADR-0038 section 3) found that a length floor checks the one thing that is easy to measure: `MyCompanyWebsocketToken2026!!!!!` is 32 bytes and guessable. ADR-0038 therefore accepts over TCP only a token that `wsrvd --generate-token` made. The publish token has the same exposure, and more of it: the endpoint is plain HTTP, `publish_host` is often non-loopback in containers (ADR-0028 section 4), and 1.0.0 documented `openssl rand -hex 32` in some places and `head -c 32 /dev/urandom` in others, while the Laravel guide told readers to make "a file of at least 32 random bytes". Nothing stops a reader from writing a long sentence instead.

## Decision

### 1. The same form, everywhere the endpoint runs
When `publish_port` is set, the contents of `publish_token_file` must be the form `wsrvd --generate-token=FILE` writes (ADR-0038 section 3: `wsrv1_`, 64 lowercase hex digits of 32 bytes from the kernel's random source, `_`, a CRC-32 in 8 hex digits; 79 bytes). Any other token stops the start with `InvalidConfig`. The message:
- names the fix (`wsrvd --generate-token=FILE`),
- says that the rule is new in 1.1, because the person reading it may be running a configuration that worked in 1.0,
- never prints the token.

The rule applies on loopback too: any local user, any container sharing the network namespace and any process on the host can reach `127.0.0.1`, and one rule is easier to state and to test than two.

### 2. Where it is enforced
In `wsrvd`'s start-up, next to the IPC token check (`read_secrets`). `EmbeddedConfig::set_publish_token` and `PublishEndpoint::create` keep their 32-byte floor as the last defence. The format lives in `apps/wsrvd/` (it is the daemon's file format, and `src/` may not include `apps/`), and the C ABI has no way to set a publish token (the endpoint is a `wsrvd` feature), so the daemon's start-up is the only way a token can arrive.

### 3. What this does not do
- No back-off or lockout after failed requests, for the reason given in ADR-0038 section 3: a global one lets anyone who can reach the port lock the real publishers out.
- No escape hatch (no `publish_allow_weak_token`). A switch that turns a security default off is the weakening the project's rules forbid, and the one-line fix is always available: generate a token.
- The Unix-socket IPC token is unchanged (the kernel authenticates the peer there).
- The PHP `Publisher` does not learn the format. It sends whatever it is given; the daemon is the authority, and a client-side check would only be a second copy of the rule.

## Options considered
1. **Warn at start in 1.1, require in 2.0.** Compatible, and my first recommendation. The owner chose against it: a warning is read by the careful and ignored by the people at risk, and 2.0 may be years away.
2. **Accept legacy tokens that look random** (a length and character-class heuristic). Cannot be decided: `aaaaaaaa...` and `0123456789abcdef...` pass most heuristics. Rejected.
3. **The generated form (this ADR).**

## Consequences
- **A 1.0 deployment with a token it made itself fails to start after upgrading to 1.1** (a minor version). The owner accepted this for the security gain. It is a one-time rotation, and the CHANGELOG entry says "breaking for a self-made publish token" with the commands for each environment (Docker, Compose, Kubernetes, systemd). The publishers (anything that reads the file or the Secret) must be restarted or re-read after the rotation.
- The examples and manifests that made a token with `head`/`od`/`openssl` now call the generator (`examples/docker`, `deploy/kubernetes`, `deploy/systemd`, the Laravel guide, the PHP package README, the PHP interop test). The reference Kubernetes manifest makes the token per pod in its init container: its publisher is the worker in the same pod, so no Secret is needed (a shared token for publishers outside the pod is documented as `kubectl create secret ... --from-file`).
- The compose example's idempotent `[ -s token ] ||` guard would keep an old token across an upgrade, so its README tells people with a 1.0 volume to delete the token.
- The separate `wsrv-chat-demo` repository must be re-checked when 1.1 ships.

## Slices
1. This ADR, the failing start-up tests, the check in `read_secrets` (tests first, mutation-checked).
2. Examples, manifests, CI (`kubernetes.yml`, `container.yml`, `interop.php`) and documentation.

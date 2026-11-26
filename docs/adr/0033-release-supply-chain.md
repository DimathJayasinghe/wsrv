# ADR-0033: The release supply chain: scan, sign, attest, SBOM, a second registry

- Status: **Accepted** (2026-11-23; 1.0 production-readiness work, owner-delegated scope)
- Date: 2026-11-23
- Deciders: project owner, maintainer assistant
- Amends: ADR-0026 (distribution), ADR-0026 amendment 1 (container image)

## Context
A network daemon that people put in front of their users is a supply-chain target: they run `docker pull`, and must be able to tell that the bytes are the ones this repository's workflow built, from this source, and what is inside. ADR-0026 already publishes checksums and a build-provenance attestation for the binaries. The container image, the artifact most users run, had neither a signature, an SBOM, nor a vulnerability scan, and lived only on GHCR; Docker Hub is where most people look first.

## Decision
All of it in `release.yml`, on a version tag, with every third-party action pinned to a full commit SHA:
1. **Scan before push.** Each architecture's image is built, scanned with Trivy (HIGH and CRITICAL, fixable only: `ignore-unfixed`), and pushed only if clean. A vulnerable image never becomes a tag. (A CVE with no fix upstream cannot be fixed by us; those stay visible in the scan output without blocking.)
2. **Sign the joined image keyless with cosign.** No long-lived key exists to leak: the signature is bound to this workflow's OIDC identity and recorded in the public Rekor log. Verify with `cosign verify --certificate-identity-regexp ... --certificate-oidc-issuer https://token.actions.githubusercontent.com`.
3. **Provenance attestation for the image** (`attest-build-provenance`, pushed to the registry next to the image): `gh attestation verify oci://IMAGE -R OWNER/REPO`.
4. **SBOM**: an SPDX JSON of the final image (syft) is a release asset, listed in `SHA256SUMS` and attested with the binaries.
5. **Docker Hub copy, only if the repository has `DOCKERHUB_USERNAME` and `DOCKERHUB_TOKEN`**: the two per-architecture manifests are joined under the Docker Hub name as well (same bytes, `:latest` only for non-prerelease tags), and signed there. Without the secrets the step is skipped and the release still succeeds on GHCR.
6. The release is all-or-nothing: `publish` needs both the binaries and the image, so a failed scan or signature stops the GitHub release.

## Consequences
+ A user can verify provenance, signature and contents of what they run, with standard tools.
+ No new secret except the optional Docker Hub token; signing needs none.
− Every release run is longer (scan, sign, SBOM): a few minutes, and only on tags.
− A scan finding in the Ubuntu base image blocks a release until the base or the package set is updated: that is the point, but it can delay a tag. A finding that is a false positive for this image would need an explicit, reviewed ignore file.
− Trivy's vulnerability database and the sigstore services are external dependencies of the release job (not of the build or the image).

## Amendment A1 (2026-11-26): found while verifying `1.0.0-rc.1`

The first tag to run these steps for real passed every check, with two lessons.

1. **Verify with cosign 3 or newer.** The release job installs cosign v3 (`cosign-installer` v4), which writes signatures in the Sigstore bundle format. cosign 2.x answers `no signatures found` for the same image. The user-facing text (`docker/HUB.md`) now says so. Verified with cosign v3.0.6: the identity is `release.yml@refs/tags/v1.0.0-rc.1`, the certificate chain and the transparency-log entry check out.
2. **The image is run before it is pushed.** The release job built and scanned each architecture's image but never started it, and `container.yml` starts it on x86-64 only, so nothing ever ran the arm64 image. The job now runs the image on its own (native) runner and waits for the container healthcheck, before the scan and the push.

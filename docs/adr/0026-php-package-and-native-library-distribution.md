# ADR-0026: Composer package and distribution of `libwsrv.so`

- Status: **Proposed** (2026-11-22; the owner moves it to Accepted)
- Date: 2026-11-22
- Deciders: project owner, maintainer assistant

## Context
The PHP binding is complete (sidecar client, embedded server over FFI, examples, tests on PHP 8.2 to latest). Publication is the next step in the owner's order. Two things must be shipped to a PHP user, and they travel differently:

1. **PHP code** (`bindings/php/src`): installed by Composer from Packagist.
2. **The native library** (`libwsrv.so`, for the embedded mode) and the `wsrvd` daemon (for the sidecar mode). Composer installs PHP files from a git tag; it does not build C++ and a binary does not belong in git history.

Facts that shape the choice:
- Packagist reads a `composer.json` at the **root** of a git repository. Ours is in `bindings/php/`.
- `libwsrv.so` needs glibc, OpenSSL 3 (`libssl.so.3`, `libcrypto.so.3`), zlib and libstdc++ at run time (checked with `ldd` on the release build). musl (Alpine) is not covered by a glibc build.
- `Server` already refuses a library with a different ABI major or an older minor, so a wrong library fails with a clear message, never with a crash.
- The library is the security boundary of every deployment: a download that is not verified is a supply-chain hole (SECURITY_DESIGN; BINDINGS section 6 says "checksums + signatures").
- CI minutes are scarce, so the release build must run on tags only.

## Options considered

### Where the package lives
1. **Root `composer.json` in this repository** pointing its autoload at `bindings/php/src`, with `.gitattributes export-ignore` keeping the C++ sources, tests and docs out of the Composer archive. No second repository, no split job; one tag is the release of everything. The package name and the version are the project's.
2. **A read-only split repository** (`git subtree split` on each tag, pushed by a workflow). Cleaner `composer.json` at the root, but needs a second repository, a push token (a secret with write access), and a job that can fail or drift.
3. **Path or VCS repository entry in the user's `composer.json`** only. No Packagist; users must know the repository URL. Fine as a stopgap, not publication.

### How `libwsrv.so` reaches the user
A. **Ship the binary inside the Composer package.** One step for the user, but it puts binaries in git or in a build step that rewrites the tag, and one `.so` cannot serve both x86-64 and aarch64.
B. **Release artifacts plus a verifying fetch helper** (this ADR). A tag workflow builds `libwsrv.so` and `wsrvd` per architecture, publishes them with a `SHA256SUMS` file on GitHub Releases; the package ships a small PHP script that downloads the right one, checks it and puts it in a known directory.
C. **Build from source on the user's machine** (documented, no helper). Always available, needs a toolchain; it stays the documented fallback (and the only path for musl).
D. **A PHP extension compiled by PECL.** Rejected in ADR-0025 for now.

## Decision

1. **Option 1 for the package** (root `composer.json`, no split repository). The PHP code is autoloaded from `bindings/php/src`; `.gitattributes` excludes everything else from the archive. The existing `bindings/php/composer.json` stays for development inside the binding directory, and a test checks that the two agree on `autoload` and `require`.
2. **Option B plus C for the library.** A release workflow runs on `v*` tags only: it builds the `release` preset on `ubuntu-22.04` (oldest glibc we support, so newer systems run it) for x86-64 and aarch64, packs `libwsrv.so.<version>` and `wsrvd`, writes `SHA256SUMS`, and attaches them to the GitHub Release. The artifacts are named `wsrv-<version>-linux-<arch>-glibc.tar.gz`. Building from source (option C) is documented for every other platform, including musl.
3. **The helper, `vendor/bin/wsrv-install-lib`.** It reads the package's own version, picks the architecture from `php_uname('m')`, downloads the archive and `SHA256SUMS` over HTTPS, refuses anything whose SHA-256 does not match, extracts to a directory the user names (default `vendor/wsrv/lib`, never a system path), and prints the value to put in `WSRV_LIBRARY`. It does not run as a Composer post-install script: code that downloads and runs at install time is the thing supply-chain attacks look for, and users who vendor dependencies or install offline must not be surprised. It is a command the user runs on purpose.
4. **Trust model, stated honestly.** The checksum file comes from the same release as the archive, so it detects corruption and a tampered archive, but not a tampered release as a whole. The release assets are therefore also signed (GitHub artifact attestation, verifiable with `gh attestation verify`), and the helper accepts `--sha256=<hash>` so a careful user can pin the value from a source they trust. Verifying the attestation inside PHP is out of scope; the documentation shows the `gh` command.
5. **Versioning.** The package version is the project version, tags are `vMAJOR.MINOR.PATCH`. `Server::ABI_MINOR` already ties the binding to a library; the helper installs the library of the package's own tag, so they always match unless the user overrides it.
6. **Supported platforms for the prebuilt library: Linux x86-64 and aarch64, glibc 2.35 or newer, OpenSSL 3.** Anything else builds from source.

## What is the owner's, not decided here
These are outward-facing and need the owner, because they cannot be undone or are visible to others:
- **The Packagist vendor and package name.** Today `wsrv/client` is a placeholder. Registering a vendor on Packagist claims a public name; the recommendation is `wsrv/wsrv` (one package for both modes) but it is the owner's choice. The rename would be one line in two files.
- **Making the repository public** (the release workflow's attestation and Packagist's webhook both need it; see the public-repo readiness report).
- **Pushing the first tag** (a tag starts the release workflow and is the version users see).
- **Signing identity**: the workflow uses GitHub's own attestation (keyless, no secret to keep); a GPG key would be an extra secret.

## Consequences
- `composer require` gives PHP code; one command gives a verified library. No compiler is needed on glibc Linux for either mode. The sidecar client needs neither the library nor FFI.
- One repository, one tag, one release. Packagist's auto-update webhook is the only external moving part.
- The release workflow costs CI minutes only on tags. It cannot be tested before the first tag; the first release should be a `-rc` tag (a pre-release that Composer ignores by default) so that a failure costs nothing visible.
- The helper is a new piece of PHP that downloads files. It is small, takes no input from the network except the archive and the checksum file, writes only below the directory the user gave it, and has an offline test (a local `file://` server and a bad-checksum case).
- Open question for review: whether to also publish a `wsrvd` Debian/OCI image. Not needed for Composer; leave it to M14.

## Rejected details
- A Composer plugin or post-install script that fetches the library: runs code at install time with the user's rights.
- A `latest` download: the helper installs the library of its own version only.
- Bundling OpenSSL and zlib into the `.so` (static linking): a security update would then need a new wsrv release; the system libraries keep getting patched by the distribution.

## Revisit trigger
Users on musl or macOS asking for prebuilt libraries (then add targets to the release matrix), or a second language binding that needs the same artifacts (then the helper's logic moves to a shared script).

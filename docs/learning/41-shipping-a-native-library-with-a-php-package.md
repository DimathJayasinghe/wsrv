# Lesson 41 — Shipping a native library with a PHP package

## Goal
After this lesson you can explain why `composer require` cannot deliver `libwsrv.so` by itself, what the helper `wsrv-install-lib` does instead, and why it is a command you run and not a hook that runs for you.

## The problem
Composer installs PHP files from a git tag. Our embedded mode also needs a compiled library, and a binary in git history is permanent, differs per CPU, and cannot be reviewed. A convenient answer, "download it in a post-install script", is also the shape of many supply-chain attacks: code that fetches and runs things the moment you install.

## 1. One repository, one tag
Packagist reads `composer.json` at the root of a repository. Ours lived in `bindings/php/`. Two ways out: a second repository fed by a split job (a token to protect, a job to keep alive), or a root `composer.json` whose autoload points into `bindings/php/src`. We took the second. `.gitattributes` marks everything else `export-ignore`, so the archive Composer downloads is the PHP code, not the C++ tree. `PackageTest` fails when someone adds a top-level directory and forgets to list it, and when the two `composer.json` files drift.

## 2. Release artifacts and a verifying helper
A workflow that runs only on `v*` tags builds `libwsrv.so` and `wsrvd` for x86_64 and aarch64 and attaches them, with a `SHA256SUMS` file and a build attestation, to a GitHub release. `vendor/bin/wsrv-install-lib` downloads the files of its own version, compares the SHA-256, and only then writes them, atomically (a temporary file, then `rename`), into a directory you named.

What it refuses, and the test for each:
- a file whose hash differs (`a library that does not match its checksum is refused and nothing is written`);
- a file the checksum list does not mention (silence is not trust);
- a version such as `latest`, `../1.2.3` or `1.2.3\n` (names are built from a checked version, never taken from the network);
- anything larger than 64 MiB.

## 3. Pitfall found by the test: `$` is not "end of string"
In PCRE, `$` also matches just before a trailing newline, so `"1.2.3\n"` passed the version check. `\z` means the true end. The test listed the bad inputs and one failed; that is the whole point of listing them.

## 4. Trust, honestly
The checksum file comes from the same place as the file it checks, so it catches corruption and a swapped archive, not a compromised release. The attestation (provenance signed by GitHub, checked with `gh attestation verify`) and `--sha256=` (a hash from a source you trust) cover that. Say what a control does not do.

## Exercises
1. Why is the helper not a Composer `post-install-cmd` script? List two things that go wrong for someone who installs with `--no-scripts` or inside a locked-down CI.
2. Add a third architecture to `LibraryInstaller::architecture()`. Which tests change, and which workflow line?
3. The release workflow compares the tag, `CMakeLists.txt` and `Version.php`. Why is a unit test alone not enough?

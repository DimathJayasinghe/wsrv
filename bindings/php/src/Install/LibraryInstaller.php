<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv\Install;

use Closure;

/**
 * Downloads the native library of this package's own version and refuses anything whose SHA-256 does
 * not match (ADR-0026 sections 3 and 4). It is a command the user runs on purpose, never a Composer
 * post-install hook.
 *
 * Where the bytes come from is a Closure, so the tests run without a network:
 * `fn (string $assetName, int $maxBytes): string`, which throws InstallException on any failure.
 *
 * What it does not do: run anything it downloaded, write outside the directory it was given, follow a
 * `latest`, or take a file name from the network (names are built here from a checked version and a
 * fixed architecture list).
 */
final class LibraryInstaller
{
    public const MAX_LIBRARY_BYTES = 64 * 1024 * 1024;
    public const MAX_SUMS_BYTES = 64 * 1024;
    public const SUMS_NAME = 'SHA256SUMS';

    /** @param Closure(string, int): string $fetch */
    public function __construct(private readonly Closure $fetch)
    {
    }

    /** "libwsrv-0.1.0-linux-x86_64-glibc.so" or "wsrvd-0.1.0-linux-x86_64-glibc". */
    public static function assetName(string $kind, string $version, string $arch): string
    {
        self::checkVersion($version);
        $arch = self::architecture($arch);
        return match ($kind) {
            'libwsrv' => "libwsrv-$version-linux-$arch-glibc.so",
            'wsrvd' => "wsrvd-$version-linux-$arch-glibc",
            default => throw new InstallException("unknown asset kind '$kind'"),
        };
    }

    /**
     * The prebuilt files are Linux (glibc) ELF files, and the server is built on epoll. On another system
     * the architecture check alone would pass (Apple Silicon says "arm64", which maps to aarch64) and the
     * installer would write a library that can never load, so stop first and say what works instead.
     *
     * @param string $family PHP_OS_FAMILY
     */
    public static function checkOperatingSystem(string $family): void
    {
        if ($family !== 'Linux') {
            throw new InstallException(
                "no prebuilt library for $family: wsrvd and libwsrv are Linux only. On $family run the wsrvd Docker image "
                . "(see docs/DEPLOYMENT.md) and your worker in a container too; the PHP client works on $family"
            );
        }
    }

    /** The release's architecture name for what php_uname('m') says; anything else has no prebuilt library. */
    public static function architecture(string $uname): string
    {
        return match ($uname) {
            'x86_64', 'amd64' => 'x86_64',
            'aarch64', 'arm64' => 'aarch64',
            default => throw new InstallException(
                "no prebuilt library for architecture '$uname' (x86_64 and aarch64 on glibc Linux only); build from source, see docs/BUILD_AND_TOOLING.md"
            ),
        };
    }

    public static function checkVersion(string $version): void
    {
        if (preg_match('/^\d{1,4}\.\d{1,4}\.\d{1,4}(-[0-9A-Za-z]+(\.[0-9A-Za-z]+)*)?\z/', $version) !== 1) {
            throw new InstallException('not a release version: ' . substr(addcslashes($version, "\0..\37\177..\377"), 0, 40));
        }
    }

    /**
     * The `<64 hex>  <name>` lines of a SHA256SUMS file. Anything else is an error: a line that is
     * almost right must not be skipped and read as "no entry".
     *
     * @return array<string, string> file name => lower-case hash
     */
    public static function parseSums(string $text): array
    {
        $sums = [];
        foreach (explode("\n", $text) as $line) {
            $line = rtrim($line, "\r");
            if ($line === '') {
                continue;
            }
            if (preg_match('/^([0-9a-fA-F]{64}) [ *]([A-Za-z0-9._-]+)\z/', $line, $m) !== 1) {
                throw new InstallException('malformed line in ' . self::SUMS_NAME);
            }
            if (isset($sums[$m[2]])) {
                throw new InstallException("{$m[2]} is listed twice in " . self::SUMS_NAME);
            }
            $sums[$m[2]] = strtolower($m[1]);
        }
        return $sums;
    }

    /**
     * Fetch, verify, then write. Nothing is written for a file that failed its check.
     *
     * @param string|null $pin a SHA-256 the caller trusts for the library, instead of the SHA256SUMS entry
     * @return array<string, string> 'libwsrv' (and 'wsrvd') => the path written
     */
    public function install(string $version, string $arch, string $dir, ?string $pin = null, bool $withDaemon = false): array
    {
        $kinds = $withDaemon ? ['libwsrv', 'wsrvd'] : ['libwsrv'];
        $names = [];
        foreach ($kinds as $kind) {
            $names[$kind] = self::assetName($kind, $version, $arch);
        }
        if ($pin !== null && preg_match('/^[0-9a-fA-F]{64}\z/', $pin) !== 1) {
            throw new InstallException('--sha256 must be 64 hexadecimal digits');
        }

        $sums = [];
        if ($pin === null || $withDaemon) {
            $sums = self::parseSums(($this->fetch)(self::SUMS_NAME, self::MAX_SUMS_BYTES));
        }
        $verified = [];
        foreach ($names as $kind => $name) {
            $expected = $kind === 'libwsrv' && $pin !== null ? strtolower($pin) : ($sums[$name] ?? null);
            if ($expected === null) {
                throw new InstallException("$name is not listed in " . self::SUMS_NAME . '; refusing to install it');
            }
            $bytes = ($this->fetch)($name, self::MAX_LIBRARY_BYTES);
            if (strlen($bytes) > self::MAX_LIBRARY_BYTES || !hash_equals($expected, hash('sha256', $bytes))) {
                throw new InstallException("$name does not match its SHA-256; nothing was installed");
            }
            $verified[$kind] = $bytes;
        }

        if (!is_dir($dir) && !mkdir($dir, 0o755, true) && !is_dir($dir)) {
            throw new InstallException("cannot create $dir");
        }
        $written = [];
        foreach ($verified as $kind => $bytes) {
            $target = rtrim($dir, '/') . '/' . ($kind === 'libwsrv' ? 'libwsrv.so' : 'wsrvd');
            $written[$kind] = $this->writeAtomically($target, $bytes);
        }
        return $written;
    }

    private function writeAtomically(string $target, string $bytes): string
    {
        $temporary = $target . '.part-' . bin2hex(random_bytes(6));
        if (file_put_contents($temporary, $bytes, LOCK_EX) !== strlen($bytes) || !chmod($temporary, 0o755)) {
            @unlink($temporary);
            throw new InstallException("cannot write $target");
        }
        if (!rename($temporary, $target)) {
            @unlink($temporary);
            throw new InstallException("cannot replace $target");
        }
        return $target;
    }
}

<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

// ADR-0026: the helper that installs libwsrv.so. No network: the bytes come from a closure or a directory.

use Wsrv\Install\InstallException;
use Wsrv\Install\LibraryInstaller;
use Wsrv\Version;

/** A scratch directory that is removed when the test ends. */
function install_scratch(callable $body): void
{
    $dir = sys_get_temp_dir() . '/wsrv-install-' . bin2hex(random_bytes(6));
    mkdir($dir, 0o700, true);
    try {
        $body($dir);
    } finally {
        foreach (glob($dir . '/*') ?: [] as $file) {
            @unlink($file);
        }
        @rmdir($dir);
    }
}

/** A fetcher over an array of name => bytes; a missing name is an error, as a 404 would be. */
function install_fetcher(array $files, array &$requested = []): Closure
{
    return function (string $name, int $max) use ($files, &$requested): string {
        $requested[] = $name;
        return $files[$name] ?? throw new InstallException("cannot download $name");
    };
}

function install_release(string $version, string $arch, string $library, string $daemon): array
{
    $lib = LibraryInstaller::assetName('libwsrv', $version, $arch);
    $dae = LibraryInstaller::assetName('wsrvd', $version, $arch);
    return [
        $lib => $library,
        $dae => $daemon,
        'SHA256SUMS' => hash('sha256', $library) . "  $lib\n" . hash('sha256', $daemon) . "  $dae\n",
    ];
}

test('install: the package version is the project version in CMakeLists.txt', function () {
    $cmake = (string) file_get_contents(dirname(__DIR__, 3) . '/CMakeLists.txt');
    assertTrue(preg_match('/project\([^)]*VERSION\s+([0-9.]+)/s', $cmake, $m) === 1, 'found the project version');
    assertSame($m[1], Version::PACKAGE, 'update src/Version.php together with the project version');
});

test('install: asset names are built from a checked version and a fixed architecture list', function () {
    assertSame('libwsrv-1.2.3-linux-x86_64-glibc.so', LibraryInstaller::assetName('libwsrv', '1.2.3', 'x86_64'));
    assertSame('wsrvd-1.2.3-rc.1-linux-aarch64-glibc', LibraryInstaller::assetName('wsrvd', '1.2.3-rc.1', 'arm64'));
    foreach (['latest', '1.2', '../1.2.3', '1.2.3/../x', "1.2.3\n", '1.2.3-', '', '1.2.3 ', 'v1.2.3'] as $bad) {
        assertThrows(InstallException::class, fn () => LibraryInstaller::checkVersion($bad), 'version ' . show($bad));
    }
    assertThrows(InstallException::class, fn () => LibraryInstaller::architecture('riscv64'), 'no prebuilt library');
    assertThrows(InstallException::class, fn () => LibraryInstaller::assetName('evil', '1.2.3', 'x86_64'));
});

test('install: SHA256SUMS is parsed strictly', function () {
    $h = str_repeat('ab', 32);
    assertSame(['a.so' => $h, 'b' => $h], LibraryInstaller::parseSums("$h  a.so\n" . strtoupper($h) . " *b\n\n"));
    foreach (["$h a.so", substr($h, 1) . '  a.so', "$h  ../a.so", "$h  a.so\n$h  a.so", "$h  a b"] as $bad) {
        assertThrows(InstallException::class, fn () => LibraryInstaller::parseSums($bad), show($bad));
    }
});

test('install: a verified library is written executable, and nothing else is', function () {
    install_scratch(function (string $dir) {
        $files = install_release('1.2.3', 'x86_64', "LIB\x7fELF", 'DAEMON');
        $written = (new LibraryInstaller(install_fetcher($files)))->install('1.2.3', 'x86_64', "$dir/out");
        assertSame(["$dir/out/libwsrv.so"], array_values($written));
        assertSame("LIB\x7fELF", file_get_contents("$dir/out/libwsrv.so"));
        assertTrue(is_executable("$dir/out/libwsrv.so"), 'executable');
        assertSame(['libwsrv.so'], array_values(array_diff(scandir("$dir/out"), ['.', '..'])), 'no temporary file left');
    });
});

test('install: --daemon also installs wsrvd', function () {
    install_scratch(function (string $dir) {
        $files = install_release('1.2.3', 'aarch64', 'LIB', 'DAEMON');
        $written = (new LibraryInstaller(install_fetcher($files)))->install('1.2.3', 'aarch64', $dir, null, true);
        assertSame('DAEMON', file_get_contents($written['wsrvd']));
        assertSame('LIB', file_get_contents($written['libwsrv']));
    });
});

test('install: a library that does not match its checksum is refused and nothing is written', function () {
    install_scratch(function (string $dir) {
        $files = install_release('1.2.3', 'x86_64', 'LIB', 'DAEMON');
        $files[LibraryInstaller::assetName('libwsrv', '1.2.3', 'x86_64')] = 'TAMPERED';
        $e = assertThrows(InstallException::class, fn () => (new LibraryInstaller(install_fetcher($files)))->install('1.2.3', 'x86_64', "$dir/out"));
        assertTrue(str_contains($e->getMessage(), 'does not match'), $e->getMessage());
        assertTrue(!is_dir("$dir/out"), 'the directory was not even created');
    });
});

test('install: a daemon that fails its check stops the install before the library is written', function () {
    install_scratch(function (string $dir) {
        $files = install_release('1.2.3', 'x86_64', 'LIB', 'DAEMON');
        $files[LibraryInstaller::assetName('wsrvd', '1.2.3', 'x86_64')] = 'TAMPERED';
        assertThrows(InstallException::class, fn () => (new LibraryInstaller(install_fetcher($files)))->install('1.2.3', 'x86_64', "$dir/out", null, true));
        assertTrue(!file_exists("$dir/out/libwsrv.so"), 'all or nothing');
    });
});

test('install: a file missing from SHA256SUMS is refused, not trusted', function () {
    install_scratch(function (string $dir) {
        $files = install_release('1.2.3', 'x86_64', 'LIB', 'DAEMON');
        $files['SHA256SUMS'] = '';
        $e = assertThrows(InstallException::class, fn () => (new LibraryInstaller(install_fetcher($files)))->install('1.2.3', 'x86_64', $dir));
        assertTrue(str_contains($e->getMessage(), 'not listed'), $e->getMessage());
    });
});

test('install: a pinned hash replaces SHA256SUMS, and a wrong pin is refused', function () {
    install_scratch(function (string $dir) {
        $files = install_release('1.2.3', 'x86_64', 'LIB', 'DAEMON');
        $requested = [];
        (new LibraryInstaller(install_fetcher($files, $requested)))->install('1.2.3', 'x86_64', $dir, strtoupper(hash('sha256', 'LIB')));
        assertSame(['libwsrv-1.2.3-linux-x86_64-glibc.so'], $requested, 'SHA256SUMS was not even fetched');
        assertThrows(InstallException::class, fn () => (new LibraryInstaller(install_fetcher($files)))->install('1.2.3', 'x86_64', $dir, str_repeat('0', 64)), 'wrong pin');
        assertThrows(InstallException::class, fn () => (new LibraryInstaller(install_fetcher($files)))->install('1.2.3', 'x86_64', $dir, 'not-a-hash'), 'malformed pin');
    });
});

test('install: the command works offline from a directory and says how to use the library', function () {
    install_scratch(function (string $dir) {
        $version = Version::PACKAGE;
        $arch = LibraryInstaller::architecture(php_uname('m'));
        mkdir("$dir/dl");
        foreach (install_release($version, $arch, 'LIB', 'DAEMON') as $name => $bytes) {
            file_put_contents("$dir/dl/$name", $bytes);
        }
        $cmd = escapeshellarg(PHP_BINARY) . ' ' . escapeshellarg(dirname(__DIR__) . '/bin/wsrv-install-lib')
            . ' --from-dir=' . escapeshellarg("$dir/dl") . ' --dir=' . escapeshellarg("$dir/lib") . ' 2>&1';
        exec($cmd, $lines, $status);
        assertSame(0, $status, implode("\n", $lines));
        assertTrue(str_contains(implode("\n", $lines), 'export WSRV_LIBRARY='), implode("\n", $lines));
        assertSame('LIB', file_get_contents("$dir/lib/libwsrv.so"));
        // and a tampered download fails the command, with a message, not a stack trace
        file_put_contents("$dir/dl/" . LibraryInstaller::assetName('libwsrv', $version, $arch), 'EVIL');
        exec($cmd, $again, $status2);
        assertSame(1, $status2, implode("\n", $again));
        assertTrue(str_contains(implode("\n", $again), 'does not match'), implode("\n", $again));
        @unlink("$dir/dl/SHA256SUMS");
        foreach (glob("$dir/dl/*") ?: [] as $f) {
            @unlink($f);
        }
        @rmdir("$dir/dl");
        @unlink("$dir/lib/libwsrv.so");
        @rmdir("$dir/lib");
    });
});

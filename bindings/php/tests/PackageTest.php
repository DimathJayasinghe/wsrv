<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

// ADR-0026 section 1: the root composer.json is what Packagist reads; bindings/php/composer.json is what a
// developer uses inside the binding. They must not drift, and the Composer archive must not carry the C++ tree.

function package_json(string $path): array
{
    $text = is_file($path) ? (string) file_get_contents($path) : fail("missing $path");
    $data = json_decode($text, true);
    return is_array($data) ? $data : fail("$path is not valid JSON");
}

test('package: the root and the binding composer.json agree on what they require and suggest', function () {
    $root = package_json(dirname(__DIR__, 3) . '/composer.json');
    $local = package_json(dirname(__DIR__) . '/composer.json');
    foreach (['name', 'license', 'require', 'suggest', 'type'] as $key) {
        assertSame($local[$key] ?? null, $root[$key] ?? null, "composer.json key '$key'");
    }
});

test('package: the root autoload and bin point at files that exist', function () {
    $repository = dirname(__DIR__, 3);
    $root = package_json($repository . '/composer.json');
    $local = package_json(dirname(__DIR__) . '/composer.json');
    $dir = $root['autoload']['psr-4']['Wsrv\\'] ?? fail('no psr-4 entry for Wsrv\\');
    assertSame('bindings/php/' . $local['autoload']['psr-4']['Wsrv\\'], $dir, 'same source directory, seen from the root');
    assertTrue(is_file($repository . '/' . $dir . 'Client.php'), 'the autoload directory has the classes');
    foreach ($root['bin'] ?? [] as $bin) {
        assertTrue(is_file($repository . '/' . $bin), "bin $bin exists");
        assertTrue(is_executable($repository . '/' . $bin), "bin $bin is executable");
    }
    assertSame(['bindings/php/bin/wsrv-install-lib'], $root['bin'] ?? null);
    assertSame(['bin/wsrv-install-lib'], $local['bin'] ?? null);
});

test('package: every top-level entry is either in the package or export-ignored', function () {
    $repository = dirname(__DIR__, 3);
    $attributes = (string) file_get_contents($repository . '/.gitattributes');
    preg_match_all('#^/(\S+) export-ignore$#m', $attributes, $m);
    $ignored = $m[1];
    $kept = ['bindings', 'LICENSE', 'NOTICE', 'CHANGELOG.md', 'README.md', 'composer.json'];
    // Only tracked entries end up in the archive; untracked local files (tooling, build output) do not matter.
    is_dir($repository . '/.git') || skip('not a git checkout');
    exec('git -C ' . escapeshellarg($repository) . ' ls-files', $files, $status);
    assertSame(0, $status, 'git ls-files');
    $tracked = array_values(array_unique(array_map(static fn (string $f): string => explode('/', $f)[0], $files)));
    foreach ($tracked as $entry) {
        assertTrue(in_array($entry, $kept, true) || in_array($entry, $ignored, true), "$entry is in the package or listed in .gitattributes");
    }
    foreach ($ignored as $path) {
        assertTrue(file_exists($repository . '/' . $path), "export-ignore $path names something that exists");
    }
    assertSame(['php'], array_values(array_diff(scandir($repository . '/bindings') ?: [], ['.', '..'])), 'bindings/ holds only the PHP binding');
});

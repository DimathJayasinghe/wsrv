<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

// Every PHP file in the package must parse (src, tests and the examples, which nothing else runs).
test('lint: every PHP file in the package parses', function () {
    $root = dirname(__DIR__);
    $files = new RecursiveIteratorIterator(new RecursiveDirectoryIterator($root, FilesystemIterator::SKIP_DOTS));
    $count = 0;
    foreach ($files as $file) {
        if ($file->getExtension() !== 'php' || str_contains($file->getPathname(), '/vendor/')) {
            continue;
        }
        $count++;
        try {
            token_get_all((string) file_get_contents($file->getPathname()), TOKEN_PARSE);
        } catch (ParseError $e) {
            fail($file->getPathname() . ':' . $e->getLine() . ': ' . $e->getMessage());
        }
    }
    assertTrue($count >= 15, 'found the package files (' . $count . ')');
});

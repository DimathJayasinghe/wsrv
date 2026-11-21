<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

// ADR-0025 section 2: the FFI declarations are the C header minus what FFI cannot read. If the
// header changes and the cdef does not, these tests fail; nothing is left to a crash at run time.

require_once __DIR__ . '/../tools/CdefGenerator.php';

use Wsrv\Tools\CdefGenerator;

function wsrv_header(): string
{
    $path = dirname(__DIR__, 3) . '/include/wsrv/wsrv.h';
    return is_file($path) ? (string) file_get_contents($path) : fail("header not found at $path");
}

test('cdef: src/Embedded/wsrv.cdef is exactly what the generator makes from include/wsrv/wsrv.h', function () {
    $expected = CdefGenerator::fromHeader(wsrv_header());
    $actual = (string) file_get_contents(dirname(__DIR__) . '/src/Embedded/wsrv.cdef');
    assertSame($expected, $actual, 'run: php bindings/php/tools/gen_cdef.php to refresh the cdef');
});

test('cdef: every WSRV_API function of the header is declared', function () {
    preg_match_all('/WSRV_API\s+[\w\s\*]+?\b(wsrv_\w+)\s*\(/', preg_replace('/^#.*$/m', '', wsrv_header()), $m);
    assertTrue(count($m[1]) >= 18, 'found the header functions (' . count($m[1]) . ')');
    $cdef = (string) file_get_contents(dirname(__DIR__) . '/src/Embedded/wsrv.cdef');
    foreach ($m[1] as $name) {
        assertTrue(preg_match('/\b' . $name . '\s*\(/', $cdef) === 1, "$name is in the cdef");
    }
});

test('cdef: the generator removes what FFI cannot parse and keeps what it needs', function () {
    $out = CdefGenerator::fromHeader(
        "#ifndef X\n#define X\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n"
        . "/* doc */\nWSRV_API int wsrv_a(void); /* tail */\ntypedef struct s { int v; } s;\n"
        . "#ifdef __cplusplus\n}\n#endif\n#endif\n"
    );
    assertSame("int wsrv_a(void);\ntypedef struct s { int v; } s;\n", $out);
});

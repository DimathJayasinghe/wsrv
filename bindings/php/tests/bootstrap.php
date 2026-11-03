<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

// A PSR-4 autoloader for Wsrv\ so the tests run without Composer.
spl_autoload_register(static function (string $class): void {
    $prefix = 'Wsrv\\';
    if (strncmp($class, $prefix, strlen($prefix)) !== 0) {
        return;
    }
    $file = __DIR__ . '/../src/' . str_replace('\\', '/', substr($class, strlen($prefix))) . '.php';
    if (is_file($file)) {
        require $file;
    }
});

// ---- a tiny test harness (no PHPUnit needed) -------------------------------------------------

/** @var array<string, callable> $GLOBALS['wsrv_tests'] */
$GLOBALS['wsrv_tests'] = [];

function test(string $name, callable $body): void
{
    $GLOBALS['wsrv_tests'][$name] = $body;
}

function fail(string $message): never
{
    throw new AssertionError($message);
}

/** A value for a failure message: long strings are cut and shown as hex so binary cannot flood a log. */
function show(mixed $value): string
{
    if (is_string($value) && (strlen($value) > 64 || preg_match('//u', $value) !== 1)) {
        return 'string(' . strlen($value) . ') 0x' . bin2hex(substr($value, 0, 32)) . (strlen($value) > 32 ? '...' : '');
    }
    if (is_array($value)) {
        return '[' . implode(', ', array_map(show(...), $value)) . ']';
    }
    return var_export($value, true);
}

function assertSame(mixed $expected, mixed $actual, string $what = ''): void
{
    if ($expected !== $actual) {
        fail(($what !== '' ? $what . ': ' : '') . 'expected ' . show($expected) . ', got ' . show($actual));
    }
}

function assertTrue(bool $condition, string $what = 'condition'): void
{
    if (!$condition) {
        fail($what . ' is false');
    }
}

/** @param class-string<Throwable> $class */
function assertThrows(string $class, callable $body, string $what = ''): Throwable
{
    try {
        $body();
    } catch (Throwable $e) {
        if (!($e instanceof $class)) {
            fail(($what !== '' ? $what . ': ' : '') . "expected $class, got " . get_class($e) . ': ' . $e->getMessage());
        }
        return $e;
    }
    fail(($what !== '' ? $what . ': ' : '') . "expected $class, nothing was thrown");
}

/** Hex string to bytes: hex('00 00 00 02 06 00'). */
function hex(string $spaced): string
{
    $bytes = hex2bin(str_replace(' ', '', $spaced));
    return $bytes === false ? fail('bad hex') : $bytes;
}

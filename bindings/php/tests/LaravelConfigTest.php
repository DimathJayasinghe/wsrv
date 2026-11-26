<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

use Wsrv\Laravel\ConnectionConfig;

function laravel_config(array $override = []): array
{
    return array_replace([
        'driver' => 'wsrv', 'key' => 'app-key', 'secret' => 'a-long-enough-app-secret-0123456789',
        'publish_url' => 'http://127.0.0.1:9200', 'publish_token' => 'wsrv1_' . str_repeat('a', 64) . '_00000000',
    ], $override);
}

test('laravel config: a complete connection builds the gateway, the publisher and no encryption', function () {
    $c = ConnectionConfig::fromArray(laravel_config(['options' => ['max_presence_members' => 5, 'client_events' => false]]));
    assertSame(5, $c->gateway()->maxPresenceMembers);
    assertSame(false, $c->gateway()->clientEvents);
    assertSame(null, $c->encryption());
    assertSame('/run/wsrv/ipc.sock', $c->ipcEndpoint());
    assertSame('', $c->ipcToken());
    assertSame(false, $c->checkAppKey());
});

test('laravel config: a typo in a key or an option is an error, not a silent default', function () {
    assertThrows(InvalidArgumentException::class, fn () => ConnectionConfig::fromArray(laravel_config(['publish_utl' => 'x'])));
    assertThrows(InvalidArgumentException::class, fn () => ConnectionConfig::fromArray(laravel_config(['options' => ['max_presence_member' => 5]])));
    assertThrows(InvalidArgumentException::class, fn () => ConnectionConfig::fromArray(laravel_config(['options' => ['max_presence_members' => '5']])));
    assertThrows(InvalidArgumentException::class, fn () => ConnectionConfig::fromArray(laravel_config(['options' => ['client_events' => 1]])));
});

test('laravel config: missing or weak credentials are refused, and the message never contains the secret', function () {
    assertThrows(InvalidArgumentException::class, fn () => ConnectionConfig::fromArray(laravel_config(['key' => ''])));
    assertThrows(InvalidArgumentException::class, fn () => ConnectionConfig::fromArray(laravel_config(['secret' => null])));
    $weak = 'weak-secret';
    try {
        ConnectionConfig::fromArray(laravel_config(['secret' => $weak]));
        fail('a weak secret was accepted');
    } catch (InvalidArgumentException $e) {
        assertTrue(!str_contains($e->getMessage() . $e->getTraceAsString(), $weak), 'the secret leaked');
    }
    $noToken = laravel_config();
    unset($noToken['publish_token']);
    assertThrows(InvalidArgumentException::class, fn () => ConnectionConfig::fromArray($noToken));
});

test('laravel config: a token comes from a file (trimmed), and not from both places', function () {
    $file = tempnam(sys_get_temp_dir(), 'wsrv-token');
    file_put_contents($file, "  tok-from-file\n");
    $noInline = laravel_config(['publish_token_file' => $file]);
    unset($noInline['publish_token']);
    $c = ConnectionConfig::fromArray($noInline);
    assertTrue($c->publisher() instanceof Wsrv\Publisher);
    assertThrows(InvalidArgumentException::class, fn () => ConnectionConfig::fromArray(laravel_config(['publish_token_file' => $file])), 'both');
    $noInline['publish_token_file'] = $file . '.missing';
    assertThrows(InvalidArgumentException::class, fn () => ConnectionConfig::fromArray($noInline), 'unreadable');
    unlink($file);
});

test('laravel config: the encryption key is validated when the connection is read', function () {
    $good = ConnectionConfig::fromArray(laravel_config(['encryption_master_key_base64' => base64_encode(random_bytes(32))]));
    assertTrue($good->encryption() !== null);
    assertThrows(InvalidArgumentException::class, fn () => ConnectionConfig::fromArray(laravel_config(['encryption_master_key_base64' => 'nope'])));
});

test('laravel config: the publish endpoint must be plain http and the timeout positive', function () {
    assertThrows(InvalidArgumentException::class, fn () => ConnectionConfig::fromArray(laravel_config(['publish_url' => 'https://x:9200'])));
    assertThrows(InvalidArgumentException::class, fn () => ConnectionConfig::fromArray(laravel_config(['publish_timeout' => 0])));
});

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
    // the defaults carry a page that joins all 64 of its channels in one burst
    assertSame(64, $c->gateway()->maxChannelsPerConnection);
    assertSame(128, $c->gateway()->messageBurst);
    assertSame('/run/wsrv/ipc.sock', $c->ipcEndpoint());
    assertSame('', $c->ipcToken());
    assertSame(false, $c->checkAppKey());
});

test('laravel config: a typo in a key or an option is an error, not a silent default', function () {
    assertThrows(InvalidArgumentException::class, fn () => ConnectionConfig::fromArray(laravel_config(['publish_utl' => 'x'])));
    assertThrows(InvalidArgumentException::class, fn () => ConnectionConfig::fromArray(laravel_config(['options' => ['max_presence_member' => 5]])));
    assertThrows(InvalidArgumentException::class, fn () => ConnectionConfig::fromArray(laravel_config(['options' => ['max_presence_members' => '5']])));
    assertThrows(InvalidArgumentException::class, fn () => ConnectionConfig::fromArray(laravel_config(['options' => ['client_events' => 1]])));
    // an option that was removed before release (the quota bounds joins in progress) is named, not ignored
    assertThrows(InvalidArgumentException::class, fn () => ConnectionConfig::fromArray(laravel_config(['options' => ['max_pending_per_connection' => 8]])));
});

test('laravel config: the message burst is an option, and cannot be smaller than the channel quota allows', function () {
    $c = ConnectionConfig::fromArray(laravel_config(['options' => ['max_channels_per_connection' => 10, 'message_burst' => 40, 'messages_per_second' => 5]]));
    assertSame(40, $c->gateway()->messageBurst);
    assertSame(5, $c->gateway()->messagesPerSecond);
    assertSame(74, ConnectionConfig::fromArray(laravel_config(['options' => ['max_channels_per_connection' => 10]]))->gateway()->messageBurst);
    assertThrows(InvalidArgumentException::class, fn () => ConnectionConfig::fromArray(laravel_config(['options' => ['max_channels_per_connection' => 64, 'message_burst' => 70]]))->gateway());
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
    function_exists('sodium_crypto_secretbox') || skip('needs ext-sodium (not enabled on every Windows PHP)');
    $good = ConnectionConfig::fromArray(laravel_config(['encryption_master_key_base64' => base64_encode(random_bytes(32))]));
    assertTrue($good->encryption() !== null);
    assertThrows(InvalidArgumentException::class, fn () => ConnectionConfig::fromArray(laravel_config(['encryption_master_key_base64' => 'nope'])));
});

test('laravel config: the publish endpoint must be plain http and the timeout positive', function () {
    assertThrows(InvalidArgumentException::class, fn () => ConnectionConfig::fromArray(laravel_config(['publish_url' => 'https://x:9200'])));
    assertThrows(InvalidArgumentException::class, fn () => ConnectionConfig::fromArray(laravel_config(['publish_timeout' => 0])));
});

test('laravel config: the statistics interval of the wsrv:echo command is whole seconds, 60 by default, 0 for off', function () {
    assertSame(60, ConnectionConfig::statsInterval(null));
    assertSame(60, ConnectionConfig::statsInterval('60'));
    assertSame(0, ConnectionConfig::statsInterval('0'));
    assertSame(5, ConnectionConfig::statsInterval(5));
    assertSame(86400, ConnectionConfig::statsInterval('86400'));
    foreach (['-1', '1.5', '1e3', ' 5', '5 ', '', 'soon', '86401', '99999999999999999999', 1.5, true, []] as $bad) {
        assertThrows(InvalidArgumentException::class, fn () => ConnectionConfig::statsInterval($bad), 'accepted ' . var_export($bad, true));
    }
});

test('laravel config: public_channels and max_public_channels reach the gateway, and a wrong type is refused (ADR-0042 A3)', function () {
    $g = ConnectionConfig::fromArray(laravel_config(['options' => ['public_channels' => ['news', 'orders.*'], 'max_public_channels' => 100]]))->gateway();
    assertSame(['news', 'orders.*'], $g->publicChannels);
    assertSame(100, $g->maxPublicChannels);
    $g = ConnectionConfig::fromArray(laravel_config(['options' => ['public_channels' => null]]))->gateway();
    assertSame(null, $g->publicChannels, 'null is the same as leaving it out: any public channel');
    $g = ConnectionConfig::fromArray(laravel_config(['options' => ['public_channels' => []]]))->gateway();
    assertSame([], $g->publicChannels, 'an empty list is none');
    assertSame(512, ConnectionConfig::fromArray(laravel_config())->gateway()->maxPublicChannels);
    foreach (['news', 5, true, ['x' => 'news'], ['news', 5], [['a']], ['private-x'], ['a*b']] as $bad) {
        assertThrows(InvalidArgumentException::class, fn () => ConnectionConfig::fromArray(laravel_config(['options' => ['public_channels' => $bad]])), json_encode($bad));
    }
    assertThrows(InvalidArgumentException::class, fn () => ConnectionConfig::fromArray(laravel_config(['options' => ['max_public_channels' => '5']])));
    assertThrows(InvalidArgumentException::class, fn () => ConnectionConfig::fromArray(laravel_config(['options' => ['max_public_channels' => 0]])));
});

<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv\Laravel;

use Wsrv\Publisher;
use Wsrv\Pusher\EncryptedChannels;
use Wsrv\Pusher\GatewayConfig;

/**
 * The `wsrv` entry of config/broadcasting.php, checked and turned into the objects the broadcaster
 * and the `wsrv:echo` command need. Plain PHP: it does not need Laravel, so it is tested without it.
 *
 *     'wsrv' => [
 *         'driver' => 'wsrv',
 *         'key' => env('WSRV_APP_KEY'),                  // what Echo is given as `key`
 *         'secret' => env('WSRV_APP_SECRET'),            // at least 16 bytes; never sent to a browser
 *         'publish_url' => env('WSRV_PUBLISH_URL', 'http://127.0.0.1:9200'),
 *         'publish_token_file' => env('WSRV_PUBLISH_TOKEN_FILE'),   // or 'publish_token'
 *         'ipc_endpoint' => env('WSRV_IPC', '/run/wsrv/ipc.sock'),  // or tcp://host:port
 *         'ipc_token_file' => env('WSRV_IPC_TOKEN_FILE'),           // only for a tcp:// endpoint
 *         'encryption_master_key_base64' => env('WSRV_ENCRYPTION_KEY'),  // only for private-encrypted-*
 *         'options' => ['max_presence_members' => 100, 'client_events' => true],
 *     ],
 *
 * An unknown key anywhere is an error: a misspelled limit must not silently fall back to a default.
 */
final class ConnectionConfig
{
    private const KEYS = [
        'driver', 'key', 'secret', 'publish_url', 'publish_token', 'publish_token_file', 'publish_timeout',
        'ipc_endpoint', 'ipc_token', 'ipc_token_file', 'encryption_master_key_base64', 'check_app_key', 'options',
    ];

    /** option name => [GatewayConfig parameter, type] */
    private const OPTIONS = [
        'activity_timeout' => ['activityTimeout', 'int'],
        'max_frame_bytes' => ['maxFrameBytes', 'int'],
        'max_channels_per_connection' => ['maxChannelsPerConnection', 'int'],
        'max_presence_members' => ['maxPresenceMembers', 'int'],
        'max_channel_data_bytes' => ['maxChannelDataBytes', 'int'],
        'client_events' => ['clientEvents', 'bool'],
        'client_events_per_second' => ['clientEventsPerSecond', 'int'],
        'messages_per_second' => ['messagesPerSecond', 'int'],
        'message_burst' => ['messageBurst', 'int'],
        'max_strikes' => ['maxStrikes', 'int'],
    ];

    /** The seconds between statistics lines when `wsrv:echo` is not told otherwise. */
    public const DEFAULT_STATS_INTERVAL = 60;
    private const MAX_STATS_INTERVAL = 86400;

    /** @param array<string, mixed> $config */
    private function __construct(private readonly array $config)
    {
    }

    /**
     * The value of the `--stats-interval` option of `wsrv:echo` (ADR-0042 A2): whole seconds, 0 for off.
     * It comes from a command line, so a string; anything but plain digits is an error, not a default.
     *
     * @throws \InvalidArgumentException
     */
    public static function statsInterval(mixed $option): int
    {
        if ($option === null) {
            return self::DEFAULT_STATS_INTERVAL;
        }
        if (is_string($option) && preg_match('/\A[0-9]{1,6}\z/', $option) === 1) {
            $option = (int) $option;
        }
        if (!is_int($option) || $option < 0 || $option > self::MAX_STATS_INTERVAL) {
            throw new \InvalidArgumentException('--stats-interval must be a whole number of seconds from 0 (off) to ' . self::MAX_STATS_INTERVAL);
        }
        return $option;
    }

    /**
     * @param array<string, mixed> $config
     * @throws \InvalidArgumentException naming the key at fault, never printing a secret
     */
    public static function fromArray(#[\SensitiveParameter] array $config): self
    {
        foreach (array_keys($config) as $name) {
            if (!in_array($name, self::KEYS, true)) {
                throw new \InvalidArgumentException("unknown key '$name' in the wsrv broadcasting connection");
            }
        }
        $options = $config['options'] ?? [];
        if (!is_array($options)) {
            throw new \InvalidArgumentException("'options' must be an array");
        }
        foreach ($options as $name => $value) {
            [, $type] = self::OPTIONS[$name] ?? throw new \InvalidArgumentException("unknown option '$name' in the wsrv broadcasting connection");
            if (($type === 'int' && !is_int($value)) || ($type === 'bool' && !is_bool($value))) {
                throw new \InvalidArgumentException("option '$name' must be of type $type");
            }
        }
        foreach (['key', 'secret'] as $required) {
            if (!isset($config[$required]) || !is_string($config[$required]) || $config[$required] === '') {
                throw new \InvalidArgumentException("'$required' is not set in the wsrv broadcasting connection");
            }
        }
        $self = new self($config);
        $self->gateway();                       // validate everything now, not at the first request
        $self->publisher();
        $self->encryption();
        return $self;
    }

    public function gateway(): GatewayConfig
    {
        $args = [];
        foreach ((array) ($this->config['options'] ?? []) as $name => $value) {
            $args[self::OPTIONS[$name][0]] = $value;
        }
        return new GatewayConfig($this->config['key'], $this->config['secret'], ...$args);
    }

    public function publisher(): Publisher
    {
        $timeout = $this->config['publish_timeout'] ?? 2.0;
        if (!is_int($timeout) && !is_float($timeout) || $timeout <= 0) {
            throw new \InvalidArgumentException("'publish_timeout' must be a number of seconds above zero");
        }
        return new Publisher(
            (string) ($this->config['publish_url'] ?? 'http://127.0.0.1:9200'),
            $this->secretFrom('publish_token', 'publish_token_file', true),
            (float) $timeout,
        );
    }

    public function encryption(): ?EncryptedChannels
    {
        $key = $this->config['encryption_master_key_base64'] ?? null;
        if ($key === null || $key === '') {
            return null;
        }
        if (!is_string($key)) {
            throw new \InvalidArgumentException("'encryption_master_key_base64' must be a string");
        }
        return EncryptedChannels::fromBase64($key);
    }

    /** The IPC endpoint of the daemon: a socket path, or tcp://host:port. */
    public function ipcEndpoint(): string
    {
        $endpoint = $this->config['ipc_endpoint'] ?? '/run/wsrv/ipc.sock';
        if (!is_string($endpoint) || $endpoint === '') {
            throw new \InvalidArgumentException("'ipc_endpoint' must be a socket path or tcp://host:port");
        }
        return $endpoint;
    }

    public function ipcToken(): string
    {
        return $this->secretFrom('ipc_token', 'ipc_token_file', false);
    }

    /** Whether the daemon runs with handshake_hook, so the worker can refuse a URL with another app key. */
    public function checkAppKey(): bool
    {
        $value = $this->config['check_app_key'] ?? false;
        if (!is_bool($value)) {
            throw new \InvalidArgumentException("'check_app_key' must be true or false");
        }
        return $value;
    }

    private function secretFrom(string $inline, string $file, bool $required): string
    {
        $value = $this->config[$inline] ?? null;
        $path = $this->config[$file] ?? null;
        if ($value !== null && $value !== '' && $path !== null && $path !== '') {
            throw new \InvalidArgumentException("set '$inline' or '$file', not both");
        }
        if (is_string($path) && $path !== '') {
            $text = @file_get_contents($path);
            if ($text === false) {
                throw new \InvalidArgumentException("cannot read the file named by '$file'");
            }
            $value = trim($text);
        }
        if ($value === null || $value === '') {
            if ($required) {
                throw new \InvalidArgumentException("neither '$inline' nor '$file' is set in the wsrv broadcasting connection");
            }
            return '';
        }
        if (!is_string($value)) {
            throw new \InvalidArgumentException("'$inline' must be a string");
        }
        return $value;
    }
}

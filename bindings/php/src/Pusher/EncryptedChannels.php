<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv\Pusher;

/**
 * End-to-end encrypted channels (`private-encrypted-*`), as Laravel Echo and pusher-js use them: the
 * server encrypts each event for the channel with a key only the application and its authorized
 * browsers can derive, so the daemon, the worker and anyone on the wire only see ciphertext.
 *
 * The scheme is Pusher's (libsodium secretbox, XSalsa20-Poly1305), so that pusher-js can decrypt:
 *   shared secret = SHA-256(channel name . master key), 32 raw bytes
 *   event data    = {"nonce": base64(24 random bytes), "ciphertext": base64(secretbox(JSON of the payload))}
 *   the auth answer for the channel carries base64(shared secret) as `shared_secret`.
 *
 * The master key (32 random bytes, base64 in the config) never leaves the application server.
 * Needs ext-sodium, which is part of PHP since 7.2.
 */
final class EncryptedChannels
{
    public const PREFIX = 'private-encrypted-';

    /** @param string $masterKey 32 raw bytes */
    private function __construct(#[\SensitiveParameter] private readonly string $masterKey)
    {
    }

    /** @throws \InvalidArgumentException if the key is not base64 of 32 bytes, or sodium is missing */
    public static function fromBase64(#[\SensitiveParameter] string $base64): self
    {
        if (!function_exists('sodium_crypto_secretbox')) {
            throw new \InvalidArgumentException('encrypted channels need ext-sodium');
        }
        $raw = base64_decode($base64, true);
        if ($raw === false || strlen($raw) !== SODIUM_CRYPTO_SECRETBOX_KEYBYTES) {
            throw new \InvalidArgumentException('the encryption master key must be base64 of exactly 32 bytes (php -r \'echo base64_encode(random_bytes(32));\')');
        }
        return new self($raw);
    }

    public static function isEncrypted(string $channel): bool
    {
        return str_starts_with($channel, self::PREFIX);
    }

    /** What the auth endpoint adds to its answer for an encrypted channel. */
    public function sharedSecretBase64(string $channel): string
    {
        return base64_encode($this->sharedSecret($channel));
    }

    /**
     * The `data` of an event for $channel: the payload, JSON encoded, then encrypted.
     *
     * @param string $json the JSON text of the payload
     * @return array{nonce: string, ciphertext: string}
     */
    public function encrypt(string $channel, string $json): array
    {
        $this->require($channel);
        $nonce = random_bytes(SODIUM_CRYPTO_SECRETBOX_NONCEBYTES);
        return [
            'nonce' => base64_encode($nonce),
            'ciphertext' => base64_encode(sodium_crypto_secretbox($json, $nonce, $this->sharedSecret($channel))),
        ];
    }

    /** The inverse of encrypt(), for tests and tools; null if it does not authenticate. */
    public function decrypt(string $channel, string $nonceBase64, string $ciphertextBase64): ?string
    {
        $this->require($channel);
        $nonce = base64_decode($nonceBase64, true);
        $cipher = base64_decode($ciphertextBase64, true);
        if ($nonce === false || $cipher === false || strlen($nonce) !== SODIUM_CRYPTO_SECRETBOX_NONCEBYTES) {
            return null;
        }
        $plain = sodium_crypto_secretbox_open($cipher, $nonce, $this->sharedSecret($channel));
        return $plain === false ? null : $plain;
    }

    public function __debugInfo(): array
    {
        return ['masterKey' => '(hidden)'];
    }

    public function __serialize(): array
    {
        throw new \LogicException('EncryptedChannels holds the master key and cannot be serialized');
    }

    private function sharedSecret(string $channel): string
    {
        $this->require($channel);
        return hash('sha256', $channel . $this->masterKey, true);
    }

    private function require(string $channel): void
    {
        if (!self::isEncrypted($channel)) {
            throw new \InvalidArgumentException("not an encrypted channel: $channel");
        }
    }
}

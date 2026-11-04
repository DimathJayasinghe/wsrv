<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv\Ipc;

use Wsrv\ProtocolError;

/**
 * What a browser asked for when it opened a WebSocket (ADR-0018): the data of a Handshake event.
 * Every value comes straight from the client, so treat it as untrusted input.
 *
 * `headers` holds only what the daemon was told to forward (`handshake_forward_header`), in request
 * order, every value of a repeated header as its own entry. Cookie and Authorization are never
 * here unless that setting lists them.
 */
final class HandshakeRequest
{
    /**
     * @param list<string> $offered subprotocols the client offered, in order
     * @param list<array{0:string,1:string}> $headers [name, value] pairs, name spelled as configured
     */
    public function __construct(
        public readonly string $peer,
        public readonly string $path,
        public readonly string $query,
        public readonly string $origin,
        public readonly string $host,
        public readonly string $subprotocol,
        public readonly array $offered,
        public readonly array $headers,
    ) {
    }

    /**
     * Read the blob of docs/IPC_PROTOCOL.md section 9. Strict: a length beyond the data, a missing
     * part or a trailing byte is an error, never a guess.
     *
     * @throws ProtocolError
     */
    public static function parse(string $blob): self
    {
        $at = 0;
        $str = static function () use ($blob, &$at): string {
            if (strlen($blob) - $at < 4) {
                throw new ProtocolError('handshake blob is truncated');
            }
            /** @var array{1:int} $length */
            $length = unpack('N', $blob, $at);
            $at += 4;
            if ($length[1] > strlen($blob) - $at) {
                throw new ProtocolError('handshake blob declares a string beyond its end');
            }
            $value = substr($blob, $at, $length[1]);
            $at += $length[1];
            return $value;
        };
        $count = static function () use ($blob, &$at): int {
            if (strlen($blob) - $at < 2) {
                throw new ProtocolError('handshake blob is truncated');
            }
            /** @var array{1:int} $n */
            $n = unpack('n', $blob, $at);
            $at += 2;
            return $n[1];
        };

        $peer = $str();
        $path = $str();
        $query = $str();
        $origin = $str();
        $host = $str();
        $subprotocol = $str();
        $offered = [];
        for ($i = $count(); $i > 0; $i--) {
            $offered[] = $str();
        }
        $headers = [];
        for ($i = $count(); $i > 0; $i--) {
            $name = $str();
            $headers[] = [$name, $str()];
        }
        if ($at !== strlen($blob)) {
            throw new ProtocolError('handshake blob has trailing bytes');
        }
        return new self($peer, $path, $query, $origin, $host, $subprotocol, $offered, $headers);
    }

    /** Every value of a forwarded header, in request order; the name is matched without case. @return list<string> */
    public function headers(string $name): array
    {
        $values = [];
        foreach ($this->headers as [$headerName, $value]) {
            if (strcasecmp($headerName, $name) === 0) {
                $values[] = $value;
            }
        }
        return $values;
    }

    /** The first value of a forwarded header, or null. */
    public function header(string $name): ?string
    {
        return $this->headers($name)[0] ?? null;
    }

    /**
     * The value of cookie $name from the forwarded Cookie headers (RFC 6265 section 5.4: pairs
     * separated by ';'), as sent: not decoded, quotes kept. The first match wins. Null if absent,
     * which includes the case where Cookie was not listed in `handshake_forward_header`.
     */
    public function cookie(string $name): ?string
    {
        foreach ($this->headers('Cookie') as $line) {
            foreach (explode(';', $line) as $pair) {
                $pair = trim($pair, " \t");
                $equals = strpos($pair, '=');
                if ($equals !== false && substr($pair, 0, $equals) === $name) {
                    return substr($pair, $equals + 1);
                }
            }
        }
        return null;
    }
}

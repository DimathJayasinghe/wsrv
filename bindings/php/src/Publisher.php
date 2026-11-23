<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv;

/**
 * Publish to a room from any PHP process (a controller, a queued job, a cron script) over the
 * daemon's HTTP publish endpoint (ADR-0028, docs/OPERATIONS.md 3.1.1). No extension and no
 * long-lived connection: one short request per call.
 *
 *     $publisher = new Publisher('http://wsrvd:9200', trim(file_get_contents('/run/secrets/wsrv_publish')));
 *     $result = $publisher->publish('room.7', json_encode(['text' => 'hi']), except: $connId);
 *     // $result->subscribers, ->accepted, ->refused
 *
 * Only `http://` is supported: the token travels in clear text, so keep the daemon on a private
 * network or behind a TLS proxy (then the proxy, not this class, speaks https). A 200 is returned as
 * a PublishResult; anything else throws PublishFailed, whose isRetryable() separates "try later"
 * (429, 503, unreachable) from a mistake in the call.
 */
final class Publisher
{
    private const MAX_RESPONSE = 4096;

    private readonly string $host;
    private readonly int $port;

    /**
     * @param string $baseUrl http://host[:port] of the publish endpoint (no path)
     * @param string $token   the contents of the daemon's publish token file
     * @param float  $timeout seconds to connect, and for each read or write after that
     * @param (callable(string, string, int, float): string)|null $transport for tests: (host, request, port, timeout) => raw response
     */
    public function __construct(
        string $baseUrl,
        private readonly string $token,
        private readonly float $timeout = 5.0,
        private readonly mixed $transport = null,
    ) {
        $parts = parse_url($baseUrl);
        if ($parts === false || ($parts['scheme'] ?? '') !== 'http' || !isset($parts['host'])
            || isset($parts['user']) || isset($parts['query']) || isset($parts['fragment'])
            || (isset($parts['path']) && $parts['path'] !== '' && $parts['path'] !== '/')) {
            throw new \InvalidArgumentException(
                "the publish URL must look like http://host:port (https is not supported: put TLS in a proxy), got '$baseUrl'"
            );
        }
        $this->host = $parts['host'];
        $this->port = $parts['port'] ?? 80;
        if ($token === '' || preg_match('/[\x00-\x20\x7f]/', $token) === 1) {
            throw new \InvalidArgumentException('the publish token is empty or has spaces or control characters (trim the file contents)');
        }
    }

    /**
     * @param string $topic   letters, digits and . _ : / @ - (the broker's rule)
     * @param string $payload the message itself; text must be UTF-8, or pass $binary
     * @param int    $except  connection id to leave out (the sender, usually); 0 = nobody
     * @throws PublishFailed
     */
    public function publish(string $topic, string $payload, bool $binary = false, int $except = 0): PublishResult
    {
        Ipc\Codec::topic($topic);  // the same rule as the broker, before anything goes on the wire
        if ($except < 0) {
            throw new \InvalidArgumentException('except must be a connection id or 0');
        }
        $query = 'topic=' . $topic . ($except > 0 ? '&except=' . $except : '') . ($binary ? '&binary=1' : '');
        $request = "POST /publish?$query HTTP/1.1\r\nHost: {$this->host}:{$this->port}\r\n"
            . "Authorization: Bearer {$this->token}\r\nContent-Length: " . strlen($payload)
            . "\r\nConnection: close\r\n\r\n" . $payload;
        $raw = $this->transport !== null
            ? ($this->transport)($this->host, $request, $this->port, $this->timeout)
            : $this->exchange($request);
        return self::parse($raw);
    }

    /** @throws PublishFailed */
    private function exchange(string $request): string
    {
        $stream = @stream_socket_client("tcp://{$this->host}:{$this->port}", $errno, $error, $this->timeout);
        if ($stream === false) {
            throw new PublishFailed("cannot reach the publish endpoint: $error");
        }
        try {
            stream_set_timeout($stream, (int) $this->timeout, (int) (fmod($this->timeout, 1.0) * 1e6));
            $written = 0;
            while ($written < strlen($request)) {
                $n = @fwrite($stream, substr($request, $written));
                if ($n === false || $n === 0) {
                    throw new PublishFailed('the publish endpoint closed the connection while receiving the request');
                }
                $written += $n;
            }
            $response = '';
            while (strlen($response) < self::MAX_RESPONSE && !feof($stream)) {
                $chunk = fread($stream, self::MAX_RESPONSE - strlen($response));
                if ($chunk === false || (stream_get_meta_data($stream)['timed_out'] ?? false)) {
                    throw new PublishFailed('the publish endpoint did not answer in time');
                }
                $response .= $chunk;
            }
            return $response;
        } finally {
            fclose($stream);
        }
    }

    /** @throws PublishFailed */
    private static function parse(string $raw): PublishResult
    {
        $split = strpos($raw, "\r\n\r\n");
        if ($split === false || preg_match('#\AHTTP/1\.[01] (\d{3}) #', $raw, $m) !== 1) {
            throw new PublishFailed('the answer is not an HTTP response');
        }
        $status = (int) $m[1];
        $body = substr($raw, $split + 4);
        if ($status !== 200) {
            throw new PublishFailed('publish refused with ' . $status . ': ' . trim($body), $status, $body);
        }
        if (preg_match('/\Asubscribers=(\d+) accepted=(\d+) refused=(\d+)\n\z/', $body, $c) !== 1) {
            throw new PublishFailed('a 200 without the counts', $status, $body);
        }
        return new PublishResult((int) $c[1], (int) $c[2], (int) $c[3]);
    }
}

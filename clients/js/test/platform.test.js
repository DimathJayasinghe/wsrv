// SPDX-License-Identifier: Apache-2.0
// The client against the platform's own WebSocket and a server that refuses the handshake.
//
// Node 22 builds with undici 6.x (for example the official node:22 images) report a failed opening
// handshake with an "error" event and never a "close": the socket stays CONNECTING for ever. A client
// that waits for the close never reconnects. This test fails on such a build without the client's own
// handling, and passes on platforms that follow the specification.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import net from 'node:net';
import { WsrvClient } from '../src/client.js';

const REFUSAL = 'HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n';

test('a client keeps reconnecting after refused handshakes, whatever the platform reports', async (t) => {
  let attempts = 0;
  const server = net.createServer((socket) => {
    attempts += 1;
    socket.on('error', () => {});
    socket.once('data', () => socket.end(REFUSAL));
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const client = new WsrvClient(`ws://127.0.0.1:${server.address().port}/`, {
    backoff: { min: 20, max: 40 },
    reconnectOnOnline: false,
  });
  client.on('error', () => {});
  t.after(() => {
    client.close();
    server.close();
  });
  client.connect();
  const deadline = Date.now() + 5000;
  while (attempts < 3 && Date.now() < deadline) {
    await new Promise((resolve) => setTimeout(resolve, 20));
  }
  assert.ok(attempts >= 3, `the server saw ${attempts} attempt(s); a client that waits for a close that never comes stops at 1`);
  assert.notEqual(client.state, 'closed');
});

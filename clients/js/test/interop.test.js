// SPDX-License-Identifier: Apache-2.0
// End to end: the real wsrvd, a supervised PHP worker, and this client, through restarts.
//   WSRVD=/path/to/wsrvd node --test test/interop.test.js        (PHP: $WSRV_PHP or "php")
import { test, after } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { mkdtempSync, chmodSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { WsrvClient } from '../src/client.js';

const here = dirname(fileURLToPath(import.meta.url));
const wsrvd = process.env.WSRVD;
const php = process.env.WSRV_PHP ?? 'php';
const skip = wsrvd ? false : 'set WSRVD=/path/to/wsrvd to run the interop test';

const dir = mkdtempSync(join(tmpdir(), 'wsrv-js-'));
chmodSync(dir, 0o700);
const socket = join(dir, 'ipc.sock');
const children = new Set();

function start(command, args, { waitFor } = {}) {
  const child = spawn(command, args, { stdio: ['ignore', 'pipe', 'pipe'] });
  children.add(child);
  child.on('exit', () => children.delete(child));
  let output = '';
  const ready = new Promise((resolve, reject) => {
    const onData = (chunk) => {
      output += chunk;
      const match = waitFor?.exec(output);
      if (match) resolve(match);
    };
    child.stdout.on('data', onData);
    child.stderr.on('data', onData);
    child.on('exit', (code) => reject(new Error(`${command} exited early (${code}):\n${output}`)));
    if (!waitFor) resolve(null);
    setTimeout(() => reject(new Error(`${command} did not become ready:\n${output}`)), 15_000).unref();
  });
  return { child, ready, output: () => output };
}

async function stopChild(child, signal) {
  const exited = new Promise((resolve) => child.once('exit', resolve));
  child.kill(signal);
  await exited;
}

function startDaemon(port) {
  return start(wsrvd, [
    `--ipc-socket=${socket}`, `--listen-port=${port}`, '--allow-missing-origin=true',
    '--shutdown-grace-ms=300', '--max-connections-per-ip=0', '--max-new-connections-per-ip-per-second=0',
  ], { waitFor: /listening on port (\d+)/ });
}

/** Resolves with the next event of that type, or rejects after a timeout. */
function next(client, type, predicate = () => true, ms = 10_000) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => { off(); reject(new Error(`timed out waiting for ${type}`)); }, ms);
    const off = client.on(type, (event) => {
      if (predicate(event)) { clearTimeout(timer); off(); resolve(event); }
    });
  });
}

after(() => {
  for (const child of children) child.kill('SIGKILL');
  rmSync(dir, { recursive: true, force: true });
});

test('a client follows a daemon restart and a worker restart, re-joining its room each time', { skip, timeout: 120_000 }, async () => {
  try {
    await scenario();
  } finally {
    for (const child of children) child.kill('SIGKILL'); // a failed run must not leave processes behind
  }
});

async function scenario() {
  let daemon = startDaemon(0);
  const port = Number((await daemon.ready)[1]);
  let worker = start(php, [join(here, 'worker.php'), socket]);

  let tickets = 0;
  const client = new WsrvClient(`ws://127.0.0.1:${port}/`, {
    ticket: () => `ticket-${++tickets}`,
    backoff: { min: 100, max: 500 },
    reconnectOnOnline: false,
  });
  const messages = [];
  client.on('message', ({ data }) => messages.push(data));
  client.join('room.1');

  // 1. first connection: the room is joined and the worker answers
  client.connect();
  await next(client, 'message', ({ data }) => data === 'joined room.1');
  client.send('hello');
  await next(client, 'message', ({ data }) => data === 'echo hello');
  assert.equal(tickets, 1);

  // 2. the daemon is killed and comes back on the same port: the client reconnects by itself,
  //    with a new ticket, and the worker (Supervisor) reconnects to the new daemon
  const reconnecting = next(client, 'reconnecting');
  await stopChild(daemon.child, 'SIGKILL');
  await reconnecting;
  daemon = startDaemon(port);
  await daemon.ready;
  const reopened = await next(client, 'open', (e) => e.reconnect, 30_000);
  assert.equal(reopened.reconnect, true);
  await next(client, 'message', ({ data }) => data === 'joined room.1', 30_000);
  assert.ok(tickets >= 2, `a fresh ticket per attempt (got ${tickets})`);
  client.send('again');
  await next(client, 'message', ({ data }) => data === 'echo again');

  // 3. the worker is stopped with SIGTERM and started again: the daemon closes the browser's socket
  //    (1012, after the grace period), the client reconnects, and the new worker answers
  const closed = next(client, 'close');
  await stopChild(worker.child, 'SIGTERM');
  const closeEvent = await closed;
  assert.equal(closeEvent.code, 1012, 'the daemon says "service restart" when the application leaves');
  assert.equal(closeEvent.willRetry, true);
  worker = start(php, [join(here, 'worker.php'), socket]);
  await next(client, 'open', (e) => e.reconnect, 30_000);
  await next(client, 'message', ({ data }) => data === 'joined room.1', 30_000);
  client.send('third');
  await next(client, 'message', ({ data }) => data === 'echo third');

  client.close();
  assert.equal(client.state, 'closed');
  await stopChild(worker.child, 'SIGTERM');
  await stopChild(daemon.child, 'SIGTERM');
}

// SPDX-License-Identifier: Apache-2.0
// Unit tests: a fake WebSocket and fake timers, so nothing here waits for real time.
import { test, mock } from 'node:test';
import assert from 'node:assert/strict';
import { WsrvClient } from '../src/client.js';

class FakeSocket {
  static instances = [];
  readyState = 0;
  sent = [];
  constructor(url, protocols) {
    this.url = url;
    this.protocols = protocols;
    FakeSocket.instances.push(this);
  }
  send(data) { this.sent.push(data); }
  close(code, reason) { this.closedWith = { code, reason }; this.readyState = 3; }
  // test helpers: what the network would do
  open() { this.readyState = 1; this.onopen?.({}); }
  message(data) { this.onmessage?.({ data }); }
  drop(code = 1006, reason = '', wasClean = false) { this.readyState = 3; this.onclose?.({ code, reason, wasClean }); }
}
const last = () => FakeSocket.instances.at(-1);
const flush = () => new Promise((resolve) => setImmediate(resolve)); // let awaited ticket code run

function make(options = {}) {
  FakeSocket.instances = [];
  mock.timers.reset();
  mock.timers.enable({ apis: ["setTimeout", "Date"] });
  const client = new WsrvClient('ws://host:9000/ws', {
    WebSocket: FakeSocket,
    jitter: () => 1,
    backoff: { min: 100, max: 800 },
    reconnectOnOnline: false,
    ...options,
  });
  const events = [];
  for (const type of ['open', 'close', 'reconnecting', 'error', 'message']) {
    client.on(type, (e) => events.push([type, e]));
  }
  return { client, events };
}
const done = () => mock.timers.reset();

test('connects, and "open" says whether it was a reconnect', async () => {
  const { client, events } = make();
  client.connect();
  await flush();
  assert.equal(client.state, 'connecting');
  last().open();
  assert.equal(client.state, 'open');
  assert.deepEqual(events.at(-1), ['open', { reconnect: false }]);
  last().drop(1012);
  mock.timers.tick(100);
  await flush();
  last().open();
  assert.deepEqual(events.at(-1), ['open', { reconnect: true }]);
  done();
});

test('the delay doubles up to the cap and the ticket is fetched again for every attempt', async () => {
  let n = 0;
  const { client, events } = make({ ticket: async () => `t${++n}` });
  client.connect();
  await flush();
  assert.equal(last().url, 'ws://host:9000/ws?ticket=t1');
  for (const expected of [100, 200, 400, 800, 800]) {
    last().drop(1006);
    const delays = events.filter(([t]) => t === 'reconnecting').map(([, e]) => e.delay);
    assert.equal(delays.at(-1), expected);
    mock.timers.tick(expected);
    await flush();
  }
  assert.equal(last().url, 'ws://host:9000/ws?ticket=t6', 'six attempts, six tickets');
  done();
});

test('the ticket keeps an existing query and is URL-encoded', async () => {
  const { client } = make({ ticket: 'a b&c', ticketParam: 't' });
  client.connect();
  await flush();
  assert.equal(last().url, 'ws://host:9000/ws?t=a+b%26c');
  done();
});

test('a failing ticket function is an error and a backoff, not a crash', async () => {
  let calls = 0;
  const { client, events } = make({
    ticket: async () => { if (++calls === 1) throw new Error('login expired'); return 'ok'; },
  });
  client.connect();
  await flush();
  assert.equal(FakeSocket.instances.length, 0, 'no socket without a ticket');
  assert.equal(events[0][0], 'error');
  assert.equal(client.state, 'waiting');
  mock.timers.tick(100);
  await flush();
  assert.match(last().url, /ticket=ok/);
  done();
});

test('rooms are sent on every open, including after a reconnect, and leave() forgets them', async () => {
  const { client } = make();
  client.join('room.1'); // before connecting: remembered
  client.connect();
  await flush();
  last().open();
  assert.deepEqual(last().sent, ['join room.1']);
  client.join('room.2'); // while open: sent at once
  assert.deepEqual(last().sent, ['join room.1', 'join room.2']);
  client.leave('room.1');
  assert.deepEqual(last().sent.at(-1), 'leave room.1');
  last().drop(1012);
  mock.timers.tick(100);
  await flush();
  last().open();
  assert.deepEqual(last().sent, ['join room.2'], 'only the room still wanted');
  assert.deepEqual(client.rooms, ['room.2']);
  done();
});

test('joinMessage decides the wire format', async () => {
  const { client } = make({ joinMessage: (room) => JSON.stringify({ op: 'join', room }) });
  client.connect();
  await flush();
  client.join('r');
  last().open();
  assert.deepEqual(last().sent, ['{"op":"join","room":"r"}']);
  done();
});

test('send() says whether the message went out; queueWhileOffline is opt-in and bounded', async () => {
  const plain = make();
  plain.client.connect();
  await flush();
  assert.equal(plain.client.send('early'), false, 'not open yet: dropped, and the caller is told');
  last().open();
  assert.equal(plain.client.send('hi'), true);
  assert.deepEqual(last().sent, ['hi']);
  done();

  const queued = make({ queueWhileOffline: true, maxQueue: 2 });
  queued.client.connect();
  await flush();
  assert.equal(queued.client.send('a'), true);
  assert.equal(queued.client.send('b'), true);
  assert.equal(queued.client.send('c'), false, 'queue full');
  last().open();
  assert.deepEqual(last().sent, ['a', 'b']);
  done();
});

test('close code 1008 (policy) and shouldReconnect() stop the retries', async () => {
  const policy = make();
  policy.client.connect();
  await flush();
  last().open();
  last().drop(1008, 'origin not allowed', true);
  assert.equal(policy.client.state, 'closed');
  assert.equal(policy.events.filter(([t]) => t === 'reconnecting').length, 0);
  assert.equal(policy.events.find(([t]) => t === 'close')[1].willRetry, false);
  done();

  const custom = make({ shouldReconnect: (info) => info.code !== 4001 });
  custom.client.connect();
  await flush();
  last().open();
  last().drop(4001);
  assert.equal(custom.client.state, 'closed');
  done();
});

test('the backoff restarts only after a connection that lasted, not after a flapping one', async () => {
  const { client, events } = make({ resetAfter: 5000 });
  const delays = () => events.filter(([t]) => t === 'reconnecting').map(([, e]) => e.delay);
  client.connect();
  await flush();
  last().drop(1006); // never opened: 100
  mock.timers.tick(100);
  await flush();
  last().open();
  last().drop(1012); // opened and died at once: backoff continues: 200
  mock.timers.tick(200);
  await flush();
  last().open();
  mock.timers.tick(6000); // a long session
  last().drop(1012); // starts over: 100
  assert.deepEqual(delays(), [100, 200, 100]);
  done();
});

test('maxRetries stops after that many attempts: the last close says willRetry false', async () => {
  const { client, events } = make({ maxRetries: 2 });
  client.connect();
  await flush();
  for (let i = 0; i < 2; i++) {
    last().drop(1006);
    mock.timers.tick(800);
    await flush();
  }
  last().drop(1006);
  assert.equal(client.state, 'closed');
  assert.equal(events.filter(([t]) => t === 'reconnecting').length, 2);
  assert.equal(events.filter(([t]) => t === 'close').at(-1)[1].willRetry, false);
  assert.equal(FakeSocket.instances.length, 3, 'one first attempt and two retries');
  done();
});

test('close() ends everything: no reconnect, a late event from the old socket is ignored, connect() restarts', async () => {
  const { client, events } = make();
  client.connect();
  await flush();
  const first = last();
  first.open();
  client.close();
  assert.deepEqual(first.closedWith, { code: 1000, reason: '' });
  first.drop(1006); // the old socket reports late
  mock.timers.tick(10_000);
  await flush();
  assert.equal(FakeSocket.instances.length, 1, 'no new attempt after close()');
  assert.equal(client.state, 'closed');
  assert.equal(events.filter(([t]) => t === 'reconnecting').length, 0);
  client.connect();
  await flush();
  assert.equal(FakeSocket.instances.length, 2);
  done();
});

test('close() while a ticket is still being fetched opens no socket', async () => {
  let release;
  const { client } = make({ ticket: () => new Promise((resolve) => { release = resolve; }) });
  client.connect();
  client.close();
  release('late');
  await flush();
  assert.equal(FakeSocket.instances.length, 0);
  done();
});

test('connect() while already running does nothing', async () => {
  const { client } = make();
  client.connect();
  client.connect();
  await flush();
  assert.equal(FakeSocket.instances.length, 1);
  done();
});

test('a throwing listener is reported as an error and does not break the loop', async () => {
  const { client, events } = make();
  client.on('open', () => { throw new Error('bug in the page'); });
  client.connect();
  await flush();
  last().open();
  assert.equal(client.state, 'open');
  assert.ok(events.some(([t, e]) => t === 'error' && e.error.message === 'bug in the page'));
  done();
});

test('bad options are refused at construction', () => {
  assert.throws(() => new WsrvClient('ws://x', { WebSocket: FakeSocket, backoff: { min: 0 } }), RangeError);
  assert.throws(() => new WsrvClient('ws://x', { WebSocket: FakeSocket, backoff: { min: 10, max: 5 } }), RangeError);
  assert.throws(() => new WsrvClient('ws://x', { WebSocket: 'nope' }), TypeError);
});

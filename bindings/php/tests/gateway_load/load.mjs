// SPDX-License-Identifier: Apache-2.0
// A fleet of Pusher-protocol browsers, for the gateway load and churn runs (run.py drives it). It speaks on
// the wire what pusher-js 8 speaks: connect, read connection_established, send every pusher:subscribe back to
// back (signing private and presence channels with the app secret, as Laravel's auth endpoint would), answer
// pusher:ping, whisper with client-* events. Uses Node's built-in WebSocket: no dependency.
//
//   node load.mjs --url=ws://127.0.0.1:8080 --key=K --secret=S --scenario=join|whisper|churn [options]
//
// join     --clients=N --public=P --private=R --presence=Q [--ramp=conns/s] [--hold=s] [--room-size=50]
//          N browsers connect, join P public, R private (one per browser) and Q presence channels (rooms of
//          room-size), hold, and leave. With --ramp=0 they all connect at once: a reconnect storm.
// whisper  --members=M --groups=G --rate=r --duration=s
//          G private channels with M members each; every member whispers r times a second to the others.
// churn    --concurrency=C --duration=s [--abuse=0.02] [--hold=1.5]
//          C loops of: connect, join a few random channels (public, private, presence, with users shared
//          between connections), whisper, sometimes leave a channel, hold, disconnect (sometimes at once,
//          before the daemon has answered the joins); a fraction of the connections misbehaves on purpose.
//
// Prints one JSON object on stdout when it is done. Progress goes to stderr.

import { createHmac } from 'node:crypto';
import { performance } from 'node:perf_hooks';

const args = Object.fromEntries(
  process.argv.slice(2).map((a) => {
    const m = /^--([^=]+)(?:=(.*))?$/.exec(a);
    return m ? [m[1], m[2] ?? 'true'] : [a, 'true'];
  }),
);
const num = (name, fallback) => (args[name] === undefined ? fallback : Number(args[name]));
const url = args.url;
const key = args.key;
const secret = args.secret;
if (!url || !key || !secret || !args.scenario) {
  console.error('usage: node load.mjs --url=ws://host:port --key=K --secret=S --scenario=join|whisper|churn [...]');
  process.exit(2);
}
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const rand = (n) => Math.floor(Math.random() * n);
const pick = (list) => list[rand(list.length)];
const WS_URL = `${url}/app/${key}?protocol=7&client=js&version=8.6.0&flash=false`;

function percentiles(values) {
  if (values.length === 0) return { n: 0 };
  const sorted = [...values].sort((a, b) => a - b);
  const at = (p) => sorted[Math.min(sorted.length - 1, Math.floor((p / 100) * sorted.length))];
  return { n: sorted.length, p50: round(at(50)), p95: round(at(95)), p99: round(at(99)), max: round(sorted[sorted.length - 1]) };
}
const round = (v) => Math.round(v * 100) / 100;

function sign(socketId, channel, channelData) {
  const toSign = channelData === undefined ? `${socketId}:${channel}` : `${socketId}:${channel}:${channelData}`;
  return `${key}:${createHmac('sha256', secret).update(toSign).digest('hex')}`;
}

/** One browser: the Pusher protocol on a WebSocket. */
class Browser {
  constructor(counters) {
    this.counters = counters;
    this.socketId = null;
    this.pending = new Map(); // channel -> [{resolve, reject, t0, timer}]: one subscribe may be sent twice
    this.closed = false;
    this.onWhisper = null;
    this.ws = null;
  }

  open(timeoutMs = 20000) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        reject(new Error('connect timeout'));
        this.ws?.close();
      }, timeoutMs);
      let ws;
      try {
        ws = new WebSocket(WS_URL);
      } catch (e) {
        clearTimeout(timer);
        reject(e);
        return;
      }
      this.ws = ws;
      ws.addEventListener('message', (ev) => {
        if (typeof ev.data !== 'string') return;
        let frame;
        try {
          frame = JSON.parse(ev.data);
        } catch {
          this.counters.garbage++;
          return;
        }
        if (frame.event === 'pusher:connection_established') {
          clearTimeout(timer);
          this.socketId = JSON.parse(frame.data).socket_id;
          resolve(this.socketId);
          return;
        }
        this.handle(frame);
      });
      ws.addEventListener('error', () => {
        clearTimeout(timer);
        reject(new Error('socket error'));
      });
      ws.addEventListener('close', (ev) => {
        clearTimeout(timer);
        this.closed = true;
        this.counters.closes[ev.code] = (this.counters.closes[ev.code] ?? 0) + 1;
        for (const channel of [...this.pending.keys()]) this.settle(channel, false, new Error(`closed ${ev.code}`));
        reject(new Error(`closed ${ev.code}`));
      });
    });
  }

  handle(frame) {
    switch (frame.event) {
      case 'pusher:ping':
        this.send({ event: 'pusher:pong', data: {} });
        break;
      case 'pusher_internal:subscription_succeeded':
        this.settle(frame.channel, true);
        break;
      case 'pusher:subscription_error':
        this.counters.subscriptionErrors++;
        this.settle(frame.channel, false, new Error('subscription_error'));
        break;
      case 'pusher:error': {
        const data = typeof frame.data === 'string' ? JSON.parse(frame.data) : frame.data;
        const code = data?.code ?? 0;
        this.counters.errors[code] = (this.counters.errors[code] ?? 0) + 1;
        break;
      }
      case 'pusher_internal:member_added':
        this.counters.memberAdded++;
        break;
      case 'pusher_internal:member_removed':
        this.counters.memberRemoved++;
        break;
      default:
        if (frame.event?.startsWith('client-') && this.onWhisper) this.onWhisper(frame);
    }
  }

  /** Answer every subscribe still waiting for this channel (a page may send the same one twice). */
  settle(channel, ok, error) {
    const waiters = this.pending.get(channel);
    if (!waiters) return;
    this.pending.delete(channel);
    for (const w of waiters) {
      clearTimeout(w.timer);
      if (ok) w.resolve(performance.now() - w.t0);
      else w.reject(error);
    }
  }

  send(obj) {
    if (this.ws && this.ws.readyState === WebSocket.OPEN) this.ws.send(typeof obj === 'string' ? obj : JSON.stringify(obj));
  }

  /**
   * Send pusher:subscribe the way pusher-js does; resolves with the milliseconds the answer took. No answer
   * within timeoutMs is a failure that is counted (a join the gateway lost is a bug, not a slow run).
   */
  subscribe(channel, userId, timeoutMs = 20000) {
    const data = { channel };
    if (channel.startsWith('presence-')) {
      data.channel_data = JSON.stringify({ user_id: String(userId), user_info: { name: `user ${userId}` } });
      data.auth = sign(this.socketId, channel, data.channel_data);
    } else if (channel.startsWith('private-')) {
      data.auth = sign(this.socketId, channel);
    }
    return new Promise((resolve, reject) => {
      const waiter = { resolve, reject, t0: performance.now(), timer: null };
      waiter.timer = setTimeout(() => {
        const waiters = this.pending.get(channel) ?? [];
        const at = waiters.indexOf(waiter);
        if (at >= 0) waiters.splice(at, 1);
        if (waiters.length === 0) this.pending.delete(channel);
        this.counters.subscribeTimeouts++;
        reject(new Error('subscribe timeout'));
      }, timeoutMs);
      const waiters = this.pending.get(channel) ?? [];
      waiters.push(waiter);
      this.pending.set(channel, waiters);
      this.send({ event: 'pusher:subscribe', data });
    });
  }

  unsubscribe(channel) {
    this.send({ event: 'pusher:unsubscribe', data: { channel } });
  }

  whisper(channel, event, data) {
    this.send({ event, channel, data });
  }

  close(code = 1000) {
    try {
      this.ws?.close(code);
    } catch {
      /* already closing */
    }
  }
}

function newCounters() {
  return { errors: {}, closes: {}, subscriptionErrors: 0, subscribeTimeouts: 0, memberAdded: 0, memberRemoved: 0, garbage: 0 };
}
const sum = (list) => list.reduce((a, b) => a + b, 0);

// ---- join --------------------------------------------------------------------------------------------------

async function scenarioJoin() {
  const N = num('clients', 100);
  const P = num('public', 1);
  const R = num('private', 1);
  const Q = num('presence', 1);
  const ramp = num('ramp', 0);
  const hold = num('hold', 5);
  const roomSize = num('room-size', 50);
  const offset = num('offset', 0);
  const counters = newCounters();
  const connectMs = [];
  const readyMs = [];
  const subMs = [];
  const failures = { connect: 0, subscribe: 0 };
  const live = [];
  let readyCount = 0;
  const start = performance.now();
  let lastReady = start;

  async function one(i) {
    const id = offset + i;
    const b = new Browser(counters);
    const t0 = performance.now();
    try {
      await b.open();
    } catch {
      failures.connect++;
      return;
    }
    const opened = performance.now();
    connectMs.push(opened - t0);
    live.push(b);
    const subs = [];
    for (let p = 0; p < P; p++) subs.push(b.subscribe(`pub.${(id + p) % 20}`));
    for (let r = 0; r < R; r++) subs.push(b.subscribe(`private-user.${id}.${r}`));
    for (let q = 0; q < Q; q++) subs.push(b.subscribe(`presence-room.${Math.floor(id / roomSize)}.${q}`, id));
    const settled = await Promise.allSettled(subs);
    for (const s of settled) {
      if (s.status === 'fulfilled') subMs.push(s.value);
      else failures.subscribe++;
    }
    readyMs.push(performance.now() - opened);
    readyCount++;
    lastReady = performance.now();
  }

  const tasks = [];
  for (let i = 0; i < N; i++) {
    tasks.push(one(i));
    if (ramp > 0 && i % Math.max(1, Math.round(ramp / 20)) === 0) await sleep(1000 / 20);
  }
  await Promise.all(tasks);
  const allReadyMs = lastReady - start;
  process.stderr.write(`join: ${readyCount}/${N} ready after ${round(allReadyMs)} ms, holding ${hold} s\n`);
  await sleep(hold * 1000);
  const stillOpen = live.filter((b) => !b.closed).length;
  for (const b of live) b.close(1000);
  await sleep(500);
  return {
    scenario: 'join',
    clients: N,
    channelsPerClient: P + R + Q,
    ramp,
    ready: readyCount,
    stillOpenAfterHold: stillOpen,
    allReadyMs: round(allReadyMs),
    joinsPerSecond: round((readyCount * (P + R + Q) * 1000) / allReadyMs),
    connectMs: percentiles(connectMs),
    readyMs: percentiles(readyMs),
    subscribeMs: percentiles(subMs),
    failures,
    counters,
  };
}

// ---- whisper -----------------------------------------------------------------------------------------------

async function scenarioWhisper() {
  const M = num('members', 5);
  const G = num('groups', 10);
  const rate = num('rate', 5);
  const duration = num('duration', 20);
  const offset = num('offset', 0);
  const counters = newCounters();
  const latency = [];
  let sent = 0;
  let received = 0;
  const all = [];
  const failures = { connect: 0, subscribe: 0 };

  async function member(g, m) {
    const b = new Browser(counters);
    try {
      await b.open();
    } catch {
      failures.connect++;
      return null;
    }
    b.onWhisper = (frame) => {
      received++;
      const data = typeof frame.data === 'string' ? JSON.parse(frame.data) : frame.data;
      if (data && typeof data.t === 'number') latency.push(Date.now() - data.t);
    };
    try {
      await b.subscribe(`private-chat.${offset + g}`);
    } catch {
      failures.subscribe++;
      return null;
    }
    return b;
  }

  const joins = [];
  for (let g = 0; g < G; g++) for (let m = 0; m < M; m++) joins.push(member(g, m));
  const members = (await Promise.all(joins)).filter(Boolean);
  all.push(...members);
  process.stderr.write(`whisper: ${members.length}/${G * M} members joined, whispering ${rate}/s each for ${duration} s\n`);
  const endAt = performance.now() + duration * 1000;
  const loops = members.map(async (b, idx) => {
    const g = Math.floor(idx / M);
    await sleep(rand(1000 / rate));
    while (performance.now() < endAt && !b.closed) {
      b.whisper(`private-chat.${offset + g}`, 'client-typing', { t: Date.now(), from: idx });
      sent++;
      await sleep(1000 / rate);
    }
  });
  await Promise.all(loops);
  await sleep(1500); // let the last deliveries arrive
  const expected = sent * (M - 1);
  for (const b of all) b.close(1000);
  await sleep(500);
  return {
    scenario: 'whisper',
    members: members.length,
    groups: G,
    perMemberRate: rate,
    whispersSent: sent,
    deliveriesExpected: expected,
    deliveriesReceived: received,
    deliveryLoss: expected === 0 ? 0 : round(((expected - received) / expected) * 100),
    latencyMs: percentiles(latency),
    failures,
    counters,
  };
}

// ---- churn -------------------------------------------------------------------------------------------------

async function scenarioChurn() {
  const C = num('concurrency', 20);
  const duration = num('duration', 30);
  const abuse = num('abuse', 0.02);
  const holdMax = num('hold', 1.5) * 1000;
  const offset = num('offset', 0);
  const counters = newCounters();
  const stats = { connections: 0, connectFailures: 0, joinsOk: 0, joinsFailed: 0, whispers: 0, leaves: 0, immediateCloses: 0, abusive: 0 };
  const endAt = performance.now() + duration * 1000;

  function randomChannel() {
    const r = Math.random();
    if (r < 0.3) return { name: `pub.${rand(20)}` };
    if (r < 0.7) return { name: `private-user.${offset + rand(400)}` };
    return { name: `presence-room.${rand(8)}`, user: rand(30) };
  }

  async function loop() {
    while (performance.now() < endAt) {
      const b = new Browser(counters);
      try {
        await b.open();
      } catch {
        stats.connectFailures++;
        await sleep(50);
        continue;
      }
      stats.connections++;
      const channels = [];
      const subs = [];
      const n = 1 + rand(5);
      for (let i = 0; i < n; i++) {
        const c = randomChannel();
        channels.push(c);
        subs.push(b.subscribe(c.name, c.user).then(() => true, () => false));
      }
      if (Math.random() < 0.15) {
        // leave before the daemon has answered: the path pusher-js takes on a route change
        stats.immediateCloses++;
        b.close(1000);
        await Promise.allSettled(subs);
        continue;
      }
      if (Math.random() < abuse) {
        stats.abusive++;
        if (Math.random() < 0.5) for (let i = 0; i < 12; i++) b.send('not json at all'); // strikes
        else for (let i = 0; i < 400; i++) b.send({ event: 'pusher:ping', data: {} }); // over the message bucket
      }
      const results = await Promise.all(subs);
      for (const ok of results) ok ? stats.joinsOk++ : stats.joinsFailed++;
      const joined = channels.filter((_, i) => results[i] && !channels[i].name.startsWith('pub.'));
      if (joined.length > 0 && Math.random() < 0.5) {
        b.whisper(pick(joined).name, 'client-ping', { t: Date.now() });
        stats.whispers++;
      }
      if (channels.length > 1 && Math.random() < 0.3) {
        b.unsubscribe(pick(channels).name);
        stats.leaves++;
      }
      await sleep(rand(holdMax));
      b.close(1000);
      await sleep(rand(50));
    }
  }

  await Promise.all(Array.from({ length: C }, loop));
  await sleep(1000);
  return { scenario: 'churn', concurrency: C, duration, stats, counters };
}

const scenarios = { join: scenarioJoin, whisper: scenarioWhisper, churn: scenarioChurn };
const run = scenarios[args.scenario];
if (!run) {
  console.error(`unknown scenario ${args.scenario}`);
  process.exit(2);
}
const result = await run();
console.log(JSON.stringify(result));
process.exit(0);

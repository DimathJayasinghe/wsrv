// A fleet of WsrvClients that keep talking while the soak script breaks the server. Prints one JSON
// line every 10 s ("tick") and a final one on SIGTERM ("final"). Nothing here decides pass or fail:
// soak.py reads these lines and the container's own numbers.
import { WsrvClient } from '/soak/client/client.js';

const target = process.env.TARGET;
const count = Number(process.env.CLIENTS ?? 200);
const REPLY_TIMEOUT_MS = 10_000;

// late_echo: answered after the client gave up waiting (the reply timeout) or after it reconnected. Not
// an error: a cut network holds a reply for as long as the cut lasts. bad_echo: an answer to a message
// that was never sent, or a second answer to one: that is a bug.
const totals = { sent: 0, echoed: 0, lost: 0, offline_skips: 0, reconnects: 0, late_echo: 0, bad_echo: 0 };
const latencies = [];
const clients = [];
const traces = []; // per client: how many sockets it made and what the latest one did

/** The platform WebSocket, recording what each socket does, so a client that is stuck can be explained. */
function tracedWebSocket(trace) {
  return class extends WebSocket {
    constructor(url, protocols) {
      super(url, protocols);
      const record = { born: Date.now(), events: [] };
      trace.sockets += 1;
      trace.last = record;
      trace.ws = this;
      for (const type of ['open', 'error', 'close']) {
        this.addEventListener(type, () => record.events.push(`${type}+${Date.now() - record.born}ms`));
      }
    }
  };
}

for (let id = 0; id < count; id++) {
  const trace = { sockets: 0, last: null, ws: null };
  traces.push(trace);
  const client = new WsrvClient(target, {
    backoff: { min: 200, max: 3000 },
    reconnectOnOnline: false,
    WebSocket: tracedWebSocket(trace),
  });
  const pending = new Map(); // seq -> sent at
  const answered = new Set(); // recent seqs that were answered, to tell a late echo from a duplicate
  let seq = 0;
  client.on('open', ({ reconnect }) => {
    if (reconnect) totals.reconnects += 1;
    client.join('soak');
  });
  client.on('close', () => {
    totals.lost += pending.size; // sent on this connection and never answered
    pending.clear();
  });
  client.on('message', ({ data }) => {
    if (typeof data !== 'string' || !data.startsWith('echo ping:')) return;
    const [, , n] = data.split(':');
    const number = Number(n);
    const sentAt = pending.get(number);
    answered.delete(number - 5000); // keep the window small: a long soak sends millions
    if (sentAt === undefined) {
      if (Number.isInteger(number) && number >= 1 && number <= seq && !answered.has(number)) {
        totals.late_echo += 1; // sent, given up on (timeout or reconnect), answered after all
        answered.add(number);
      } else {
        totals.bad_echo += 1; // never sent, or answered twice
      }
      return;
    }
    pending.delete(number);
    answered.add(number);
    totals.echoed += 1;
    latencies.push(Date.now() - sentAt);
  });
  client.on('error', () => {});
  client.connect();
  clients.push(client);

  const tick = () => {
    if (client.state === 'open') {
      seq += 1;
      if (client.send(`ping:${id}:${seq}`)) {
        pending.set(seq, Date.now());
        totals.sent += 1;
      }
      for (const [n, at] of pending) {
        if (Date.now() - at > REPLY_TIMEOUT_MS) {
          pending.delete(n);
          totals.lost += 1;
        }
      }
    } else {
      totals.offline_skips += 1;
    }
    setTimeout(tick, 500 + Math.random() * 1000);
  };
  setTimeout(tick, Math.random() * 1500);
}

function percentile(sorted, p) {
  return sorted.length ? sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * p))] : null;
}

function report(kind) {
  const sorted = latencies.splice(0).sort((a, b) => a - b);
  const open = clients.filter((c) => c.state === 'open').length;
  const states = {};
  const stuck = []; // a few clients that are not open, with what their latest socket did
  clients.forEach((c, id) => {
    states[c.state] = (states[c.state] ?? 0) + 1;
    if (c.state !== 'open' && stuck.length < 5) {
      const { sockets, last, ws } = traces[id];
      stuck.push({ id, state: c.state, sockets, readyState: ws?.readyState ?? null,
        age_ms: last ? Date.now() - last.born : null, events: last ? last.events.slice(-4) : [] });
    }
  });
  console.log(JSON.stringify({ kind, t: Date.now(), clients: count, open, states, ...totals,
    p50_ms: percentile(sorted, 0.5), p99_ms: percentile(sorted, 0.99), max_ms: sorted.at(-1) ?? null,
    rss_mb: Math.round(process.memoryUsage().rss / 1048576), stuck }));
}

setInterval(() => report('tick'), 10_000);
for (const signal of ['SIGTERM', 'SIGINT']) {
  process.on(signal, () => {
    report('final');
    process.exit(0);
  });
}

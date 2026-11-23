# wsrv-client (JavaScript)

A WebSocket client that survives restarts. Browser and Node 22+, no dependencies, one file (`src/client.js`) plus a hand-written `.d.ts`. Design and limits: [ADR-0031](../../docs/adr/0031-browser-and-node-client.md).

Why it exists: when you deploy, restart or drain a node, the daemon closes every socket (1001 on shutdown, 1012 when the worker leaves), and **a browser does not reconnect on its own**. Every app then writes the same loop. This is that loop, tested against a real `wsrvd`.

```js
import { WsrvClient } from './src/client.js';

const client = new WsrvClient('wss://app.example.com/ws', {
  // called before EVERY attempt: a ticket is single-use (docs/LARAVEL.md section 3)
  ticket: async () => (await fetch('/ws-ticket', { method: 'POST' }).then((r) => r.json())).ticket,
});

client.on('message', ({ data }) => render(JSON.parse(data)));
client.on('reconnecting', ({ attempt, delay }) => showBanner(`reconnecting (${attempt})...`));
client.on('open', ({ reconnect }) => hideBanner());

client.join('room.7');   // remembered; re-sent after every reconnect
client.connect();
client.send(JSON.stringify({ text: 'hi' })); // false if not connected: the message was dropped
```

## What it does

- **Backoff:** 500 ms doubling to 15 s, each delay jittered to 50-100% so a crowd of clients does not return in step. The backoff starts over only after a connection stayed open 10 s (`resetAfter`), so a server that accepts and drops immediately is not hammered.
- **Ticket per attempt:** `ticket` may be a string or an (async) function. A function that throws is an `error` event and a backoff, not a crash.
- **Rooms:** `join(room)` / `leave(room)` send `join <room>` / `leave <room>` (the protocol of the examples; change it with `joinMessage` / `leaveMessage`). The rooms still wanted are sent again after every reconnect. The daemon never lets a browser name a room: your worker decides on that message.
- **Stops retrying** on `close()`, on close code 1008 (policy: the server refused this client), when `shouldReconnect(info)` returns false, and after `maxRetries`.
- **Back online:** in a browser, the `online` event ends the wait at once.
- **Offline sends:** `send()` returns `false` unless the socket is open. Opt in with `queueWhileOffline: true` (bounded by `maxQueue`, default 100) to send them after the next connect.

## What it does not do

- No message replay. Delivery is at most once (docs/IPC_PROTOCOL.md): what was published while the client was disconnected is gone unless your application numbers its messages and asks for the gap.
- No client heartbeat: browsers cannot send Ping frames, and the server pings.
- No wsrv-specific wire protocol: it works against any WebSocket server.

## Events

`open {reconnect}` · `message {data}` · `close {code, reason, wasClean, willRetry}` · `reconnecting {attempt, delay, closeInfo}` · `error {error}`. A throwing listener is reported as `error`, never breaks the connection loop.

## Tests

```sh
node --test test/client.test.js                      # unit tests: fake WebSocket, fake timers
WSRVD=/path/to/wsrvd node --test test/interop.test.js # real wsrvd + a supervised PHP worker, both restarted
```

The interop test connects a client, kills the daemon with `SIGKILL` and starts it again on the same port, then stops and restarts the worker, and checks that the client reconnects each time with a new ticket, re-joins its room and gets answers. CTest runs both as `js_unit` and `js_interop` when Node 22+ is installed.

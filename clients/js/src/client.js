// SPDX-License-Identifier: Apache-2.0
// A WebSocket client that survives restarts (ADR-0031). No dependencies; browser and Node 22+.
//
//   const client = new WsrvClient('wss://app.example.com/ws', {
//     ticket: async () => (await fetch('/ws-ticket', { method: 'POST' }).then((r) => r.json())).ticket,
//   });
//   client.on('message', ({ data }) => render(JSON.parse(data)));
//   client.join('room.7');       // re-joined after every reconnect
//   client.connect();

const DEFAULTS = {
  ticketParam: 'ticket',
  backoff: { min: 500, max: 15_000, factor: 2 },
  resetAfter: 10_000,
  connectTimeout: 15_000, // ms an attempt (ticket and handshake) may take before it is abandoned; 0 = never
  maxRetries: Infinity,
  queueWhileOffline: false,
  maxQueue: 100,
  reconnectOnOnline: true,
  joinMessage: (room) => `join ${room}`,
  leaveMessage: (room) => `leave ${room}`,
};

// 1008 = policy violation: the server refused this client; asking again will not change the answer.
const NO_RETRY_CODES = new Set([1008]);

const OPEN = 1; // WebSocket.OPEN, spelled out so an injected implementation need not define it

/** States: idle (never started), connecting, open, waiting (between attempts), closed (by close()). */
export class WsrvClient {
  #url;
  #options;
  #WebSocket;
  #ws = null;
  #state = 'idle';
  #attempt = 0; // consecutive failed attempts: the backoff exponent
  #retries = 0; // attempts since the last stable connection: compared with maxRetries
  #timer = null;
  #connectTimer = null; // the deadline of the attempt in progress
  #openedAt = 0;
  #everOpened = false; // lets 'open' say whether this is a reconnect
  #rooms = new Set();
  #queue = [];
  #listeners = new Map();
  #generation = 0; // identifies the current socket: events from older ones are ignored
  #onlineHandler = null;

  /**
   * @param {string} url ws:// or wss:// address of the server
   * @param {object} [options] see client.d.ts
   */
  constructor(url, options = {}) {
    this.#url = url;
    this.#options = {
      ...DEFAULTS,
      ...options,
      backoff: { ...DEFAULTS.backoff, ...options.backoff },
    };
    this.#WebSocket = options.WebSocket ?? globalThis.WebSocket;
    if (typeof this.#WebSocket !== 'function') {
      throw new TypeError('no WebSocket implementation: use Node 22+, a browser, or pass options.WebSocket');
    }
    const { min, max, factor } = this.#options.backoff;
    if (!(min > 0) || !(max >= min) || !(factor >= 1)) {
      throw new RangeError('backoff needs 0 < min <= max and factor >= 1');
    }
    const { connectTimeout } = this.#options;
    if (typeof connectTimeout !== 'number' || !Number.isFinite(connectTimeout) || connectTimeout < 0) {
      throw new RangeError('connectTimeout is a number of milliseconds, 0 or more (0 turns the deadline off)');
    }
  }

  get state() {
    return this.#state;
  }

  /** Rooms this client will (re)join whenever it is connected. */
  get rooms() {
    return [...this.#rooms];
  }

  /** Start connecting. Safe to call again after close(); a no-op while already running. */
  connect() {
    if (this.#state !== 'idle' && this.#state !== 'closed') {
      return;
    }
    this.#attempt = 0;
    this.#retries = 0;
    this.#watchOnline();
    this.#attemptConnection();
  }

  /**
   * Send one message. @returns {boolean} true if it went to the socket (or into the offline queue
   * when queueWhileOffline is on); false means it was dropped, so the caller knows.
   */
  send(data) {
    if (this.#ws && this.#state === 'open' && this.#ws.readyState === OPEN) {
      this.#ws.send(data);
      return true;
    }
    if (this.#options.queueWhileOffline && this.#queue.length < this.#options.maxQueue) {
      this.#queue.push(data);
      return true;
    }
    return false;
  }

  /** Remember the room and, when connected, ask the server to put us in it. Re-sent after reconnects. */
  join(room) {
    this.#rooms.add(room);
    if (this.#state === 'open') {
      this.#ws.send(this.#options.joinMessage(room));
    }
  }

  leave(room) {
    if (this.#rooms.delete(room) && this.#state === 'open') {
      this.#ws.send(this.#options.leaveMessage(room));
    }
  }

  /** Close for good: no more reconnects. connect() starts again. */
  close(code = 1000, reason = '') {
    const wasOpen = this.#state === 'open' || this.#state === 'connecting';
    this.#state = 'closed';
    this.#clearTimer();
    this.#clearConnectTimer();
    this.#unwatchOnline();
    this.#queue = [];
    const ws = this.#ws;
    this.#ws = null;
    this.#generation += 1; // whatever the old socket still reports is not ours any more
    if (ws && wasOpen) {
      try {
        ws.close(code, reason);
      } catch {
        // a socket that cannot be closed is already gone
      }
    }
    this.#emit('close', { code, reason, wasClean: true, willRetry: false });
  }

  /** Listen for 'open', 'message', 'close', 'reconnecting' or 'error'. @returns {() => void} an unsubscribe function */
  on(type, listener) {
    if (!this.#listeners.has(type)) {
      this.#listeners.set(type, new Set());
    }
    this.#listeners.get(type).add(listener);
    return () => this.off(type, listener);
  }

  off(type, listener) {
    this.#listeners.get(type)?.delete(listener);
  }

  // ---- connecting ---------------------------------------------------------------------------------

  async #attemptConnection() {
    this.#state = 'connecting';
    const generation = ++this.#generation;
    this.#armConnectTimeout(generation);
    let url;
    try {
      url = await this.#urlWithTicket();
    } catch (error) {
      if (generation === this.#generation) {
        this.#emit('error', { error });
        this.#scheduleRetry(null);
      }
      return;
    }
    if (generation !== this.#generation) {
      return; // close() or a newer attempt came while the ticket was being fetched
    }
    let ws;
    try {
      ws = new this.#WebSocket(url, this.#options.protocols);
    } catch (error) {
      this.#emit('error', { error });
      this.#scheduleRetry(null);
      return;
    }
    this.#ws = ws;
    ws.onopen = () => this.#handleOpen(generation);
    ws.onmessage = (event) => generation === this.#generation && this.#emit('message', { data: event.data });
    ws.onerror = (event) => this.#handleSocketError(generation, ws, event);
    ws.onclose = (event) => this.#handleClose(generation, event);
  }

  #handleSocketError(generation, ws, event) {
    if (generation !== this.#generation) {
      return;
    }
    this.#emit('error', { error: event });
    if (generation === this.#generation && this.#state === 'connecting') {
      // The socket failed before it opened. The specification sends a 'close' next, but Node 22's
      // built-in WebSocket (undici 6.x, e.g. the official node:22 images) does not: it leaves the socket
      // in CONNECTING for ever, and waiting for the close would leave this client stuck. An error
      // before open is the end of the attempt.
      this.#failAttempt(generation, ws, { code: 1006, reason: '', wasClean: false });
    }
  }

  /** The attempt cannot finish: report it the way a close would, back off, and stop listening to its socket. */
  #failAttempt(generation, ws, info) {
    this.#handleClose(generation, info);
    this.#generation += 1; // whatever the abandoned socket still reports (a close, perhaps) is not ours any more
    try {
      ws?.close();
    } catch {
      // a socket that cannot be closed is already gone
    }
  }

  /** Every attempt has a deadline, so no platform behaviour can leave the client in 'connecting' for ever. */
  #armConnectTimeout(generation) {
    this.#clearConnectTimer();
    const { connectTimeout } = this.#options;
    if (connectTimeout > 0) {
      this.#connectTimer = setTimeout(() => this.#connectTimedOut(generation), connectTimeout);
    }
  }

  #connectTimedOut(generation) {
    this.#connectTimer = null;
    if (generation !== this.#generation || this.#state !== 'connecting') {
      return;
    }
    const error = new Error(`no connection after ${this.#options.connectTimeout} ms`);
    this.#emit('error', { error });
    if (generation === this.#generation) {
      this.#failAttempt(generation, this.#ws, { code: 1006, reason: 'connect timeout', wasClean: false });
    }
  }

  #clearConnectTimer() {
    if (this.#connectTimer !== null) {
      clearTimeout(this.#connectTimer);
      this.#connectTimer = null;
    }
  }

  async #urlWithTicket() {
    const { ticket, ticketParam } = this.#options;
    const value = typeof ticket === 'function' ? await ticket() : ticket;
    if (value === undefined || value === null || value === '') {
      return this.#url;
    }
    const url = new URL(this.#url);
    url.searchParams.set(ticketParam, String(value));
    return url.toString();
  }

  #handleOpen(generation) {
    if (generation !== this.#generation) {
      return;
    }
    this.#clearConnectTimer();
    const reconnect = this.#everOpened;
    this.#everOpened = true;
    this.#state = 'open';
    this.#openedAt = Date.now();
    for (const room of this.#rooms) {
      this.#ws.send(this.#options.joinMessage(room));
    }
    for (const data of this.#queue.splice(0)) {
      this.#ws.send(data);
    }
    this.#emit('open', { reconnect });
  }

  #handleClose(generation, event) {
    if (generation !== this.#generation) {
      return;
    }
    this.#clearConnectTimer();
    const wasOpen = this.#state === 'open';
    this.#ws = null;
    const lasted = wasOpen ? Date.now() - this.#openedAt : 0;
    if (lasted >= this.#options.resetAfter) {
      this.#attempt = 0; // a stable connection: the next failure starts the backoff over
      this.#retries = 0;
    }
    const info = { code: event?.code ?? 1006, reason: event?.reason ?? '', wasClean: Boolean(event?.wasClean) };
    const willRetry = this.#mayRetry(info);
    this.#emit('close', { ...info, willRetry });
    if (generation !== this.#generation) {
      return; // a listener called close() (or connect()): what happens next is not ours to schedule
    }
    if (willRetry) {
      this.#scheduleRetry(info);
    } else {
      this.#state = 'closed';
      this.#unwatchOnline();
    }
  }

  #mayRetry(info) {
    const { shouldReconnect, maxRetries } = this.#options;
    if (NO_RETRY_CODES.has(info.code) || this.#retries >= maxRetries) {
      return false;
    }
    return shouldReconnect ? Boolean(shouldReconnect(info)) : true;
  }

  #scheduleRetry(info) {
    this.#clearConnectTimer();
    if (this.#retries >= this.#options.maxRetries) {
      this.#state = 'closed';
      this.#unwatchOnline();
      this.#emit('error', { error: new Error('gave up reconnecting (maxRetries)') });
      return;
    }
    const delay = this.#nextDelay();
    this.#attempt += 1;
    this.#retries += 1;
    this.#state = 'waiting';
    this.#emit('reconnecting', { attempt: this.#retries, delay, closeInfo: info });
    this.#timer = setTimeout(() => {
      this.#timer = null;
      this.#attemptConnection();
    }, delay);
  }

  /** min * factor^attempt, capped, then jittered to 50-100% so a crowd of clients does not return in step. */
  #nextDelay() {
    const { min, max, factor } = this.#options.backoff;
    const base = Math.min(max, min * factor ** this.#attempt);
    const jitter = this.#options.jitter ? this.#options.jitter() : 0.5 + Math.random() / 2; // [0.5, 1]
    return Math.round(base * jitter);
  }

  #clearTimer() {
    if (this.#timer !== null) {
      clearTimeout(this.#timer);
      this.#timer = null;
    }
  }

  // ---- the browser coming back online -------------------------------------------------------------

  #watchOnline() {
    if (!this.#options.reconnectOnOnline || this.#onlineHandler || typeof globalThis.addEventListener !== 'function') {
      return;
    }
    this.#onlineHandler = () => {
      if (this.#state === 'waiting') {
        this.#clearTimer();
        this.#attemptConnection(); // the network is back: do not sit out the rest of the backoff
      }
    };
    globalThis.addEventListener('online', this.#onlineHandler);
  }

  #unwatchOnline() {
    if (this.#onlineHandler) {
      globalThis.removeEventListener('online', this.#onlineHandler);
      this.#onlineHandler = null;
    }
  }

  #emit(type, detail) {
    for (const listener of [...(this.#listeners.get(type) ?? [])]) {
      try {
        listener(detail);
      } catch (error) {
        // A listener's bug must not break the connection loop; surface it without recursing on 'error'.
        if (type !== 'error') {
          this.#emit('error', { error });
        }
      }
    }
  }
}

// SPDX-License-Identifier: Apache-2.0

export interface BackoffOptions {
  /** First delay in milliseconds (default 500). */
  min?: number;
  /** The delay never grows beyond this (default 15000). */
  max?: number;
  /** Growth per failed attempt (default 2). */
  factor?: number;
}

export interface CloseInfo {
  code: number;
  reason: string;
  wasClean: boolean;
}

export interface WsrvClientOptions {
  /** A string, or a function (may be async) called before EVERY connection attempt. Sent as a query parameter. */
  ticket?: string | (() => string | Promise<string>);
  /** Query parameter that carries the ticket (default "ticket"). */
  ticketParam?: string;
  protocols?: string | string[];
  backoff?: BackoffOptions;
  /** A connection that stayed open this long (ms) makes the next failure start the backoff over (default 10000). */
  resetAfter?: number;
  /** Give up after this many consecutive attempts without a stable connection (default Infinity). */
  maxRetries?: number;
  /** Return false to stop reconnecting after a close. Code 1008 never reconnects. */
  shouldReconnect?: (info: CloseInfo) => boolean;
  /** Keep up to maxQueue messages sent while offline and send them after the next connect (default false). */
  queueWhileOffline?: boolean;
  maxQueue?: number;
  /** Try at once when the browser fires "online" instead of finishing the backoff (default true). */
  reconnectOnOnline?: boolean;
  /** What join(room) sends (default `join ${room}`): the wire format belongs to your worker. */
  joinMessage?: (room: string) => string;
  leaveMessage?: (room: string) => string;
  /** For tests: returns the jitter factor in [0.5, 1] applied to each delay (default: random). */
  jitter?: () => number;
  /** Another WebSocket implementation (default: the global one). */
  WebSocket?: typeof WebSocket;
}

export interface OpenEvent { reconnect: boolean }
export interface MessageEvent { data: string | ArrayBuffer | Blob }
export interface CloseEvent extends CloseInfo { willRetry: boolean }
export interface ReconnectingEvent { attempt: number; delay: number; closeInfo: CloseInfo | null }
export interface ErrorEvent { error: unknown }

export interface WsrvClientEvents {
  open: OpenEvent;
  message: MessageEvent;
  close: CloseEvent;
  reconnecting: ReconnectingEvent;
  error: ErrorEvent;
}

export type WsrvClientState = 'idle' | 'connecting' | 'open' | 'waiting' | 'closed';

export class WsrvClient {
  constructor(url: string, options?: WsrvClientOptions);
  readonly state: WsrvClientState;
  /** Rooms that will be (re)joined whenever the client is connected. */
  readonly rooms: string[];
  connect(): void;
  /** True if sent (or queued, with queueWhileOffline); false if dropped. */
  send(data: string | ArrayBufferLike | Blob | ArrayBufferView): boolean;
  join(room: string): void;
  leave(room: string): void;
  close(code?: number, reason?: string): void;
  on<K extends keyof WsrvClientEvents>(type: K, listener: (event: WsrvClientEvents[K]) => void): () => void;
  off<K extends keyof WsrvClientEvents>(type: K, listener: (event: WsrvClientEvents[K]) => void): void;
}

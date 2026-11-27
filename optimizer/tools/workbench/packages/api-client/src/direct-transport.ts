// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { writable } from 'svelte/store';
import type { Readable } from 'svelte/store';
import type { ApiTransport, Unsubscribe } from './transport.js';
import type { ApiErrorResponse } from './types.js';

/**
 * How long a WebSocket connection must stay open, with no message at all,
 * before it is treated as proof of life for reconnect-backoff purposes (see
 * createSocket). Comfortably above the worker's default auth timeout (2s),
 * so it only ever credits a connection the worker has actually accepted.
 * Exported so tests can reference it instead of duplicating the number.
 */
export const PROOF_OF_LIFE_MS = 5000;

/**
 * Custom error class for API errors returned by the backend.
 */
export class ApiError extends Error {
  readonly code: string;
  readonly details?: Record<string, unknown>;
  readonly status: number;

  constructor(status: number, body: ApiErrorResponse) {
    super(body.error.message);
    this.name = 'ApiError';
    this.code = body.error.code;
    this.details = body.error.details;
    this.status = status;
  }
}

/** Options for constructing a DirectTransport. */
export interface DirectTransportOptions {
  /** Base URL for HTTP requests (default: '' i.e. relative to origin). */
  baseUrl?: string;
  /** Bearer token for authentication. */
  token?: string;
  /** Maximum reconnect backoff in milliseconds (default: 30000). */
  maxReconnectMs?: number;
}

/**
 * Transport implementation for the web console, using fetch() for HTTP
 * requests and WebSocket for streaming subscriptions.
 */
export class DirectTransport implements ApiTransport {
  private readonly baseUrl: string;
  private token: string | undefined;
  private readonly maxReconnectMs: number;

  private readonly _connected = writable(false);
  private readonly _error = writable<string | null>(null);

  /** Active WebSocket connections keyed by path. */
  private sockets = new Map<
    string,
    { ws: WebSocket; handlers: Set<(msg: unknown) => void> }
  >();

  /** Reconnect attempt counter per path (for exponential backoff). */
  private reconnectAttempts = new Map<string, number>();

  /** Reconnect timers so we can cancel on disconnect. */
  private reconnectTimers = new Map<string, ReturnType<typeof setTimeout>>();

  /** Whether the transport has been explicitly disconnected. */
  private disconnected = false;

  get connected(): Readable<boolean> {
    return this._connected;
  }

  get error(): Readable<string | null> {
    return this._error;
  }

  constructor(options: DirectTransportOptions = {}) {
    this.baseUrl = options.baseUrl ?? '';
    this.token = options.token;
    this.maxReconnectMs = options.maxReconnectMs ?? 30_000;
  }

  /**
   * Update the auth token. WebSocket subscriptions authenticate by sending the
   * token as their first message on open, so a token supplied *after* a socket
   * connected (the usual case: the user pastes a token into the login form
   * while unauthenticated sockets are already open) never reaches the worker
   * and the stream stays dead. Reconnect the live sockets so each re-handshakes
   * with the new token.
   */
  setToken(token: string | undefined): void {
    this.token = token;
    this.reconnectSockets();
  }

  /**
   * Close and immediately re-open every active WebSocket so it re-authenticates
   * with the current token, preserving each path's handlers. The replacement
   * socket is installed before the old one is closed, so the old socket's
   * `close` handler sees that it is no longer the active socket for its path
   * and does not schedule a duplicate reconnect.
   */
  private reconnectSockets(): void {
    for (const [path, entry] of this.sockets) {
      const timer = this.reconnectTimers.get(path);
      if (timer) {
        clearTimeout(timer);
        this.reconnectTimers.delete(path);
      }
      this.reconnectAttempts.set(path, 0);
      const { handlers, ws: oldWs } = entry;
      const ws = this.createSocket(path, handlers);
      this.sockets.set(path, { ws, handlers });
      oldWs.close(1000, 'token changed');
    }
  }

  // -- ApiTransport implementation ------------------------------------------

  async connect(): Promise<void> {
    this.disconnected = false;
    this._error.set(null);
    // Verify reachability with a health check.
    try {
      await this.get('/v1/health');
      this._connected.set(true);
    } catch (err) {
      const msg = err instanceof Error ? err.message : String(err);
      this._error.set(msg);
      this._connected.set(false);
      throw err;
    }
  }

  disconnect(): void {
    this.disconnected = true;
    this._connected.set(false);
    // Tear down all WebSocket connections and pending reconnect timers.
    for (const [, timer] of this.reconnectTimers) {
      clearTimeout(timer);
    }
    this.reconnectTimers.clear();
    this.reconnectAttempts.clear();
    for (const [, entry] of this.sockets) {
      entry.handlers.clear();
      entry.ws.close(1000, 'client disconnect');
    }
    this.sockets.clear();
  }

  async get<T>(path: string, params?: Record<string, string>): Promise<T> {
    const url = this.buildHttpUrl(path, params);
    const res = await fetch(url, { headers: this.authHeaders() });
    return this.handleResponse<T>(res);
  }

  async getBlob(
    path: string,
    params?: Record<string, string>,
  ): Promise<Response> {
    const url = this.buildHttpUrl(path, params);
    const res = await fetch(url, { headers: this.authHeaders() });
    if (!res.ok) {
      throw new ApiError(res.status, {
        error: {
          code: 'UNKNOWN',
          message: res.statusText || `HTTP ${res.status}`,
        },
      });
    }
    return res;
  }

  async patch<T>(path: string, body: object): Promise<T> {
    const url = this.buildHttpUrl(path);
    const res = await fetch(url, {
      method: 'PATCH',
      headers: {
        ...this.authHeaders(),
        'Content-Type': 'application/json',
        // CSRF: declares this as an XHR. The worker requires this on every
        // mutating /v1/* request (see src/worker/http_server.cc CSRF check);
        // we set it on every mutating request for transport uniformity.
        'X-Requested-With': 'XMLHttpRequest',
      },
      body: JSON.stringify(body),
    });
    return this.handleResponse<T>(res);
  }

  async post<T>(path: string, body: object): Promise<T> {
    const url = this.buildHttpUrl(path);
    const res = await fetch(url, {
      method: 'POST',
      headers: {
        ...this.authHeaders(),
        'Content-Type': 'application/json',
        // CSRF: required by the worker on mutating /v1/* requests. The header
        // makes the request a non-simple CORS request, blocking the
        // form-submission / no-preflight attack surface the worker guards
        // against. See src/worker/http_server.cc.
        'X-Requested-With': 'XMLHttpRequest',
      },
      body: JSON.stringify(body),
    });
    return this.handleResponse<T>(res);
  }

  subscribe(path: string, handler: (msg: unknown) => void): Unsubscribe {
    const existing = this.sockets.get(path);
    if (existing) {
      existing.handlers.add(handler);
      return () => {
        existing.handlers.delete(handler);
        if (existing.handlers.size === 0) {
          this.teardownSocket(path);
        }
      };
    }

    // Create a new WebSocket connection for this path. Every stream opens
    // immediately regardless of whether a token is set: plenty of real
    // deployments serve every stream, logs included, with no token at all
    // (the unix socket, --api-no-auth, the in-process ASP.NET host), and
    // those consoles never acquire one -- holding a stream back until a
    // token appeared left exactly those deployments silent forever. A
    // deployment where the stream genuinely needs a token the client does
    // not have yet costs one rejected attempt (see the 4001 handling in
    // createSocket below), not a dead stream.
    const handlers = new Set<(msg: unknown) => void>();
    handlers.add(handler);
    const ws = this.createSocket(path, handlers);
    this.sockets.set(path, { ws, handlers });

    return () => {
      handlers.delete(handler);
      if (handlers.size === 0) {
        this.teardownSocket(path);
      }
    };
  }

  // -- Internal helpers -----------------------------------------------------

  private buildHttpUrl(
    path: string,
    params?: Record<string, string>,
  ): string {
    const url = new URL(path, this.baseUrl || globalThis.location?.origin);
    if (params) {
      for (const [k, v] of Object.entries(params)) {
        url.searchParams.set(k, v);
      }
    }
    return url.toString();
  }

  private authHeaders(): Record<string, string> {
    if (this.token) {
      return { Authorization: `Bearer ${this.token}` };
    }
    return {};
  }

  private async handleResponse<T>(res: Response): Promise<T> {
    if (!res.ok) {
      let body: ApiErrorResponse;
      try {
        body = (await res.json()) as ApiErrorResponse;
      } catch {
        throw new ApiError(res.status, {
          error: {
            code: 'UNKNOWN',
            message: res.statusText || `HTTP ${res.status}`,
          },
        });
      }
      throw new ApiError(res.status, body);
    }
    return (await res.json()) as T;
  }

  private buildWsUrl(path: string): string {
    // If baseUrl is provided and absolute, derive ws:// from it.
    // Otherwise derive from current page origin.
    let origin: string;
    if (this.baseUrl && /^https?:\/\//.test(this.baseUrl)) {
      origin = this.baseUrl;
    } else {
      origin = globalThis.location?.origin ?? 'http://localhost';
    }
    const wsProto = origin.startsWith('https') ? 'wss' : 'ws';
    const hostAndPath = origin.replace(/^https?:\/\//, '');
    return `${wsProto}://${hostAndPath}${path}`;
  }

  private createSocket(
    path: string,
    handlers: Set<(msg: unknown) => void>,
  ): WebSocket {
    const url = this.buildWsUrl(path);
    const ws = new WebSocket(url);
    // Proof of life for THIS socket, guarding the one-time backoff reset
    // below. Two ways to earn it: a message (an auth_ok, a snapshot, any
    // real traffic), or simply staying open for PROOF_OF_LIFE_MS without
    // being closed -- a pre-authenticated stream with nothing to report
    // yet (events, between notifications) sends no message at all, and
    // without this it would never reset, so a later drop+reconnect would
    // keep climbing a backoff it never needed. PROOF_OF_LIFE_MS is well
    // above the worker's auth timeout (2s by default), so a connection the
    // worker is about to reject for authentication is already closed with
    // 4001 (handled below, not reconnected) long before the timer could
    // mistake it for proof of life.
    let provedLive = false;
    let liveTimer: ReturnType<typeof setTimeout> | undefined;

    const markLive = () => {
      if (provedLive) return;
      provedLive = true;
      if (liveTimer !== undefined) {
        clearTimeout(liveTimer);
        liveTimer = undefined;
      }
      this.reconnectAttempts.set(path, 0);
    };

    ws.addEventListener('open', () => {
      // Deliberately does NOT reset reconnectAttempts here. The 101
      // handshake always fires `open`, even for a connection the worker is
      // about to reject for authentication -- resetting on `open` alone
      // made the backoff for a stream that fails auth every time sit at a
      // constant ~1s instead of growing. The counter resets once this
      // connection proves itself instead (markLive, above).
      if (this.token) {
        ws.send(JSON.stringify({ auth: this.token }));
      }
      liveTimer = setTimeout(markLive, PROOF_OF_LIFE_MS);
    });

    ws.addEventListener('message', (ev: MessageEvent) => {
      markLive();
      try {
        const data: unknown = JSON.parse(String(ev.data));
        for (const h of handlers) {
          h(data);
        }
      } catch {
        // Ignore unparseable messages.
      }
    });

    ws.addEventListener('close', (ev: CloseEvent) => {
      if (liveTimer !== undefined) {
        clearTimeout(liveTimer);
        liveTimer = undefined;
      }
      // Only the socket that is still the active one for this path may drive
      // reconnection. If it has been replaced (e.g. by a token re-auth) its
      // close is expected and must not schedule a duplicate reconnect.
      if (this.sockets.get(path)?.ws !== ws) return;
      if (this.disconnected || handlers.size === 0) return;
      // 4001 is the worker's "authentication rejected or timed out" close
      // (see src/worker/ws_handlers.cc). Reconnecting immediately would
      // just repeat the same failure, re-occupying a server-side
      // pre-authentication slot on every cycle. Stay closed until the
      // token changes -- setToken() reopens it then via reconnectSockets().
      if (ev.code === 4001) return;
      this.scheduleReconnect(path, handlers);
    });

    ws.addEventListener('error', () => {
      // The close event will follow; reconnect logic lives there.
    });

    return ws;
  }

  private scheduleReconnect(
    path: string,
    handlers: Set<(msg: unknown) => void>,
  ): void {
    const attempt = (this.reconnectAttempts.get(path) ?? 0) + 1;
    this.reconnectAttempts.set(path, attempt);
    const backoff = Math.min(1000 * Math.pow(2, attempt - 1), this.maxReconnectMs);

    const timer = setTimeout(() => {
      this.reconnectTimers.delete(path);
      if (this.disconnected || handlers.size === 0) return;
      const ws = this.createSocket(path, handlers);
      this.sockets.set(path, { ws, handlers });
    }, backoff);

    this.reconnectTimers.set(path, timer);
  }

  private teardownSocket(path: string): void {
    const timer = this.reconnectTimers.get(path);
    if (timer) {
      clearTimeout(timer);
      this.reconnectTimers.delete(path);
    }
    this.reconnectAttempts.delete(path);
    const entry = this.sockets.get(path);
    if (entry) {
      entry.handlers.clear();
      entry.ws.close(1000, 'unsubscribed');
      this.sockets.delete(path);
    }
  }
}

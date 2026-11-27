// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { get } from 'svelte/store';
import {
  DirectTransport,
  ApiError,
  PROOF_OF_LIFE_MS,
} from '../direct-transport.js';

// ---------------------------------------------------------------------------
// Minimal WebSocket mock
// ---------------------------------------------------------------------------

type WsEventType = 'open' | 'close' | 'message' | 'error';

class MockWebSocket {
  static instances: MockWebSocket[] = [];

  url: string;
  readyState = 0; // CONNECTING
  sent: string[] = [];

  private listeners = new Map<WsEventType, Set<(ev: unknown) => void>>();

  constructor(url: string) {
    this.url = url;
    MockWebSocket.instances.push(this);
  }

  addEventListener(type: string, fn: (ev: unknown) => void): void {
    if (!this.listeners.has(type as WsEventType)) {
      this.listeners.set(type as WsEventType, new Set());
    }
    this.listeners.get(type as WsEventType)!.add(fn);
  }

  removeEventListener(type: string, fn: (ev: unknown) => void): void {
    this.listeners.get(type as WsEventType)?.delete(fn);
  }

  send(data: string): void {
    this.sent.push(data);
  }

  close(_code?: number, _reason?: string): void {
    this.readyState = 3; // CLOSED
  }

  // Test helpers to simulate server events.
  simulateOpen(): void {
    this.readyState = 1; // OPEN
    for (const fn of this.listeners.get('open') ?? []) fn({});
  }

  simulateMessage(data: unknown): void {
    for (const fn of this.listeners.get('message') ?? []) {
      fn({ data: JSON.stringify(data) } as unknown);
    }
  }

  simulateClose(code?: number): void {
    this.readyState = 3;
    for (const fn of this.listeners.get('close') ?? []) fn({ code });
  }

  simulateError(): void {
    for (const fn of this.listeners.get('error') ?? []) fn({});
  }
}

// ---------------------------------------------------------------------------
// Test setup
// ---------------------------------------------------------------------------

let transport: DirectTransport;

beforeEach(() => {
  MockWebSocket.instances = [];
  vi.stubGlobal('WebSocket', MockWebSocket);
  vi.stubGlobal('fetch', vi.fn());
  transport = new DirectTransport({
    baseUrl: 'http://localhost:9090',
    token: 'test-token',
  });
});

afterEach(() => {
  transport.disconnect();
  vi.restoreAllMocks();
});

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

function mockFetchOk(body: unknown, status = 200): void {
  (globalThis.fetch as ReturnType<typeof vi.fn>).mockResolvedValueOnce({
    ok: status >= 200 && status < 300,
    status,
    statusText: 'OK',
    json: () => Promise.resolve(body),
  });
}

function mockFetchError(
  status: number,
  body: { error: { code: string; message: string } },
): void {
  (globalThis.fetch as ReturnType<typeof vi.fn>).mockResolvedValueOnce({
    ok: false,
    status,
    statusText: 'Bad Request',
    json: () => Promise.resolve(body),
  });
}

// ---------------------------------------------------------------------------
// HTTP request tests
// ---------------------------------------------------------------------------

describe('DirectTransport HTTP', () => {
  it('get() sends GET with auth header and parses JSON', async () => {
    const body = { status: 'ok', uptime_seconds: 10, connections: { active: 1, max: 8 }, inflight: 0, ready: true };
    mockFetchOk(body);

    const result = await transport.get('/v1/health');

    expect(result).toEqual(body);
    const call = (globalThis.fetch as ReturnType<typeof vi.fn>).mock.calls[0];
    expect(call[0]).toBe('http://localhost:9090/v1/health');
    expect(call[1].headers).toEqual({ Authorization: 'Bearer test-token' });
  });

  it('get() appends query params', async () => {
    mockFetchOk({ url: '/foo', hostname: 'h', alternates: [] });

    await transport.get('/v1/cache/alternates', { url: '/foo', hostname: 'h' });

    const call = (globalThis.fetch as ReturnType<typeof vi.fn>).mock.calls[0];
    const url = new URL(call[0] as string);
    expect(url.searchParams.get('url')).toBe('/foo');
    expect(url.searchParams.get('hostname')).toBe('h');
  });

  it('patch() sends PATCH with JSON body', async () => {
    const responseBody = { applied: { jpeg_quality: 90 }, rejected: {}, warnings: [], config: {} };
    mockFetchOk(responseBody);

    await transport.patch('/v1/config', { jpeg_quality: 90 });

    const call = (globalThis.fetch as ReturnType<typeof vi.fn>).mock.calls[0];
    expect(call[1].method).toBe('PATCH');
    expect(call[1].headers['Content-Type']).toBe('application/json');
    // CSRF: declares itself as an XHR so the worker can distinguish from
    // cross-origin form submissions.
    expect(call[1].headers['X-Requested-With']).toBe('XMLHttpRequest');
    expect(JSON.parse(call[1].body as string)).toEqual({ jpeg_quality: 90 });
  });

  it('post() sends POST with JSON body', async () => {
    mockFetchOk({ deleted: 5 });

    await transport.post('/v1/cache/purge', { url: '/foo' });

    const call = (globalThis.fetch as ReturnType<typeof vi.fn>).mock.calls[0];
    expect(call[1].method).toBe('POST');
    expect(call[1].headers['Content-Type']).toBe('application/json');
    // CSRF: the worker rejects mutating /v1/* requests that don't set
    // X-Requested-With: XMLHttpRequest. We set it on every POST so the
    // SPA transport stays uniform across endpoints.
    expect(call[1].headers['X-Requested-With']).toBe('XMLHttpRequest');
  });

  it('throws ApiError on non-ok response', async () => {
    mockFetchError(400, {
      error: { code: 'BAD_REQUEST', message: 'Missing param' },
    });

    await expect(transport.get('/v1/stats')).rejects.toThrow(ApiError);
    try {
      mockFetchError(400, {
        error: { code: 'BAD_REQUEST', message: 'Missing param' },
      });
      await transport.get('/v1/stats');
    } catch (err) {
      expect(err).toBeInstanceOf(ApiError);
      const apiErr = err as ApiError;
      expect(apiErr.code).toBe('BAD_REQUEST');
      expect(apiErr.status).toBe(400);
      expect(apiErr.message).toBe('Missing param');
    }
  });

  it('throws ApiError with UNKNOWN code when response body is not JSON', async () => {
    (globalThis.fetch as ReturnType<typeof vi.fn>).mockResolvedValueOnce({
      ok: false,
      status: 502,
      statusText: 'Bad Gateway',
      json: () => Promise.reject(new Error('not json')),
    });

    try {
      await transport.get('/v1/stats');
    } catch (err) {
      expect(err).toBeInstanceOf(ApiError);
      expect((err as ApiError).code).toBe('UNKNOWN');
      expect((err as ApiError).status).toBe(502);
    }
  });

  it('get() without token omits Authorization header', async () => {
    const noAuth = new DirectTransport({ baseUrl: 'http://localhost:9090' });
    mockFetchOk({ status: 'ok', uptime_seconds: 0, connections: { active: 0, max: 0 }, inflight: 0, ready: true });

    await noAuth.get('/v1/health');

    const call = (globalThis.fetch as ReturnType<typeof vi.fn>).mock.calls[0];
    expect(call[1].headers).toEqual({});
    noAuth.disconnect();
  });
});

// ---------------------------------------------------------------------------
// connect / disconnect
// ---------------------------------------------------------------------------

describe('DirectTransport connect/disconnect', () => {
  it('connect() sets connected=true on successful health check', async () => {
    mockFetchOk({ status: 'ok', uptime_seconds: 0, connections: { active: 0, max: 0 }, inflight: 0, ready: true });

    await transport.connect();

    expect(get(transport.connected)).toBe(true);
    expect(get(transport.error)).toBeNull();
  });

  it('connect() sets error on failure', async () => {
    (globalThis.fetch as ReturnType<typeof vi.fn>).mockRejectedValueOnce(
      new Error('Network error'),
    );

    await expect(transport.connect()).rejects.toThrow('Network error');
    expect(get(transport.connected)).toBe(false);
    expect(get(transport.error)).toBe('Network error');
  });

  it('disconnect() sets connected=false', async () => {
    mockFetchOk({ status: 'ok', uptime_seconds: 0, connections: { active: 0, max: 0 }, inflight: 0, ready: true });
    await transport.connect();

    transport.disconnect();

    expect(get(transport.connected)).toBe(false);
  });
});

// ---------------------------------------------------------------------------
// WebSocket subscription tests
// ---------------------------------------------------------------------------

describe('DirectTransport WebSocket', () => {
  it('subscribe() creates a WebSocket and sends auth on open', () => {
    const handler = vi.fn();
    transport.subscribe('/v1/ws/stats', handler);

    expect(MockWebSocket.instances).toHaveLength(1);
    const ws = MockWebSocket.instances[0];
    expect(ws.url).toBe('ws://localhost:9090/v1/ws/stats');

    // Simulate server accepting the connection.
    ws.simulateOpen();
    expect(ws.sent).toEqual([JSON.stringify({ auth: 'test-token' })]);
  });

  it('subscribe() delivers parsed messages to handler', () => {
    const handler = vi.fn();
    transport.subscribe('/v1/ws/stats', handler);

    const ws = MockWebSocket.instances[0];
    ws.simulateOpen();
    ws.simulateMessage({ type: 'snapshot', sequence: 0, data: {} });

    expect(handler).toHaveBeenCalledWith({
      type: 'snapshot',
      sequence: 0,
      data: {},
    });
  });

  it('multiple handlers on the same path share one WebSocket', () => {
    const h1 = vi.fn();
    const h2 = vi.fn();
    transport.subscribe('/v1/ws/stats', h1);
    transport.subscribe('/v1/ws/stats', h2);

    expect(MockWebSocket.instances).toHaveLength(1);

    const ws = MockWebSocket.instances[0];
    ws.simulateOpen();
    ws.simulateMessage({ type: 'delta', sequence: 1, data: { errors: { total: 5, origin_misconfiguration: 0 } } });

    expect(h1).toHaveBeenCalledTimes(1);
    expect(h2).toHaveBeenCalledTimes(1);
  });

  it('unsubscribe() removes handler; last unsub closes WebSocket', () => {
    const h1 = vi.fn();
    const h2 = vi.fn();
    const unsub1 = transport.subscribe('/v1/ws/stats', h1);
    const unsub2 = transport.subscribe('/v1/ws/stats', h2);

    const ws = MockWebSocket.instances[0];
    ws.simulateOpen();

    // Remove first handler - socket stays open.
    unsub1();
    ws.simulateMessage({ type: 'delta', sequence: 1, data: {} });
    expect(h1).not.toHaveBeenCalled();
    expect(h2).toHaveBeenCalledTimes(1);

    // Remove second handler - socket should close.
    unsub2();
    expect(ws.readyState).toBe(3); // CLOSED
  });

  it('disconnect() closes all WebSockets', () => {
    transport.subscribe('/v1/ws/stats', vi.fn());
    transport.subscribe('/v1/ws/events', vi.fn());

    expect(MockWebSocket.instances).toHaveLength(2);

    transport.disconnect();

    for (const ws of MockWebSocket.instances) {
      expect(ws.readyState).toBe(3);
    }
  });

  it('reconnects with exponential backoff on close', async () => {
    vi.useFakeTimers();
    const handler = vi.fn();
    transport.subscribe('/v1/ws/stats', handler);

    expect(MockWebSocket.instances).toHaveLength(1);
    const ws1 = MockWebSocket.instances[0];
    ws1.simulateOpen();

    // Simulate unexpected close.
    ws1.simulateClose();

    // After 1s (first backoff), a new WebSocket should be created.
    vi.advanceTimersByTime(1000);
    expect(MockWebSocket.instances).toHaveLength(2);

    // Close the second one.
    const ws2 = MockWebSocket.instances[1];
    ws2.simulateClose();

    // After 2s (second backoff), a third WebSocket should be created.
    vi.advanceTimersByTime(2000);
    expect(MockWebSocket.instances).toHaveLength(3);

    vi.useRealTimers();
  });

  it('setToken re-authenticates existing sockets with the new token', () => {
    const handler = vi.fn();
    transport.subscribe('/v1/ws/stats', handler);
    const ws1 = MockWebSocket.instances[0];
    ws1.simulateOpen();
    expect(ws1.sent).toEqual([JSON.stringify({ auth: 'test-token' })]);

    // A token supplied after the socket connected must reach the worker.
    transport.setToken('new-token');

    // Exactly one replacement socket is created for the same path.
    expect(MockWebSocket.instances).toHaveLength(2);
    const ws2 = MockWebSocket.instances[1];
    expect(ws2.url).toBe(ws1.url);
    expect(ws1.readyState).toBe(3); // old socket closed

    // The replacement authenticates with the new token and stays wired to the
    // original handler.
    ws2.simulateOpen();
    expect(ws2.sent).toEqual([JSON.stringify({ auth: 'new-token' })]);
    ws2.simulateMessage({ type: 'delta', sequence: 1, data: {} });
    expect(handler).toHaveBeenCalledWith({ type: 'delta', sequence: 1, data: {} });
  });

  it('does not double-reconnect when a replaced socket later fires close', () => {
    vi.useFakeTimers();
    transport.subscribe('/v1/ws/stats', vi.fn());
    const ws1 = MockWebSocket.instances[0];
    ws1.simulateOpen();

    transport.setToken('new-token');
    expect(MockWebSocket.instances).toHaveLength(2);

    // The old, replaced socket's delayed close must be a no-op.
    ws1.simulateClose();
    vi.advanceTimersByTime(60_000);
    expect(MockWebSocket.instances).toHaveLength(2);

    vi.useRealTimers();
  });

  it('does not reconnect after disconnect()', () => {
    vi.useFakeTimers();
    transport.subscribe('/v1/ws/stats', vi.fn());

    const ws = MockWebSocket.instances[0];
    ws.simulateOpen();
    transport.disconnect();
    ws.simulateClose();

    vi.advanceTimersByTime(60_000);
    // No new WebSocket should have been created.
    expect(MockWebSocket.instances).toHaveLength(1);

    vi.useRealTimers();
  });

  it('uses wss:// for https base URL', () => {
    const secureTransport = new DirectTransport({
      baseUrl: 'https://example.com',
      token: 'tok',
    });

    secureTransport.subscribe('/v1/ws/stats', vi.fn());

    const ws = MockWebSocket.instances[MockWebSocket.instances.length - 1];
    expect(ws.url).toBe('wss://example.com/v1/ws/stats');
    secureTransport.disconnect();
  });
});

// ---------------------------------------------------------------------------
// /v1/ws/logs: open it like any other stream; survive a token the client
// does not have
// ---------------------------------------------------------------------------
//
// The log stream is the one WebSocket the worker does not pre-authenticate
// under --api-read-open (see src/worker/ws_handlers.h read_open_streams),
// but plenty of real deployments serve it -- and every other stream -- with
// no token at all: the unix socket, --api-no-auth, the in-process ASP.NET
// host. Those consoles never acquire a token, so a client that held the log
// stream back until one appeared would leave the Debug Console and the
// /logs page empty forever in exactly those deployments. subscribe() treats
// /v1/ws/logs the same as every other path instead: open immediately. A
// deployment where the stream genuinely needs a token the client does not
// have yet costs one rejected attempt, not a dead stream, because of two
// other properties: a 4001 (auth rejected/timed out) close is terminal
// instead of being retried, and the reconnect backoff only resets once a
// connection proves itself (a message, or simply staying open a while), not
// merely on `open`.

describe('DirectTransport /v1/ws/logs with no token (tokenless-open server)', () => {
  it('opens /v1/ws/logs immediately with no token and delivers a snapshot', () => {
    const anon = new DirectTransport({ baseUrl: 'http://localhost:9090' });
    const handler = vi.fn();
    anon.subscribe('/v1/ws/logs', handler);

    expect(MockWebSocket.instances).toHaveLength(1);
    const ws = MockWebSocket.instances[0];
    expect(ws.url).toBe('ws://localhost:9090/v1/ws/logs');

    ws.simulateOpen();
    // No token configured anywhere -- nothing to send. The unix socket,
    // --api-no-auth, and the ASP.NET host all serve this stream with none.
    expect(ws.sent).toEqual([]);

    ws.simulateMessage({ type: 'snapshot', entries: [], total: 0 });
    expect(handler).toHaveBeenCalledWith({
      type: 'snapshot',
      entries: [],
      total: 0,
    });
    anon.disconnect();
  });

  it('other read-open streams still open immediately without a token', () => {
    const anon = new DirectTransport({ baseUrl: 'http://localhost:9090' });
    anon.subscribe('/v1/ws/stats', vi.fn());

    expect(MockWebSocket.instances).toHaveLength(1);
    anon.disconnect();
  });
});

describe('DirectTransport /v1/ws/logs: a token-requiring server', () => {
  it('a 4001 close is terminal: no reconnect is scheduled', () => {
    vi.useFakeTimers();
    transport.subscribe('/v1/ws/logs', vi.fn());
    const ws = MockWebSocket.instances[0];
    ws.simulateOpen();

    ws.simulateClose(4001);
    vi.advanceTimersByTime(60_000);

    expect(MockWebSocket.instances).toHaveLength(1);
    vi.useRealTimers();
  });

  it('after a 4001 close, setToken() reopens the stream (not a timer)', () => {
    vi.useFakeTimers();
    transport.subscribe('/v1/ws/logs', vi.fn());
    const ws1 = MockWebSocket.instances[0];
    ws1.simulateOpen();
    ws1.simulateClose(4001);
    vi.advanceTimersByTime(60_000);
    expect(MockWebSocket.instances).toHaveLength(1);

    transport.setToken('fresh-token');

    expect(MockWebSocket.instances).toHaveLength(2);
    vi.useRealTimers();
  });

  it('a non-4001 close still reconnects normally', () => {
    vi.useFakeTimers();
    transport.subscribe('/v1/ws/logs', vi.fn());
    const ws1 = MockWebSocket.instances[0];
    ws1.simulateOpen();

    ws1.simulateClose(1001);
    vi.advanceTimersByTime(1000);

    expect(MockWebSocket.instances).toHaveLength(2);
    vi.useRealTimers();
  });
});

// ---------------------------------------------------------------------------
// Reconnect backoff: reset on proof-of-life, not on the raw 101
// ---------------------------------------------------------------------------

describe('DirectTransport reconnect backoff reset timing', () => {
  it('open alone does not reset the backoff counter', () => {
    vi.useFakeTimers();
    transport.subscribe('/v1/ws/stats', vi.fn());
    const ws1 = MockWebSocket.instances[0];
    ws1.simulateOpen();
    ws1.simulateClose(1001); // attempt 1 -> 1000ms backoff

    vi.advanceTimersByTime(1000);
    expect(MockWebSocket.instances).toHaveLength(2);
    const ws2 = MockWebSocket.instances[1];
    ws2.simulateOpen(); // Must NOT reset the counter by itself.
    ws2.simulateClose(1001);

    // If `open` had reset the counter, this would reconnect at the 1000ms
    // mark (attempt 1 again). The fix keeps it at attempt 2 -> 2000ms.
    vi.advanceTimersByTime(1000);
    expect(MockWebSocket.instances).toHaveLength(2);
    vi.advanceTimersByTime(1000);
    expect(MockWebSocket.instances).toHaveLength(3);

    vi.useRealTimers();
  });

  it('the first message resets the backoff counter', () => {
    vi.useFakeTimers();
    transport.subscribe('/v1/ws/stats', vi.fn());
    const ws1 = MockWebSocket.instances[0];
    ws1.simulateOpen();
    ws1.simulateClose(1001); // attempt 1 -> 1000ms backoff
    vi.advanceTimersByTime(1000);

    const ws2 = MockWebSocket.instances[1];
    ws2.simulateOpen();
    ws2.simulateMessage({ type: 'snapshot', sequence: 0, data: {} });
    ws2.simulateClose(1001); // Reset by the message -> attempt 1 -> 1000ms.

    vi.advanceTimersByTime(1000);
    expect(MockWebSocket.instances).toHaveLength(3);

    vi.useRealTimers();
  });

  // A pre-authenticated stream with nothing to report (events, between
  // notifications) can stay open a long time without ever sending a
  // message. Without a time-based fallback, such a stream would never
  // reset its backoff, so a later drop would keep climbing an attempt
  // count it never needed.
  it('staying open without any message still resets the backoff eventually', () => {
    vi.useFakeTimers();
    transport.subscribe('/v1/ws/events', vi.fn());
    const ws1 = MockWebSocket.instances[0];
    ws1.simulateOpen();
    ws1.simulateClose(1001); // attempt 1 -> 1000ms backoff
    vi.advanceTimersByTime(1000);

    const ws2 = MockWebSocket.instances[1];
    ws2.simulateOpen();
    // No message at all -- just stay open past the proof-of-life window.
    vi.advanceTimersByTime(PROOF_OF_LIFE_MS);
    ws2.simulateClose(1001); // Reset by the timer -> attempt 1 -> 1000ms.

    vi.advanceTimersByTime(1000);
    expect(MockWebSocket.instances).toHaveLength(3);

    vi.useRealTimers();
  });

  it('closing before the proof-of-life window elapses does not reset the backoff', () => {
    vi.useFakeTimers();
    transport.subscribe('/v1/ws/events', vi.fn());
    const ws1 = MockWebSocket.instances[0];
    ws1.simulateOpen();
    ws1.simulateClose(1001); // attempt 1 -> 1000ms backoff
    vi.advanceTimersByTime(1000);

    const ws2 = MockWebSocket.instances[1];
    ws2.simulateOpen();
    // Closed well before the proof-of-life window -- the pending timer
    // must not fire later and must not be mistaken for proof of life.
    ws2.simulateClose(1001); // Still attempt 2 -> 2000ms backoff.

    vi.advanceTimersByTime(1000);
    expect(MockWebSocket.instances).toHaveLength(2);
    vi.advanceTimersByTime(1000);
    expect(MockWebSocket.instances).toHaveLength(3);

    vi.useRealTimers();
  });
});

// ---------------------------------------------------------------------------
// setToken
// ---------------------------------------------------------------------------

describe('DirectTransport.setToken()', () => {
  it('updates the token used for subsequent requests', async () => {
    mockFetchOk({ status: 'ok' });
    await transport.get('/v1/health');

    transport.setToken('new-token');

    mockFetchOk({ status: 'ok' });
    await transport.get('/v1/health');

    const calls = (globalThis.fetch as ReturnType<typeof vi.fn>).mock.calls;
    expect(calls[0][1].headers.Authorization).toBe('Bearer test-token');
    expect(calls[1][1].headers.Authorization).toBe('Bearer new-token');
  });
});

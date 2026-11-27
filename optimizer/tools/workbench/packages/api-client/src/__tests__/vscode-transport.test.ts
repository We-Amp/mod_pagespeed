// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { get } from 'svelte/store';
import { VsCodeTransport } from '../vscode-transport.js';
import type { VsCodeApi } from '../vscode-transport.js';

// ---------------------------------------------------------------------------
// Mock VsCodeApi
// ---------------------------------------------------------------------------

function createMockVsCodeApi(): VsCodeApi & { posted: unknown[] } {
  const posted: unknown[] = [];
  return {
    posted,
    postMessage(message: unknown): void {
      posted.push(message);
    },
  };
}

// ---------------------------------------------------------------------------
// Polyfill addEventListener/removeEventListener/dispatchEvent on globalThis
// for Node.js (vitest default environment).  The VsCodeTransport uses
// globalThis.addEventListener('message', ...) which is a browser API.
// ---------------------------------------------------------------------------

const eventTarget = new EventTarget();

beforeEach(() => {
  vi.stubGlobal(
    'addEventListener',
    eventTarget.addEventListener.bind(eventTarget),
  );
  vi.stubGlobal(
    'removeEventListener',
    eventTarget.removeEventListener.bind(eventTarget),
  );
  vi.stubGlobal(
    'dispatchEvent',
    eventTarget.dispatchEvent.bind(eventTarget),
  );
});

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

let uuidCounter = 0;

/** Simulate an inbound message from the extension host. */
function dispatchMessage(data: unknown): void {
  // Node.js has MessageEvent starting from v15.
  const event = new MessageEvent('message', { data });
  globalThis.dispatchEvent(event);
}

/** Find the last outbound message of the given type. */
function lastPosted(
  api: { posted: unknown[] },
  type: string,
): Record<string, unknown> | undefined {
  for (let i = api.posted.length - 1; i >= 0; i--) {
    const msg = api.posted[i] as Record<string, unknown>;
    if (msg?.type === type) return msg;
  }
  return undefined;
}

/** Find all outbound messages of the given type. */
function allPosted(
  api: { posted: unknown[] },
  type: string,
): Record<string, unknown>[] {
  return api.posted.filter(
    (m) => (m as Record<string, unknown>)?.type === type,
  ) as Record<string, unknown>[];
}

// ---------------------------------------------------------------------------
// Test setup
// ---------------------------------------------------------------------------

let api: ReturnType<typeof createMockVsCodeApi>;
let transport: VsCodeTransport;

beforeEach(() => {
  uuidCounter = 0;
  vi.stubGlobal(
    'crypto',
    Object.assign({}, globalThis.crypto, {
      randomUUID: vi.fn(() => `uuid-${++uuidCounter}`),
    }),
  );
  api = createMockVsCodeApi();
  transport = new VsCodeTransport(api, { timeoutMs: 100 });
});

afterEach(() => {
  transport.disconnect();
  vi.restoreAllMocks();
});

// ---------------------------------------------------------------------------
// Helper: connect the transport (installs listener + resolves health probe)
// ---------------------------------------------------------------------------

async function connectTransport(): Promise<void> {
  const connectPromise = transport.connect();
  // The health GET is the first UUID generated after the last reset.
  // After connect() is called, the health request id is the current uuidCounter.
  dispatchMessage({
    type: 'api-response',
    id: `uuid-${uuidCounter}`,
    ok: true,
    data: { status: 'ok' },
  });
  await connectPromise;
}

// ---------------------------------------------------------------------------
// HTTP request tests
// ---------------------------------------------------------------------------

describe('VsCodeTransport HTTP requests', () => {
  it('get() sends a GET api-request via postMessage', () => {
    // Fire the request but swallow the pending promise to avoid unhandled
    // rejection when afterEach disconnects.
    const p = transport.get('/v1/health');
    p.catch(() => {});

    const msg = lastPosted(api, 'api-request');
    expect(msg).toBeDefined();
    expect(msg!.method).toBe('GET');
    expect(msg!.path).toBe('/v1/health');
    expect(msg!.id).toBe('uuid-1');
    expect(msg!.params).toBeUndefined();
    expect(msg!.body).toBeUndefined();
  });

  it('get() includes query params when provided', () => {
    const p = transport.get('/v1/cache/alternates', {
      url: '/foo',
      hostname: 'h',
    });
    p.catch(() => {});

    const msg = lastPosted(api, 'api-request');
    expect(msg!.params).toEqual({ url: '/foo', hostname: 'h' });
  });

  it('patch() sends a PATCH api-request with body', () => {
    const p = transport.patch('/v1/config', { jpeg_quality: 90 });
    p.catch(() => {});

    const msg = lastPosted(api, 'api-request');
    expect(msg!.method).toBe('PATCH');
    expect(msg!.path).toBe('/v1/config');
    expect(msg!.body).toEqual({ jpeg_quality: 90 });
    expect(msg!.params).toBeUndefined();
  });

  it('post() sends a POST api-request with body', () => {
    const p = transport.post('/v1/cache/purge', { url: '/bar' });
    p.catch(() => {});

    const msg = lastPosted(api, 'api-request');
    expect(msg!.method).toBe('POST');
    expect(msg!.path).toBe('/v1/cache/purge');
    expect(msg!.body).toEqual({ url: '/bar' });
  });
});

// ---------------------------------------------------------------------------
// Response routing
// ---------------------------------------------------------------------------

describe('VsCodeTransport response routing', () => {
  beforeEach(async () => {
    await connectTransport();
  });

  it('resolves the correct pending promise by correlation ID', async () => {
    const p1 = transport.get<{ a: number }>('/v1/stats');
    const p2 = transport.get<{ b: number }>('/v1/config');

    // p1 gets uuid-2, p2 gets uuid-3 (uuid-1 was used by connect/health).
    dispatchMessage({
      type: 'api-response',
      id: 'uuid-3',
      ok: true,
      data: { b: 2 },
    });
    dispatchMessage({
      type: 'api-response',
      id: 'uuid-2',
      ok: true,
      data: { a: 1 },
    });

    const r1 = await p1;
    const r2 = await p2;
    expect(r1).toEqual({ a: 1 });
    expect(r2).toEqual({ b: 2 });
  });

  it('ignores responses with unknown IDs', async () => {
    const p = transport.get('/v1/stats');

    // Dispatch a response for a non-existent ID -- should be silently ignored.
    dispatchMessage({
      type: 'api-response',
      id: 'unknown-id',
      ok: true,
      data: { garbage: true },
    });

    // Now resolve the real request.
    dispatchMessage({
      type: 'api-response',
      id: 'uuid-2',
      ok: true,
      data: { real: true },
    });

    expect(await p).toEqual({ real: true });
  });
});

// ---------------------------------------------------------------------------
// Error responses
// ---------------------------------------------------------------------------

describe('VsCodeTransport error responses', () => {
  beforeEach(async () => {
    await connectTransport();
  });

  it('rejects with error message when ok=false', async () => {
    const p = transport.get('/v1/stats');

    dispatchMessage({
      type: 'api-response',
      id: 'uuid-2',
      ok: false,
      error: 'Not found',
    });

    await expect(p).rejects.toThrow('Not found');
  });

  it('rejects with "Unknown error" when ok=false and no error message', async () => {
    const p = transport.get('/v1/stats');

    dispatchMessage({
      type: 'api-response',
      id: 'uuid-2',
      ok: false,
    });

    await expect(p).rejects.toThrow('Unknown error');
  });
});

// ---------------------------------------------------------------------------
// Request timeout
// ---------------------------------------------------------------------------

describe('VsCodeTransport request timeout', () => {
  beforeEach(async () => {
    vi.useFakeTimers();
    await connectTransport();
  });

  afterEach(() => {
    vi.useRealTimers();
  });

  it('rejects after timeoutMs with a descriptive message', async () => {
    const p = transport.get('/v1/stats');

    vi.advanceTimersByTime(101);

    await expect(p).rejects.toThrow(
      'Request timed out after 100ms: GET /v1/stats',
    );
  });

  it('does not reject if response arrives before timeout', async () => {
    const p = transport.get('/v1/stats');

    vi.advanceTimersByTime(50);

    dispatchMessage({
      type: 'api-response',
      id: 'uuid-2',
      ok: true,
      data: { ok: true },
    });

    expect(await p).toEqual({ ok: true });

    // Advancing past the timeout should not cause issues.
    vi.advanceTimersByTime(200);
  });
});

// ---------------------------------------------------------------------------
// subscribe()
// ---------------------------------------------------------------------------

describe('VsCodeTransport subscribe()', () => {
  beforeEach(async () => {
    await connectTransport();
  });

  it('first handler sends ws-subscribe message', () => {
    const handler = vi.fn();
    transport.subscribe('/v1/ws/stats', handler);

    const msg = lastPosted(api, 'ws-subscribe');
    expect(msg).toBeDefined();
    expect(msg!.path).toBe('/v1/ws/stats');
    expect(msg!.id).toBe('uuid-2');
  });

  it('second handler on same path does NOT send another ws-subscribe', () => {
    transport.subscribe('/v1/ws/stats', vi.fn());
    transport.subscribe('/v1/ws/stats', vi.fn());

    const subscribeMsgs = allPosted(api, 'ws-subscribe');
    expect(subscribeMsgs).toHaveLength(1);
  });

  it('subscribing to a different path sends a separate ws-subscribe', () => {
    transport.subscribe('/v1/ws/stats', vi.fn());
    transport.subscribe('/v1/ws/events', vi.fn());

    const subscribeMsgs = allPosted(api, 'ws-subscribe');
    expect(subscribeMsgs).toHaveLength(2);
    expect(subscribeMsgs[0].path).toBe('/v1/ws/stats');
    expect(subscribeMsgs[1].path).toBe('/v1/ws/events');
  });
});

// ---------------------------------------------------------------------------
// unsubscribe()
// ---------------------------------------------------------------------------

describe('VsCodeTransport unsubscribe()', () => {
  beforeEach(async () => {
    await connectTransport();
  });

  it('removing last handler sends ws-unsubscribe', () => {
    const unsub = transport.subscribe('/v1/ws/stats', vi.fn());

    unsub();

    const msg = lastPosted(api, 'ws-unsubscribe');
    expect(msg).toBeDefined();
    expect(msg!.path).toBe('/v1/ws/stats');
  });

  it('removing non-last handler does NOT send ws-unsubscribe', () => {
    const h1 = vi.fn();
    const h2 = vi.fn();
    const unsub1 = transport.subscribe('/v1/ws/stats', h1);
    transport.subscribe('/v1/ws/stats', h2);

    unsub1();

    const unsubMsgs = allPosted(api, 'ws-unsubscribe');
    expect(unsubMsgs).toHaveLength(0);
  });

  it('removing both handlers sends ws-unsubscribe once', () => {
    const unsub1 = transport.subscribe('/v1/ws/stats', vi.fn());
    const unsub2 = transport.subscribe('/v1/ws/stats', vi.fn());

    unsub1();
    unsub2();

    const unsubMsgs = allPosted(api, 'ws-unsubscribe');
    expect(unsubMsgs).toHaveLength(1);
  });
});

// ---------------------------------------------------------------------------
// ws-message dispatch
// ---------------------------------------------------------------------------

describe('VsCodeTransport ws-message dispatch', () => {
  beforeEach(async () => {
    await connectTransport();
  });

  it('dispatches ws-message to correct path handlers', () => {
    const statsHandler = vi.fn();
    const eventsHandler = vi.fn();

    transport.subscribe('/v1/ws/stats', statsHandler);
    transport.subscribe('/v1/ws/events', eventsHandler);

    dispatchMessage({
      type: 'ws-message',
      path: '/v1/ws/stats',
      data: { cpu: 42 },
    });

    expect(statsHandler).toHaveBeenCalledWith({ cpu: 42 });
    expect(eventsHandler).not.toHaveBeenCalled();
  });

  it('dispatches to all handlers on the same path', () => {
    const h1 = vi.fn();
    const h2 = vi.fn();

    transport.subscribe('/v1/ws/stats', h1);
    transport.subscribe('/v1/ws/stats', h2);

    dispatchMessage({
      type: 'ws-message',
      path: '/v1/ws/stats',
      data: { mem: 100 },
    });

    expect(h1).toHaveBeenCalledWith({ mem: 100 });
    expect(h2).toHaveBeenCalledWith({ mem: 100 });
  });

  it('does not dispatch to unsubscribed handler', () => {
    const h1 = vi.fn();
    const h2 = vi.fn();

    const unsub1 = transport.subscribe('/v1/ws/stats', h1);
    transport.subscribe('/v1/ws/stats', h2);

    unsub1();

    dispatchMessage({
      type: 'ws-message',
      path: '/v1/ws/stats',
      data: { x: 1 },
    });

    expect(h1).not.toHaveBeenCalled();
    expect(h2).toHaveBeenCalledWith({ x: 1 });
  });

  it('ignores ws-message for paths with no subscriptions', () => {
    const handler = vi.fn();
    transport.subscribe('/v1/ws/stats', handler);

    // Message for a different path.
    dispatchMessage({
      type: 'ws-message',
      path: '/v1/ws/unknown',
      data: { y: 2 },
    });

    expect(handler).not.toHaveBeenCalled();
  });
});

// ---------------------------------------------------------------------------
// connection-status
// ---------------------------------------------------------------------------

describe('VsCodeTransport connection-status', () => {
  beforeEach(async () => {
    await connectTransport();
  });

  it('updates connected store on connection-status message', () => {
    dispatchMessage({
      type: 'connection-status',
      connected: false,
      error: 'Worker crashed',
    });

    expect(get(transport.connected)).toBe(false);
    expect(get(transport.error)).toBe('Worker crashed');
  });

  it('sets error to null when connected without error', () => {
    // First set an error.
    dispatchMessage({
      type: 'connection-status',
      connected: false,
      error: 'Bad connection',
    });
    expect(get(transport.error)).toBe('Bad connection');

    // Now clear it.
    dispatchMessage({
      type: 'connection-status',
      connected: true,
    });

    expect(get(transport.connected)).toBe(true);
    expect(get(transport.error)).toBeNull();
  });
});

// ---------------------------------------------------------------------------
// connect()
// ---------------------------------------------------------------------------

describe('VsCodeTransport connect()', () => {
  it('succeeds via health probe and sets connected=true', async () => {
    const connectPromise = transport.connect();

    // The health GET is uuid-1.
    dispatchMessage({
      type: 'api-response',
      id: 'uuid-1',
      ok: true,
      data: { status: 'ok' },
    });

    await connectPromise;

    expect(get(transport.connected)).toBe(true);
    expect(get(transport.error)).toBeNull();
  });

  it('throws and sets error on health probe failure', async () => {
    const connectPromise = transport.connect();

    dispatchMessage({
      type: 'api-response',
      id: 'uuid-1',
      ok: false,
      error: 'Worker unreachable',
    });

    await expect(connectPromise).rejects.toThrow('Worker unreachable');
    expect(get(transport.connected)).toBe(false);
    expect(get(transport.error)).toBe('Worker unreachable');
  });

  it('installs message listener only once on repeated connect()', async () => {
    const addSpy = vi.spyOn(globalThis, 'addEventListener' as never);

    const p1 = transport.connect();
    dispatchMessage({
      type: 'api-response',
      id: 'uuid-1',
      ok: true,
      data: { status: 'ok' },
    });
    await p1;

    const p2 = transport.connect();
    dispatchMessage({
      type: 'api-response',
      id: 'uuid-2',
      ok: true,
      data: { status: 'ok' },
    });
    await p2;

    const messageCalls = (addSpy as unknown as ReturnType<typeof vi.fn>).mock
      .calls.filter((c: unknown[]) => c[0] === 'message');
    expect(messageCalls).toHaveLength(1);

    (addSpy as unknown as ReturnType<typeof vi.fn>).mockRestore();
  });
});

// ---------------------------------------------------------------------------
// disconnect()
// ---------------------------------------------------------------------------

describe('VsCodeTransport disconnect()', () => {
  beforeEach(async () => {
    await connectTransport();
  });

  it('rejects all pending requests with "Transport disconnected"', async () => {
    const p1 = transport.get('/v1/stats');
    const p2 = transport.post('/v1/purge', {});

    transport.disconnect();

    await expect(p1).rejects.toThrow('Transport disconnected');
    await expect(p2).rejects.toThrow('Transport disconnected');
  });

  it('sends ws-unsubscribe for all active subscriptions', () => {
    transport.subscribe('/v1/ws/stats', vi.fn());
    transport.subscribe('/v1/ws/events', vi.fn());

    transport.disconnect();

    const unsubMsgs = allPosted(api, 'ws-unsubscribe');
    expect(unsubMsgs).toHaveLength(2);

    const paths = unsubMsgs.map((m) => m.path);
    expect(paths).toContain('/v1/ws/stats');
    expect(paths).toContain('/v1/ws/events');
  });

  it('removes the global message listener', () => {
    const removeSpy = vi.spyOn(
      globalThis,
      'removeEventListener' as never,
    );

    transport.disconnect();

    const removeCalls = (
      removeSpy as unknown as ReturnType<typeof vi.fn>
    ).mock.calls.filter((c: unknown[]) => c[0] === 'message');
    expect(removeCalls).toHaveLength(1);

    (removeSpy as unknown as ReturnType<typeof vi.fn>).mockRestore();
  });

  it('sets connected to false', () => {
    transport.disconnect();
    expect(get(transport.connected)).toBe(false);
  });

  it('does not dispatch ws-messages after disconnect', () => {
    const handler = vi.fn();
    transport.subscribe('/v1/ws/stats', handler);

    transport.disconnect();

    dispatchMessage({
      type: 'ws-message',
      path: '/v1/ws/stats',
      data: { late: true },
    });

    expect(handler).not.toHaveBeenCalled();
  });
});

// ---------------------------------------------------------------------------
// Malformed messages
// ---------------------------------------------------------------------------

describe('VsCodeTransport malformed messages', () => {
  beforeEach(async () => {
    await connectTransport();
  });

  it('ignores null message data', () => {
    expect(() => dispatchMessage(null)).not.toThrow();
  });

  it('ignores non-object message data (string)', () => {
    expect(() => dispatchMessage('hello')).not.toThrow();
  });

  it('ignores non-object message data (number)', () => {
    expect(() => dispatchMessage(42)).not.toThrow();
  });

  it('ignores object without type property', () => {
    expect(() =>
      dispatchMessage({ id: 'uuid-99', ok: true, data: {} }),
    ).not.toThrow();
  });

  it('ignores unknown type', () => {
    expect(() =>
      dispatchMessage({ type: 'not-a-real-type', data: {} }),
    ).not.toThrow();
  });

  it('pending requests are unaffected by malformed messages', async () => {
    const p = transport.get('/v1/stats');

    // Send a barrage of malformed messages.
    dispatchMessage(null);
    dispatchMessage(undefined);
    dispatchMessage('garbage');
    dispatchMessage({ noType: true });
    dispatchMessage({ type: 'unknown-type' });

    // Now send the real response.
    dispatchMessage({
      type: 'api-response',
      id: 'uuid-2',
      ok: true,
      data: { result: 'ok' },
    });

    expect(await p).toEqual({ result: 'ok' });
  });
});

// ---------------------------------------------------------------------------
// Default timeout
// ---------------------------------------------------------------------------

describe('VsCodeTransport default timeout', () => {
  it('uses 10s default when no timeoutMs is specified', async () => {
    vi.useFakeTimers();

    const defaultTransport = new VsCodeTransport(api);

    // Install listener via connect -- this issues GET /v1/health.
    const connectPromise = defaultTransport.connect();

    // Advance to just under 10s -- should not have timed out yet.
    vi.advanceTimersByTime(9_999);

    // Now push past 10s.
    vi.advanceTimersByTime(2);

    await expect(connectPromise).rejects.toThrow(
      'Request timed out after 10000ms: GET /v1/health',
    );

    defaultTransport.disconnect();
    vi.useRealTimers();
  });
});

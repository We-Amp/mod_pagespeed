// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { get } from 'svelte/store';
import { writable } from 'svelte/store';
import type { Readable } from 'svelte/store';
import type { ApiTransport, Unsubscribe } from '../transport.js';
import type { HealthResponse } from '../types.js';
import { ApiError } from '../direct-transport.js';
import { ConnectionManager } from '../connection.js';
import type { ConnectionManagerOptions } from '../connection.js';

// ---------------------------------------------------------------------------
// Mock transport
// ---------------------------------------------------------------------------

/** Minimal mock that records calls and lets tests control responses. */
class MockTransport implements ApiTransport {
  readonly _connected = writable(false);
  readonly _error = writable<string | null>(null);

  get connected(): Readable<boolean> {
    return this._connected;
  }
  get error(): Readable<string | null> {
    return this._error;
  }

  connectFn = vi.fn(async () => {});
  disconnectFn = vi.fn(() => {});
  getFn = vi.fn(async (_path: string, _params?: Record<string, string>) => {
    return {} as unknown;
  });
  patchFn = vi.fn(async (_path: string, _body: object) => {
    return {} as unknown;
  });
  postFn = vi.fn(async (_path: string, _body: object) => {
    return {} as unknown;
  });
  subscribeFn = vi.fn(
    (_path: string, _handler: (msg: unknown) => void): Unsubscribe => {
      return () => {};
    },
  );
  setTokenFn = vi.fn((_token: string | undefined) => {});

  async connect(): Promise<void> {
    return this.connectFn();
  }
  disconnect(): void {
    this.disconnectFn();
  }
  async get<T>(
    path: string,
    params?: Record<string, string>,
  ): Promise<T> {
    return this.getFn(path, params) as Promise<T>;
  }
  async patch<T>(path: string, body: object): Promise<T> {
    return this.patchFn(path, body) as Promise<T>;
  }
  async post<T>(path: string, body: object): Promise<T> {
    return this.postFn(path, body) as Promise<T>;
  }
  subscribe(path: string, handler: (msg: unknown) => void): Unsubscribe {
    return this.subscribeFn(path, handler);
  }
  setToken(token: string | undefined): void {
    this.setTokenFn(token);
  }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

const HEALTH_OK: HealthResponse = {
  status: 'ok',
  checks: {
    cache_open: { pass: true },
    cache_configured: { pass: true },
  },
  uptime_seconds: 42,
  connections: { active: 1, max: 100 },
  inflight: 0,
  ready: true,
};

const HEALTH_NOT_READY: HealthResponse = {
  ...HEALTH_OK,
  ready: false,
};

function make401(): ApiError {
  return new ApiError(401, {
    error: { code: 'UNAUTHORIZED', message: 'Token required' },
  });
}

function makeNetworkError(): Error {
  return new Error('fetch failed');
}

function createManager(
  transport: MockTransport,
  overrides?: Partial<ConnectionManagerOptions>,
): ConnectionManager {
  return new ConnectionManager({
    transport,
    healthPollIntervalMs: 100, // fast for tests
    maxReconnectDelayMs: 400,
    ...overrides,
  });
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

describe('ConnectionManager', () => {
  let transport: MockTransport;

  beforeEach(() => {
    vi.useFakeTimers();
    transport = new MockTransport();
  });

  afterEach(() => {
    vi.useRealTimers();
  });

  // ---- connect → health polling → connected state -----------------------

  describe('connect → connected', () => {
    it('transitions to connected when health check succeeds', async () => {
      // Health OK, stats OK.
      transport.getFn.mockImplementation(async (path: string) => {
        if (path === '/v1/health') return HEALTH_OK;
        if (path === '/v1/stats') return {};
        return {};
      });

      const mgr = createManager(transport);
      await mgr.connect();

      expect(get(mgr.state)).toBe('connected');
      expect(get(mgr.workerReady)).toBe(true);
      expect(get(mgr.healthData)).toEqual(HEALTH_OK);
      expect(get(mgr.authRequired)).toBe(false);

      mgr.disconnect();
    });

    it('reports workerReady=false when health.ready is false', async () => {
      transport.getFn.mockImplementation(async (path: string) => {
        if (path === '/v1/health') return HEALTH_NOT_READY;
        return {};
      });

      const mgr = createManager(transport);
      await mgr.connect();

      expect(get(mgr.state)).toBe('connected');
      expect(get(mgr.workerReady)).toBe(false);

      mgr.disconnect();
    });

    it('exposes the underlying transport', () => {
      const mgr = createManager(transport);
      expect(mgr.transport).toBe(transport);
    });
  });

  // ---- auth detection (401 response) ------------------------------------

  describe('auth detection', () => {
    it('sets authRequired when stats returns 401', async () => {
      transport.getFn.mockImplementation(async (path: string) => {
        if (path === '/v1/health') return HEALTH_OK;
        if (path === '/v1/stats') throw make401();
        return {};
      });

      const mgr = createManager(transport);
      await mgr.connect();

      expect(get(mgr.state)).toBe('connected');
      expect(get(mgr.authRequired)).toBe(true);

      mgr.disconnect();
    });

    it('sets authRequired and stays disconnected when health returns 401', async () => {
      transport.getFn.mockImplementation(async (path: string) => {
        if (path === '/v1/health') throw make401();
        return {};
      });

      const mgr = createManager(transport);
      await mgr.connect();

      expect(get(mgr.state)).toBe('disconnected');
      expect(get(mgr.authRequired)).toBe(true);

      mgr.disconnect();
    });
  });

  // ---- setToken → retry -------------------------------------------------

  describe('setToken', () => {
    it('clears authRequired when setToken succeeds', async () => {
      let callCount = 0;
      transport.getFn.mockImplementation(async (path: string) => {
        if (path === '/v1/health') return HEALTH_OK;
        if (path === '/v1/stats') {
          callCount++;
          if (callCount === 1) throw make401();
          return {}; // second call succeeds
        }
        return {};
      });

      const mgr = createManager(transport);
      await mgr.connect();
      expect(get(mgr.authRequired)).toBe(true);

      await mgr.setToken('valid-token');
      expect(get(mgr.authRequired)).toBe(false);
      expect(transport.setTokenFn).toHaveBeenCalledWith('valid-token');

      mgr.disconnect();
    });

    it('keeps authRequired when setToken re-probe still returns 401', async () => {
      transport.getFn.mockImplementation(async (path: string) => {
        if (path === '/v1/health') return HEALTH_OK;
        if (path === '/v1/stats') throw make401();
        return {};
      });

      const mgr = createManager(transport);
      await mgr.connect();
      expect(get(mgr.authRequired)).toBe(true);

      await mgr.setToken('bad-token');
      expect(get(mgr.authRequired)).toBe(true);

      mgr.disconnect();
    });
  });

  // ---- network error → error state → reconnect -------------------------

  describe('network error and reconnect', () => {
    it('transitions to error when health check fails', async () => {
      transport.getFn.mockRejectedValue(makeNetworkError());

      const mgr = createManager(transport);
      await mgr.connect();

      expect(get(mgr.state)).toBe('error');

      mgr.disconnect();
    });

    it('transitions to error when transport.connect() throws', async () => {
      transport.connectFn.mockRejectedValue(new Error('connection refused'));

      const mgr = createManager(transport);
      await mgr.connect();

      expect(get(mgr.state)).toBe('error');

      mgr.disconnect();
    });

    it('reconnects after network error with backoff', async () => {
      let callCount = 0;
      transport.getFn.mockImplementation(async (path: string) => {
        if (path === '/v1/health') {
          callCount++;
          if (callCount <= 2) throw makeNetworkError();
          return HEALTH_OK;
        }
        return {};
      });

      // Use a high maxReconnectDelayMs so the exponential backoff values
      // (1000ms, 2000ms) are not capped.
      const mgr = createManager(transport, {
        maxReconnectDelayMs: 30_000,
      });
      await mgr.connect();

      // After connect: first health call failed -> state=error, reconnect
      // scheduled at delay = 1000ms.
      expect(get(mgr.state)).toBe('error');

      // Advance past first reconnect delay (1000ms).
      await vi.advanceTimersByTimeAsync(1100);

      // Second attempt also fails -> still error, next delay = 2000ms.
      expect(get(mgr.state)).toBe('error');

      // Advance past second reconnect delay (2000ms).
      await vi.advanceTimersByTimeAsync(2100);

      // Third attempt succeeds.
      expect(get(mgr.state)).toBe('connected');

      mgr.disconnect();
    });

    it('recovers via periodic health polling after transient failure', async () => {
      let callCount = 0;
      transport.getFn.mockImplementation(async (path: string) => {
        if (path === '/v1/health') {
          callCount++;
          // First call succeeds (connect), second fails (poll), third
          // succeeds (reconnect).
          if (callCount === 2) throw makeNetworkError();
          return HEALTH_OK;
        }
        return {};
      });

      const mgr = createManager(transport);
      await mgr.connect();
      expect(get(mgr.state)).toBe('connected');

      // Trigger the first health poll interval (100ms).
      await vi.advanceTimersByTimeAsync(110);

      // The poll failed → state=error.
      expect(get(mgr.state)).toBe('error');

      // Reconnect scheduled at 1000ms delay.
      await vi.advanceTimersByTimeAsync(1100);

      // Third call succeeds → connected.
      expect(get(mgr.state)).toBe('connected');

      mgr.disconnect();
    });
  });

  // ---- disconnect stops polling -----------------------------------------

  describe('disconnect', () => {
    it('stops health polling', async () => {
      transport.getFn.mockImplementation(async (path: string) => {
        if (path === '/v1/health') return HEALTH_OK;
        return {};
      });

      const mgr = createManager(transport);
      await mgr.connect();

      const callsBefore = transport.getFn.mock.calls.length;
      mgr.disconnect();

      expect(get(mgr.state)).toBe('disconnected');
      expect(get(mgr.healthData)).toBeNull();
      expect(get(mgr.workerReady)).toBe(false);

      // Advance time well past several poll intervals.
      await vi.advanceTimersByTimeAsync(500);

      // No additional calls should have been made.
      expect(transport.getFn.mock.calls.length).toBe(callsBefore);
    });

    it('cancels pending reconnect', async () => {
      transport.getFn.mockRejectedValue(makeNetworkError());

      const mgr = createManager(transport);
      await mgr.connect();
      expect(get(mgr.state)).toBe('error');

      const callsBefore = transport.getFn.mock.calls.length;
      mgr.disconnect();

      // Advance past reconnect delay.
      await vi.advanceTimersByTimeAsync(5000);

      // No reconnect attempt was made.
      expect(transport.getFn.mock.calls.length).toBe(callsBefore);
      expect(get(mgr.state)).toBe('disconnected');
    });

    it('calls transport.disconnect()', async () => {
      transport.getFn.mockImplementation(async (path: string) => {
        if (path === '/v1/health') return HEALTH_OK;
        return {};
      });

      const mgr = createManager(transport);
      await mgr.connect();
      mgr.disconnect();

      expect(transport.disconnectFn).toHaveBeenCalled();
    });
  });

  // ---- initial state ----------------------------------------------------

  describe('initial state', () => {
    it('starts in disconnected state with null health data', () => {
      const mgr = createManager(transport);
      expect(get(mgr.state)).toBe('disconnected');
      expect(get(mgr.workerReady)).toBe(false);
      expect(get(mgr.healthData)).toBeNull();
      expect(get(mgr.authRequired)).toBe(false);
    });
  });

  // ---- maxReconnectDelayMs cap ------------------------------------------

  describe('backoff cap', () => {
    it('does not exceed maxReconnectDelayMs', async () => {
      transport.getFn.mockRejectedValue(makeNetworkError());

      const mgr = createManager(transport, { maxReconnectDelayMs: 400 });
      await mgr.connect();

      // First reconnect at 1000ms capped to 400ms. Advance 410ms.
      const callsAfterConnect = transport.getFn.mock.calls.length;
      await vi.advanceTimersByTimeAsync(410);

      // Should have attempted a reconnect.
      expect(transport.getFn.mock.calls.length).toBeGreaterThan(
        callsAfterConnect,
      );

      mgr.disconnect();
    });
  });
});

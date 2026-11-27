// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { writable } from 'svelte/store';
import type { Readable } from 'svelte/store';
import type { ApiTransport } from './transport.js';
import type { HealthResponse } from './types.js';
import { ApiError } from './direct-transport.js';

/** Possible states for the connection lifecycle. */
export type ConnectionState = 'disconnected' | 'connecting' | 'connected' | 'error';

/** Options for constructing a ConnectionManager. */
export interface ConnectionManagerOptions {
  transport: ApiTransport;
  /** Interval in ms between health polls when connected (default: 5000). */
  healthPollIntervalMs?: number;
  /** Maximum delay in ms between reconnect attempts (default: 30000). */
  maxReconnectDelayMs?: number;
}

/**
 * Shared connection management layer that wraps an ApiTransport.
 *
 * Provides health polling, auth detection, and exponential-backoff reconnect
 * logic. Both the web console and VS Code extension create a ConnectionManager
 * around their respective transport implementation.
 */
export class ConnectionManager {
  private readonly _transport: ApiTransport;
  private readonly healthPollIntervalMs: number;
  private readonly maxReconnectDelayMs: number;

  private readonly _state = writable<ConnectionState>('disconnected');
  private readonly _workerReady = writable(false);
  private readonly _healthData = writable<HealthResponse | null>(null);
  private readonly _authRequired = writable(false);
  private readonly _error = writable<string | null>(null);
  private readonly _reconnectAttempt = writable(0);

  /** Timer handle for the periodic health poll. */
  private healthTimer: ReturnType<typeof setInterval> | null = null;

  /** Timer handle for a pending reconnect attempt. */
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;

  /** Number of consecutive failed health poll attempts (for backoff). */
  private _reconnectAttemptCount = 0;

  /** Flag set when disconnect() is called to suppress further activity. */
  private stopped = false;

  // -- Public reactive state ------------------------------------------------

  get state(): Readable<ConnectionState> {
    return this._state;
  }

  get workerReady(): Readable<boolean> {
    return this._workerReady;
  }

  get healthData(): Readable<HealthResponse | null> {
    return this._healthData;
  }

  get authRequired(): Readable<boolean> {
    return this._authRequired;
  }

  get error(): Readable<string | null> {
    return this._error;
  }

  /** Current reconnect attempt count (0 when connected). */
  get reconnectAttempt(): Readable<number> {
    return this._reconnectAttempt;
  }

  /** Maximum backoff delay in ms (for UI countdown display). */
  get maxReconnectDelay(): number {
    return this.maxReconnectDelayMs;
  }

  /** Expose the underlying transport for components to make direct calls. */
  get transport(): ApiTransport {
    return this._transport;
  }

  constructor(options: ConnectionManagerOptions) {
    this._transport = options.transport;
    this.healthPollIntervalMs = options.healthPollIntervalMs ?? 5000;
    this.maxReconnectDelayMs = options.maxReconnectDelayMs ?? 30_000;
  }

  // -- Public API -----------------------------------------------------------

  /**
   * Connect to the worker.
   *
   * 1. Sets state to 'connecting'.
   * 2. Calls the transport's connect() method.
   * 3. Performs an initial health check.
   * 4. Probes /v1/stats to detect auth requirements.
   * 5. Starts periodic health polling.
   */
  async connect(): Promise<void> {
    this.stopped = false;
    this.setReconnectAttempt(0);
    this._state.set('connecting');
    this._authRequired.set(false);
    this._error.set(null);

    try {
      await this._transport.connect();
    } catch (err) {
      // Transport-level failure (network unreachable, etc.)
      const msg = err instanceof Error ? err.message : String(err);
      this._error.set(msg || 'Failed to connect');
      this._state.set('error');
      this.scheduleReconnect();
      return;
    }

    // Initial health check.
    const healthy = await this.pollHealth();
    if (this.stopped) return;

    if (!healthy) {
      // pollHealth already set state to 'error' or handled 401.
      this.scheduleReconnect();
      return;
    }

    // Probe stats to detect auth requirements.
    await this.probeAuth();

    // Start periodic health polling.
    this.startHealthPolling();
  }

  /** Disconnect and stop all polling / reconnect activity. */
  disconnect(): void {
    this.stopped = true;
    this.stopHealthPolling();
    this.cancelReconnect();
    this._transport.disconnect();
    this._state.set('disconnected');
    this._workerReady.set(false);
    this._healthData.set(null);
    this._error.set(null);
  }

  /**
   * Supply an auth token after receiving authRequired=true.
   * Forwards the token to the transport and re-probes auth.
   */
  async setToken(token: string): Promise<void> {
    // DirectTransport exposes setToken; VsCodeTransport handles auth via the
    // extension host. We call it if available.
    if (
      'setToken' in this._transport &&
      typeof (this._transport as Record<string, unknown>)['setToken'] ===
        'function'
    ) {
      (this._transport as unknown as { setToken(t: string): void }).setToken(
        token,
      );
    }

    // Re-probe auth to clear the authRequired flag.
    await this.probeAuth();
  }

  /** Update the reconnect attempt count and sync the public store. */
  private setReconnectAttempt(count: number): void {
    this._reconnectAttemptCount = count;
    this._reconnectAttempt.set(count);
  }

  // -- Internal: health polling ---------------------------------------------

  private startHealthPolling(): void {
    this.stopHealthPolling();
    this.healthTimer = setInterval(async () => {
      if (this.stopped) return;
      const healthy = await this.pollHealth();
      if (!healthy && !this.stopped) {
        this.stopHealthPolling();
        this.scheduleReconnect();
      }
    }, this.healthPollIntervalMs);
  }

  private stopHealthPolling(): void {
    if (this.healthTimer !== null) {
      clearInterval(this.healthTimer);
      this.healthTimer = null;
    }
  }

  /**
   * Perform a single health poll.
   * @returns true if the worker is healthy, false otherwise.
   */
  private async pollHealth(): Promise<boolean> {
    try {
      const health =
        await this._transport.get<HealthResponse>('/v1/health');
      this._healthData.set(health);
      this._workerReady.set(health.ready);
      this._state.set('connected');
      this._error.set(null);
      this.setReconnectAttempt(0);
      return true;
    } catch (err) {
      if (err instanceof ApiError && err.status === 401) {
        this._authRequired.set(true);
        this._state.set('disconnected');
        return false;
      }
      // Network or other error.
      const msg = err instanceof Error ? err.message : String(err);
      this._error.set(msg || 'Health check failed');
      this._state.set('error');
      return false;
    }
  }

  // -- Internal: auth probing -----------------------------------------------

  /**
   * Probe GET /v1/stats to determine whether authentication is required.
   * A 401 response sets authRequired=true; success clears it.
   */
  private async probeAuth(): Promise<void> {
    try {
      await this._transport.get('/v1/stats');
      this._authRequired.set(false);
    } catch (err) {
      if (err instanceof ApiError && err.status === 401) {
        this._authRequired.set(true);
      }
      // Other errors are non-fatal here -- we still consider ourselves
      // connected since health was OK.
    }
  }

  // -- Internal: reconnect with backoff ------------------------------------

  private scheduleReconnect(): void {
    if (this.stopped) return;
    this.cancelReconnect();

    this.setReconnectAttempt(this._reconnectAttemptCount + 1);
    const delay = Math.min(
      1000 * Math.pow(2, this._reconnectAttemptCount - 1),
      this.maxReconnectDelayMs,
    );

    this.reconnectTimer = setTimeout(async () => {
      this.reconnectTimer = null;
      if (this.stopped) return;

      this._state.set('connecting');

      const healthy = await this.pollHealth();
      if (this.stopped) return;

      if (healthy) {
        await this.probeAuth();
        this.startHealthPolling();
      } else {
        // Still failing -- schedule another attempt.
        this.scheduleReconnect();
      }
    }, delay);
  }

  private cancelReconnect(): void {
    if (this.reconnectTimer !== null) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
  }
}

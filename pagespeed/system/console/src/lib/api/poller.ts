// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * The console's one polling mechanism. Every page that refreshes data does
 * it through a Poller (via usePolling):
 *
 * - one request at a time: the next one is scheduled only after the
 *   previous one settled (a setTimeout chain, never setInterval);
 * - a longer wait after each failed refresh: the interval, then doubling,
 *   capped at a minute;
 * - HTTP 429 ("busy": the daemon proxy serves one read per endpoint at a
 *   time, usually taken by another tab) keeps the last view and is not a
 *   failure;
 * - nothing while the browser tab is hidden, and a refresh at once when it
 *   is shown again.
 *
 * Plain TypeScript with injectable timers and visibility, so it is
 * unit-tested without a browser; polling.svelte.ts adapts it to runes.
 */

import { ApiError } from "./client";

export interface PollerClock {
  setTimeout(fn: () => void, ms: number): unknown;
  clearTimeout(handle: unknown): void;
  now(): number;
}

export interface VisibilitySource {
  hidden(): boolean;
  /** Calls `onChange` whenever visibility may have changed; returns an unsubscribe. */
  subscribe(onChange: () => void): () => void;
}

export interface PollerOptions {
  intervalMs: number;
  /** Longest wait between attempts while failing (never below the interval). */
  maxBackoffMs?: number;
  /** Wait before retrying a busy answer while nothing is shown yet. */
  busyRetryMs?: number;
  clock?: PollerClock;
  /** `undefined`: the document's visibility; `null`: always visible. */
  visibility?: VisibilitySource | null;
  isBusy?: (error: Error) => boolean;
}

export interface PollerState<T> {
  data: T | null;
  error: Error | null;
  /** True until the first answer that is not "busy". */
  loading: boolean;
  autoRefresh: boolean;
  /** Consecutive failed refreshes; busy answers do not count. */
  failures: number;
  /** True while the tab is hidden and polling is suspended. */
  paused: boolean;
  /** Epoch ms of the next scheduled refresh, or null when none is scheduled. */
  nextAt: number | null;
}

export const DEFAULT_MAX_BACKOFF_MS = 60_000;
export const DEFAULT_BUSY_RETRY_MS = 1_000;

/** The wait after `failures` consecutive failures. */
export function backoffDelay(intervalMs: number, failures: number, maxMs: number = DEFAULT_MAX_BACKOFF_MS): number {
  if (failures <= 1) return intervalMs;
  const doubled = intervalMs * 2 ** Math.min(failures - 1, 16);
  return Math.min(Math.max(intervalMs, maxMs), doubled);
}

/** The daemon proxy's "one read per endpoint is already in flight". */
export function isBusyAnswer(error: Error): boolean {
  return error instanceof ApiError && error.status === 429;
}

// Looked up at call time, so tests that fake the global timers are honoured.
const realClock: PollerClock = {
  setTimeout: (fn, ms) => globalThis.setTimeout(fn, ms),
  clearTimeout: (handle) => globalThis.clearTimeout(handle as ReturnType<typeof setTimeout>),
  now: () => Date.now(),
};

function documentVisibility(): VisibilitySource | null {
  if (typeof document === "undefined") return null;
  return {
    hidden: () => document.visibilityState === "hidden",
    subscribe(onChange) {
      document.addEventListener("visibilitychange", onChange);
      return () => document.removeEventListener("visibilitychange", onChange);
    },
  };
}

type Outcome = "ok" | "busy" | "failed";

export class Poller<T> {
  private state: PollerState<T>;
  private timer: unknown = null;
  private inFlight: Promise<void> | null = null;
  private generation = 0;
  private rerun = false;
  private disposed = false;
  private lastOutcome: Outcome = "ok";
  private readonly clock: PollerClock;
  private readonly visibility: VisibilitySource | null;
  private readonly stopWatching: () => void;
  private readonly listeners = new Set<(s: PollerState<T>) => void>();

  constructor(
    private readonly fetcher: () => Promise<T>,
    private readonly options: PollerOptions,
  ) {
    this.clock = options.clock ?? realClock;
    this.visibility = options.visibility === undefined ? documentVisibility() : options.visibility;
    this.state = {
      data: null,
      error: null,
      loading: true,
      autoRefresh: false,
      failures: 0,
      paused: this.visibility?.hidden() ?? false,
      nextAt: null,
    };
    this.stopWatching = this.visibility ? this.visibility.subscribe(() => this.onVisibilityChange()) : () => {};
  }

  get snapshot(): PollerState<T> {
    return this.state;
  }

  subscribe(fn: (s: PollerState<T>) => void): () => void {
    this.listeners.add(fn);
    return () => {
      this.listeners.delete(fn);
    };
  }

  start(): void {
    if (this.disposed) return;
    this.patch({ autoRefresh: true });
    if (this.state.paused || this.inFlight !== null || this.timer !== null) return;
    void this.refresh();
  }

  stop(): void {
    this.clearTimer();
    this.patch({ autoRefresh: false, nextAt: null });
  }

  refresh(): Promise<void> {
    if (this.disposed) return Promise.resolve();
    if (this.inFlight !== null) return this.inFlight;
    this.clearTimer();
    const run = this.fetchOnce(this.generation).finally(() => {
      this.inFlight = null;
      if (this.rerun) {
        this.rerun = false;
        void this.refresh();
      } else {
        this.scheduleNext();
      }
    });
    this.inFlight = run;
    return run;
  }

  invalidate(): void {
    if (this.disposed) return;
    this.generation++;
    if (this.inFlight !== null) {
      this.rerun = true;
      return;
    }
    void this.refresh();
  }

  dispose(): void {
    if (this.disposed) return;
    this.disposed = true;
    this.clearTimer();
    this.stopWatching();
    this.listeners.clear();
  }

  private async fetchOnce(generation: number): Promise<void> {
    let data: T;
    try {
      data = await this.fetcher();
    } catch (err) {
      if (this.disposed || generation !== this.generation) return;
      const error = err instanceof Error ? err : new Error(String(err));
      if ((this.options.isBusy ?? isBusyAnswer)(error)) {
        this.lastOutcome = "busy";
        return;
      }
      this.lastOutcome = "failed";
      this.patch({ error, failures: this.state.failures + 1, loading: false });
      return;
    }
    if (this.disposed || generation !== this.generation) return;
    this.lastOutcome = "ok";
    this.patch({ data, error: null, failures: 0, loading: false });
  }

  private scheduleNext(): void {
    if (this.disposed || !this.state.autoRefresh || this.state.paused) {
      if (this.state.nextAt !== null) this.patch({ nextAt: null });
      return;
    }
    const delay = this.nextDelay();
    this.timer = this.clock.setTimeout(() => {
      this.timer = null;
      void this.refresh();
    }, delay);
    this.patch({ nextAt: this.clock.now() + delay });
  }

  private nextDelay(): number {
    const { intervalMs } = this.options;
    if (this.lastOutcome === "failed") {
      return backoffDelay(intervalMs, this.state.failures, this.options.maxBackoffMs);
    }
    if (this.lastOutcome === "busy" && this.state.data === null) {
      return Math.min(intervalMs, this.options.busyRetryMs ?? DEFAULT_BUSY_RETRY_MS);
    }
    return intervalMs;
  }

  private onVisibilityChange(): void {
    if (this.disposed) return;
    const hidden = this.visibility?.hidden() ?? false;
    if (hidden === this.state.paused) return;
    if (hidden) {
      this.clearTimer();
      this.patch({ paused: true, nextAt: null });
      return;
    }
    this.patch({ paused: false });
    if (this.state.autoRefresh && this.inFlight === null) void this.refresh();
  }

  private clearTimer(): void {
    if (this.timer !== null) {
      this.clock.clearTimeout(this.timer);
      this.timer = null;
    }
  }

  private patch(p: Partial<PollerState<T>>): void {
    this.state = { ...this.state, ...p };
    for (const fn of this.listeners) fn(this.state);
  }
}

export interface Refreshable {
  refresh(): Promise<void>;
}

/** Every poller of the pages currently shown, for "Retry now" and the refresh shortcut. */
export class PollerRegistry {
  private readonly members = new Set<Refreshable>();

  add(p: Refreshable): () => void {
    this.members.add(p);
    return () => {
      this.members.delete(p);
    };
  }

  get size(): number {
    return this.members.size;
  }

  refreshAll(): Promise<void> {
    return Promise.all([...this.members].map((p) => p.refresh())).then(() => undefined);
  }
}

export const pollers = new PollerRegistry();

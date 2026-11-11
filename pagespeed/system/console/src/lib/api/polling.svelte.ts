// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { onDestroy } from "svelte";
import { autoRefreshOn, onAutoRefreshChange } from "./auto-refresh";
import { Poller, pollers, type PollerOptions, type PollerState, type RefreshOutcome } from "./poller";

export interface PollingControls<T> {
  readonly data: T | null;
  readonly error: Error | null;
  readonly loading: boolean;
  readonly autoRefresh: boolean;
  readonly failures: number;
  readonly paused: boolean;
  readonly nextAt: number | null;
  refresh(): Promise<RefreshOutcome>;
  start(): void;
  stop(): void;
  /** The fetcher's inputs changed: drop an answer in flight and fetch again. */
  invalidate(): void;
  /** Changes the interval from here on (poller.ts, setIntervalMs). */
  setIntervalMs(intervalMs: number): void;
}

/**
 * Poll `fetcher` every `intervalMs` through the console's shared poller
 * (lib/api/poller.ts): one request at a time, back-off on failures, busy
 * answers kept as "try again", nothing while the tab is hidden. Starts at
 * once and stops when the component is destroyed; while the viewer has
 * paused auto-refresh (lib/api/auto-refresh.ts) it fetches once and waits.
 * Call it during component initialisation.
 */
export function usePolling<T>(
  fetcher: () => Promise<T>,
  intervalMs: number = 5000,
  options: Omit<PollerOptions, "intervalMs"> = {},
): PollingControls<T> {
  const poller = new Poller(fetcher, { ...options, intervalMs });
  let state = $state.raw<PollerState<T>>(poller.snapshot);
  poller.subscribe((s) => {
    state = s;
  });
  const unregister = pollers.add(poller);
  if (autoRefreshOn()) poller.start();
  else void poller.refresh();
  const unfollow = onAutoRefreshChange((on) => (on ? poller.start() : poller.stop()));
  onDestroy(() => {
    unregister();
    unfollow();
    poller.dispose();
  });

  return {
    get data() {
      return state.data;
    },
    get error() {
      return state.error;
    },
    get loading() {
      return state.loading;
    },
    get autoRefresh() {
      return state.autoRefresh;
    },
    get failures() {
      return state.failures;
    },
    get paused() {
      return state.paused;
    },
    get nextAt() {
      return state.nextAt;
    },
    refresh: () => poller.refresh(),
    start: () => poller.start(),
    stop: () => poller.stop(),
    invalidate: () => poller.invalidate(),
    setIntervalMs: (intervalMs) => poller.setIntervalMs(intervalMs),
  };
}

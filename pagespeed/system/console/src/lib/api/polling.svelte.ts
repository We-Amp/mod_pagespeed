import { onDestroy } from "svelte";

export interface PollingState<T> {
  data: T | null;
  error: Error | null;
  loading: boolean;
  autoRefresh: boolean;
}

export interface PollingControls<T> extends PollingState<T> {
  refresh: () => Promise<void>;
  start: () => void;
  stop: () => void;
}

/**
 * Svelte 5 rune-based polling composable.
 *
 * Returns reactive state that auto-refreshes data from `fetcher`
 * every `intervalMs` milliseconds.
 */
export function usePolling<T>(
  fetcher: () => Promise<T>,
  intervalMs: number = 5000,
): PollingControls<T> {
  let data: T | null = $state(null);
  let error: Error | null = $state(null);
  let loading: boolean = $state(true);
  let autoRefresh: boolean = $state(true);
  let timer: ReturnType<typeof setInterval> | null = null;

  async function refresh(): Promise<void> {
    try {
      loading = data === null;
      const result = await fetcher();
      data = result;
      error = null;
    } catch (err) {
      error = err instanceof Error ? err : new Error(String(err));
    } finally {
      loading = false;
    }
  }

  function start(): void {
    autoRefresh = true;
    if (timer !== null) return;
    refresh();
    timer = setInterval(refresh, intervalMs);
  }

  function stop(): void {
    autoRefresh = false;
    if (timer !== null) {
      clearInterval(timer);
      timer = null;
    }
  }

  // Start polling immediately.
  start();

  // Clean up on component destroy.
  onDestroy(() => {
    stop();
  });

  return {
    get data() {
      return data;
    },
    get error() {
      return error;
    },
    get loading() {
      return loading;
    },
    get autoRefresh() {
      return autoRefresh;
    },
    refresh,
    start,
    stop,
  };
}

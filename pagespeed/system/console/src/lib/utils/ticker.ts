// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * A once-a-period tick for text that ages on screen ("updated 12 s ago").
 * It is a clock, not a poller: it makes no request. Like the poller it is a
 * timeout chain (each tick schedules the next) and it does nothing while the
 * browser tab is hidden; when the tab is shown again it ticks at once, so the
 * text is right the moment it can be read.
 *
 * Plain TypeScript with injectable timers and visibility, so it is
 * unit-tested without a browser; lib/now.svelte.ts adapts it to runes.
 */

import type { PollerClock, VisibilitySource } from "$lib/api/poller";

export class Ticker {
  private timer: unknown = null;
  private onTick: (() => void) | null = null;
  private stopWatching: () => void = () => {};

  constructor(
    private readonly periodMs: number,
    private readonly clock: Pick<PollerClock, "setTimeout" | "clearTimeout">,
    private readonly visibility: VisibilitySource | null,
  ) {}

  get running(): boolean {
    return this.onTick !== null;
  }

  /** Start ticking; a second start replaces the first. Returns the stop function. */
  start(onTick: () => void): () => void {
    this.stop();
    this.onTick = onTick;
    this.stopWatching = this.visibility ? this.visibility.subscribe(() => this.onVisibilityChange()) : () => {};
    if (!(this.visibility?.hidden() ?? false)) this.schedule();
    return () => this.stop();
  }

  stop(): void {
    this.clearTimer();
    this.stopWatching();
    this.stopWatching = () => {};
    this.onTick = null;
  }

  private schedule(): void {
    this.timer = this.clock.setTimeout(() => {
      this.timer = null;
      this.onTick?.();
      // onTick may have stopped the ticker, or started it again.
      if (this.onTick !== null && this.timer === null) this.schedule();
    }, this.periodMs);
  }

  private onVisibilityChange(): void {
    if (this.onTick === null) return;
    if (this.visibility?.hidden() ?? false) {
      this.clearTimer();
      return;
    }
    if (this.timer !== null) return;
    this.onTick();
    if (this.onTick !== null && this.timer === null) this.schedule();
  }

  private clearTimer(): void {
    if (this.timer !== null) {
      this.clock.clearTimeout(this.timer);
      this.timer = null;
    }
  }
}

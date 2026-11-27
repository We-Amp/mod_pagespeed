// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import type { StatsResponse } from '@pagespeed/api-client';

// ---------------------------------------------------------------------------
// Alert rule & active alert types
// ---------------------------------------------------------------------------

export interface AlertRule {
  id: string;
  label: string;
  severity: 'warning' | 'error';
  check: (stats: StatsResponse, prevStats: StatsResponse | null) => boolean;
  message: (stats: StatsResponse, prevStats: StatsResponse | null) => string;
  /**
   * Rate-based rules fire on the tick where `check` first returns true, then
   * stay active for this many milliseconds after the most recent tick on which
   * `check` returned true (a hold-down window). This stops a rule that only
   * triggers on the single tick where a counter increments from strobing on and
   * off every poll. Omit (or 0) for level-based rules whose `check` already
   * reflects the current condition and so is stable on its own.
   */
  holdDownMs?: number;
}

export interface ActiveAlert {
  rule: AlertRule;
  /** Pre-evaluated message string for the current stats snapshot. */
  message: string;
  triggeredAt: Date;
  dismissed: boolean;
}

// ---------------------------------------------------------------------------
// Default alert rules
// ---------------------------------------------------------------------------

export const DEFAULT_ALERT_RULES: AlertRule[] = [
  {
    id: 'error-rate',
    label: 'Error Rate',
    severity: 'warning',
    holdDownMs: 30_000,
    check: (stats, prev) =>
      prev !== null && stats.errors.total > prev.errors.total,
    message: (stats) =>
      `Worker error count is rising — ${stats.errors.total} error(s) total. Check the Debug Console for details.`,
  },
  {
    id: 'origin-misconfiguration',
    label: 'Origin Misconfiguration',
    severity: 'warning',
    check: (stats) => stats.errors.origin_misconfiguration > 0,
    message: (stats) =>
      `Origin sent ${stats.errors.origin_misconfiguration} compressed response(s). Add \`proxy_set_header Accept-Encoding "";\` to your nginx config.`,
  },
  {
    id: 'write-failures',
    label: 'Write Failures',
    severity: 'error',
    holdDownMs: 30_000,
    check: (stats, prev) =>
      prev !== null &&
      stats.alternates.write_failures > prev.alternates.write_failures,
    message: (stats) =>
      `${stats.alternates.write_failures} variant write failure(s). This may indicate disk pressure or permission issues.`,
  },
  {
    id: 'thread-saturation',
    label: 'Thread Saturation',
    severity: 'warning',
    check: (stats) =>
      stats.thread_pool.size > 0 &&
      stats.thread_pool.inflight >= stats.thread_pool.size,
    message: (stats) =>
      `All worker threads are busy (${stats.thread_pool.inflight}/${stats.thread_pool.size}). Processing will resume when current jobs complete.`,
  },
  {
    id: 'chrome-stopped',
    label: 'Chrome Stopped',
    severity: 'warning',
    check: (stats) =>
      stats.browser?.enabled === true &&
      stats.browser?.chrome_running === false,
    message: () =>
      'Browser analysis enabled but Chrome is not running. Falling back to heuristic analysis.',
  },
  {
    id: 'connection-saturation',
    label: 'Connection Saturation',
    severity: 'warning',
    check: (stats) =>
      stats.connections.max > 0 &&
      stats.connections.active / stats.connections.max > 0.9,
    message: (stats) =>
      `Connection usage at ${Math.round((stats.connections.active / stats.connections.max) * 100)}% (${stats.connections.active}/${stats.connections.max}). IPC notifications may be dropped.`,
  },
];

// ---------------------------------------------------------------------------
// AlertManager
// ---------------------------------------------------------------------------

export class AlertManager {
  private rules: AlertRule[];
  private dismissed = new Set<string>();
  private prevStats: StatsResponse | null = null;
  /**
   * Active alert episodes, keyed by rule id. `firstAt` is when the episode
   * began (a stable `triggeredAt`); `lastFireAt` is the most recent tick on
   * which `check` returned true (drives the hold-down window).
   */
  private episodes = new Map<string, { firstAt: number; lastFireAt: number }>();

  constructor(rules: AlertRule[] = DEFAULT_ALERT_RULES) {
    this.rules = rules;
  }

  /**
   * Evaluate all rules against the current stats snapshot and return the
   * non-dismissed active alerts.
   *
   * Rate-based rules (those with a `holdDownMs`) compare against the previous
   * snapshot and fire on the tick where their counter increments. To stop them
   * strobing on and off every poll, an alert stays active for `holdDownMs`
   * after the most recent increment; only when that window lapses with no
   * further increments does the episode end. Level-based rules (no `holdDownMs`)
   * are active exactly while their `check` is currently true. On the first call
   * (no previous snapshot) rate-based rules never fire.
   *
   * `triggeredAt` is pinned to the start of the current episode, so it does not
   * jump forward on every evaluation while an alert stays up. When an episode
   * ends, any dismissal for that rule is cleared, so a fresh occurrence alerts
   * again.
   *
   * `now` is injectable (epoch ms) for deterministic tests; defaults to the
   * wall clock.
   */
  evaluate(stats: StatsResponse, now: number = Date.now()): ActiveAlert[] {
    const active: ActiveAlert[] = [];

    for (const rule of this.rules) {
      const firing = rule.check(stats, this.prevStats);
      const holdDownMs = rule.holdDownMs ?? 0;
      let episode = this.episodes.get(rule.id);

      if (firing) {
        if (episode === undefined) {
          episode = { firstAt: now, lastFireAt: now };
        } else {
          episode.lastFireAt = now;
        }
        this.episodes.set(rule.id, episode);
      } else if (
        episode !== undefined &&
        holdDownMs > 0 &&
        now - episode.lastFireAt < holdDownMs
      ) {
        // Within the hold-down window — keep the episode (and alert) alive.
      } else {
        // Episode ended (or was never held): drop it and clear any dismissal so
        // the rule can alert again on a fresh occurrence.
        this.episodes.delete(rule.id);
        this.dismissed.delete(rule.id);
        episode = undefined;
      }

      if (episode !== undefined) {
        active.push({
          rule,
          message: rule.message(stats, this.prevStats),
          triggeredAt: new Date(episode.firstAt),
          dismissed: this.dismissed.has(rule.id),
        });
      }
    }

    this.prevStats = stats;
    return active.filter((a) => !a.dismissed);
  }

  /** Dismiss a specific alert by rule ID. */
  dismiss(id: string): void {
    this.dismissed.add(id);
  }

  /** Clear all dismissals. */
  reset(): void {
    this.dismissed.clear();
  }
}

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, it, expect } from 'vitest';
import type { StatsResponse } from '@pagespeed/api-client';
import { AlertManager, DEFAULT_ALERT_RULES } from '../alerts';
import type { AlertRule } from '../alerts';

/** Build a minimal StatsResponse with optional overrides. */
function makeStats(overrides: Partial<StatsResponse> = {}): StatsResponse {
  return {
    notifications: { received: 0, skipped_dedup: 0, skipped_inflight: 0, rejected_version: 0, rejected_malformed: 0, rejected_sentinel: 0 },
    variants: { written: 0, proactive: 0, gzip: 0, brotli: 0 },
    errors: { total: 0, origin_misconfiguration: 0 },
    cache: { entries: 0, size_bytes: 0 },
    by_type: {
      html: { count: 0, time_us: 0 },
      css: { count: 0, time_us: 0 },
      js: { count: 0, time_us: 0 },
      image: { count: 0, time_us: 0 },
    },
    by_format: { webp: 0, avif: 0, jpeg: 0, png: 0, svg: 0 },
    svg: {
      candidates_evaluated: 0, candidates_rejected: 0, vectorized: 0,
      fidelity_rejected: 0, timeout_exceeded: 0, size_rejected: 0,
      path_count_rejected: 0, written: 0, bytes_saved: 0,
      vectorize_time_us: 0, served: 0,
    },
    content_analysis: { photo: 0, screenshot: 0, illustration: 0, noisy: 0, denoised: 0 },
    ssimulacra2: { checks: 0, declines: 0, tombstone_hits: 0, reencodes: 0, avg_score_x100: 0 },
    learned_quality: { predictions: 0, fallbacks: 0 },
    html_assembly: { complete: 0, skipped: 0, css_aborted: 0 },
    alternates: { writes: 0, write_failures: 0 },
    selector_invocations: 0,
    cache_read_retries: 0,
    cache_read_deferred_retries: 0,
    cache_read_deferred_successes: 0,
    cache_read_failures: 0,
    cache_auto_heals: 0,
    cache_auto_heal_exhausted: 0,
    image_incomplete_matrices: 0,
    image_no_savings_skipped: 0,
    image_unconverted_fallthrough: 0,
    dedup: { writes_skipped: 0, content_hash_hits: 0, content_hash_stale: 0 },
    quality_baselining: { capped_jpeg: 0, skip_reencode: 0 },
    thread_pool: { inflight: 0, size: 4 },
    connections: { active: 0, max: 128 },
    ...overrides,
  };
}

describe('DEFAULT_ALERT_RULES', () => {
  it('contains exactly 6 rules', () => {
    expect(DEFAULT_ALERT_RULES).toHaveLength(6);
  });

  it('has unique IDs', () => {
    const ids = DEFAULT_ALERT_RULES.map((r) => r.id);
    expect(new Set(ids).size).toBe(ids.length);
  });
});

describe('AlertManager', () => {
  it('returns no alerts for healthy stats', () => {
    const mgr = new AlertManager();
    const alerts = mgr.evaluate(makeStats());
    expect(alerts).toHaveLength(0);
  });

  describe('error-rate (rate-based)', () => {
    it('does not fire on first evaluation', () => {
      const mgr = new AlertManager();
      const alerts = mgr.evaluate(
        makeStats({ errors: { total: 5, origin_misconfiguration: 0 } }),
      );
      expect(alerts).toHaveLength(0);
    });

    it('fires when errors increase between evaluations', () => {
      const mgr = new AlertManager();
      mgr.evaluate(makeStats({ errors: { total: 2, origin_misconfiguration: 0 } }));
      const alerts = mgr.evaluate(
        makeStats({ errors: { total: 5, origin_misconfiguration: 0 } }),
      );
      expect(alerts).toHaveLength(1);
      expect(alerts[0].rule.id).toBe('error-rate');
      expect(alerts[0].rule.severity).toBe('warning');
      expect(alerts[0].message).toContain('5 error(s)');
    });

    it('does not fire when errors stay the same', () => {
      const mgr = new AlertManager();
      mgr.evaluate(makeStats({ errors: { total: 3, origin_misconfiguration: 0 } }));
      const alerts = mgr.evaluate(
        makeStats({ errors: { total: 3, origin_misconfiguration: 0 } }),
      );
      expect(alerts).toHaveLength(0);
    });
  });

  describe('write-failures (rate-based)', () => {
    it('does not fire on first evaluation', () => {
      const mgr = new AlertManager();
      const alerts = mgr.evaluate(
        makeStats({ alternates: { writes: 100, write_failures: 3 } }),
      );
      expect(alerts).toHaveLength(0);
    });

    it('fires when write failures increase', () => {
      const mgr = new AlertManager();
      mgr.evaluate(makeStats({ alternates: { writes: 100, write_failures: 1 } }));
      const alerts = mgr.evaluate(
        makeStats({ alternates: { writes: 200, write_failures: 3 } }),
      );
      expect(alerts).toHaveLength(1);
      expect(alerts[0].rule.id).toBe('write-failures');
      expect(alerts[0].rule.severity).toBe('error');
      expect(alerts[0].message).toContain('3 variant write failure(s)');
    });

    it('does not fire when write failures stay the same', () => {
      const mgr = new AlertManager();
      mgr.evaluate(makeStats({ alternates: { writes: 50, write_failures: 2 } }));
      const alerts = mgr.evaluate(
        makeStats({ alternates: { writes: 80, write_failures: 2 } }),
      );
      expect(alerts).toHaveLength(0);
    });
  });

  it('fires thread-saturation when inflight >= size', () => {
    const mgr = new AlertManager();
    const alerts = mgr.evaluate(makeStats({ thread_pool: { inflight: 8, size: 8 } }));
    expect(alerts).toHaveLength(1);
    expect(alerts[0].rule.id).toBe('thread-saturation');
    expect(alerts[0].rule.severity).toBe('warning');
    expect(alerts[0].message).toContain('All worker threads are busy');
  });

  it('does not fire thread-saturation when pool size is 0', () => {
    const mgr = new AlertManager();
    const alerts = mgr.evaluate(makeStats({ thread_pool: { inflight: 0, size: 0 } }));
    const saturation = alerts.find((a) => a.rule.id === 'thread-saturation');
    expect(saturation).toBeUndefined();
  });

  describe('chrome-stopped', () => {
    it('fires when browser enabled but chrome not running', () => {
      const mgr = new AlertManager();
      const stats = makeStats();
      (stats as any).browser = {
        enabled: true,
        chrome_running: false,
        analysis_errors: 0,
        chrome_crashes: 0,
      };
      const alerts = mgr.evaluate(stats);
      expect(alerts).toHaveLength(1);
      expect(alerts[0].rule.id).toBe('chrome-stopped');
      expect(alerts[0].message).toContain('Chrome is not running');
    });

    it('does not fire when browser is disabled', () => {
      const mgr = new AlertManager();
      const stats = makeStats();
      (stats as any).browser = {
        enabled: false,
        chrome_running: false,
        analysis_errors: 0,
        chrome_crashes: 0,
      };
      const alerts = mgr.evaluate(stats);
      const chrome = alerts.find((a) => a.rule.id === 'chrome-stopped');
      expect(chrome).toBeUndefined();
    });

    it('does not fire when chrome is running', () => {
      const mgr = new AlertManager();
      const stats = makeStats();
      (stats as any).browser = {
        enabled: true,
        chrome_running: true,
        analysis_errors: 0,
        chrome_crashes: 0,
      };
      const alerts = mgr.evaluate(stats);
      const chrome = alerts.find((a) => a.rule.id === 'chrome-stopped');
      expect(chrome).toBeUndefined();
    });
  });

  describe('connection-saturation', () => {
    it('fires when connections > 90% of max', () => {
      const mgr = new AlertManager();
      const alerts = mgr.evaluate(makeStats({ connections: { active: 120, max: 128 } }));
      expect(alerts).toHaveLength(1);
      expect(alerts[0].rule.id).toBe('connection-saturation');
      expect(alerts[0].message).toContain('Connection usage at');
    });

    it('does not fire below 90%', () => {
      const mgr = new AlertManager();
      const alerts = mgr.evaluate(makeStats({ connections: { active: 50, max: 128 } }));
      const conn = alerts.find((a) => a.rule.id === 'connection-saturation');
      expect(conn).toBeUndefined();
    });

    it('does not fire when max is 0', () => {
      const mgr = new AlertManager();
      const alerts = mgr.evaluate(makeStats({ connections: { active: 0, max: 0 } }));
      const conn = alerts.find((a) => a.rule.id === 'connection-saturation');
      expect(conn).toBeUndefined();
    });
  });

  it('can fire multiple alerts at once', () => {
    const mgr = new AlertManager();
    // First eval to establish baseline for rate-based alerts
    mgr.evaluate(makeStats({ errors: { total: 0, origin_misconfiguration: 0 } }));
    const alerts = mgr.evaluate(
      makeStats({
        errors: { total: 3, origin_misconfiguration: 0 },
        thread_pool: { inflight: 4, size: 4 },
      }),
    );
    expect(alerts).toHaveLength(2);
    const ids = alerts.map((a) => a.rule.id).sort();
    expect(ids).toEqual(['error-rate', 'thread-saturation']);
  });

  describe('dismiss', () => {
    it('hides a dismissed alert from evaluate results', () => {
      const mgr = new AlertManager();
      mgr.evaluate(makeStats({ errors: { total: 0, origin_misconfiguration: 0 } }));
      let alerts = mgr.evaluate(
        makeStats({ errors: { total: 1, origin_misconfiguration: 0 } }),
      );
      expect(alerts).toHaveLength(1);

      mgr.dismiss('error-rate');
      alerts = mgr.evaluate(
        makeStats({ errors: { total: 2, origin_misconfiguration: 0 } }),
      );
      expect(alerts).toHaveLength(0);
    });

    it('keeps a dismissal for the whole episode, then re-fires after hold-down lapses', () => {
      const mgr = new AlertManager();

      // Fire and dismiss.
      mgr.evaluate(makeStats({ errors: { total: 0, origin_misconfiguration: 0 } }), 0);
      mgr.evaluate(makeStats({ errors: { total: 5, origin_misconfiguration: 0 } }), 1000);
      mgr.dismiss('error-rate');
      // Still the same episode (within hold-down) and dismissed → hidden.
      expect(
        mgr.evaluate(makeStats({ errors: { total: 6, origin_misconfiguration: 0 } }), 2000),
      ).toHaveLength(0);

      // No further increments; let the 30s hold-down window lapse. The episode
      // ends and the dismissal is cleared.
      mgr.evaluate(makeStats({ errors: { total: 6, origin_misconfiguration: 0 } }), 40000);

      // A fresh increment starts a new episode and alerts again.
      const alerts = mgr.evaluate(
        makeStats({ errors: { total: 8, origin_misconfiguration: 0 } }), 41000,
      );
      expect(alerts).toHaveLength(1);
      expect(alerts[0].rule.id).toBe('error-rate');
    });
  });

  describe('hold-down (rate-based flapping)', () => {
    it('keeps a rate alert visible after the counter stops incrementing (within hold-down)', () => {
      const mgr = new AlertManager();
      mgr.evaluate(makeStats({ errors: { total: 0, origin_misconfiguration: 0 } }), 0);
      // Increment fires the alert.
      expect(
        mgr.evaluate(makeStats({ errors: { total: 3, origin_misconfiguration: 0 } }), 1000),
      ).toHaveLength(1);
      // Same total on subsequent ticks — the alert must persist, not strobe off.
      expect(
        mgr.evaluate(makeStats({ errors: { total: 3, origin_misconfiguration: 0 } }), 2000),
      ).toHaveLength(1);
      expect(
        mgr.evaluate(makeStats({ errors: { total: 3, origin_misconfiguration: 0 } }), 5000),
      ).toHaveLength(1);
    });

    it('clears a rate alert once the hold-down window lapses with no new increments', () => {
      const mgr = new AlertManager();
      mgr.evaluate(makeStats({ errors: { total: 0, origin_misconfiguration: 0 } }), 0);
      mgr.evaluate(makeStats({ errors: { total: 3, origin_misconfiguration: 0 } }), 1000);
      const alerts = mgr.evaluate(
        makeStats({ errors: { total: 3, origin_misconfiguration: 0 } }), 1000 + 30_001,
      );
      expect(alerts).toHaveLength(0);
    });

    it('pins triggeredAt to the start of the episode across held-down ticks', () => {
      const mgr = new AlertManager();
      mgr.evaluate(makeStats({ errors: { total: 0, origin_misconfiguration: 0 } }), 0);
      const first = mgr.evaluate(
        makeStats({ errors: { total: 1, origin_misconfiguration: 0 } }), 1000,
      );
      const later = mgr.evaluate(
        makeStats({ errors: { total: 1, origin_misconfiguration: 0 } }), 6000,
      );
      expect(first[0].triggeredAt.getTime()).toBe(1000);
      expect(later[0].triggeredAt.getTime()).toBe(1000);
    });
  });

  describe('reset', () => {
    it('clears all dismissals', () => {
      const mgr = new AlertManager();
      mgr.evaluate(makeStats({ errors: { total: 0, origin_misconfiguration: 0 } }));
      mgr.evaluate(makeStats({ errors: { total: 1, origin_misconfiguration: 0 } }));
      mgr.dismiss('error-rate');
      expect(
        mgr.evaluate(makeStats({ errors: { total: 2, origin_misconfiguration: 0 } })),
      ).toHaveLength(0);

      mgr.reset();
      const alerts = mgr.evaluate(
        makeStats({ errors: { total: 3, origin_misconfiguration: 0 } }),
      );
      expect(alerts).toHaveLength(1);
    });
  });

  describe('custom rules', () => {
    it('accepts custom alert rules', () => {
      const custom: AlertRule = {
        id: 'custom-check',
        label: 'Custom',
        severity: 'error',
        check: (s) => s.connections.active > 100,
        message: (s) => `Too many connections: ${s.connections.active}`,
      };
      const mgr = new AlertManager([custom]);
      const alerts = mgr.evaluate(makeStats({ connections: { active: 150, max: 200 } }));
      expect(alerts).toHaveLength(1);
      expect(alerts[0].message).toBe('Too many connections: 150');
    });
  });

  describe('triggeredAt', () => {
    it('sets triggeredAt to a recent Date', () => {
      const before = Date.now();
      const mgr = new AlertManager();
      const stats = makeStats({ thread_pool: { inflight: 4, size: 4 } });
      const alerts = mgr.evaluate(stats);
      const after = Date.now();

      expect(alerts).toHaveLength(1);
      expect(alerts[0].triggeredAt.getTime()).toBeGreaterThanOrEqual(before);
      expect(alerts[0].triggeredAt.getTime()).toBeLessThanOrEqual(after);
    });
  });
});

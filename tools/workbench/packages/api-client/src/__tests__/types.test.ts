// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, it, expect } from 'vitest';
import type {
  HealthResponse,
  StatsResponse,
  TypeStats,
  ConfigResponse,
  ConfigPatchResponse,
  AlternateInfo,
  CacheAlternatesResponse,
  CacheUrlEntry,
  CacheUrlsResponse,
  MaskInfo,
  CacheSelectAlternateEntry,
  CacheSelectResponse,
  CachePurgeResponse,
  CacheReprocessResponse,
  ApiErrorDetail,
  ApiErrorResponse,
  WsStatsSnapshot,
  WsStatsDelta,
  WsStatsMessage,
  WsStatsAuth,
  WsStatsSetInterval,
  WsStatsClientMessage,
  WsEventItem,
  WsEventBatch,
  WsEventOverflow,
  WsEventMessage,
  LogEntry,
  LogLevel,
  LogSource,
  WsLogSnapshot,
  WsLogOverflow,
  WsLogMessage,
} from '../types.js';

// These tests verify that the type definitions match the backend API
// response shapes. TypeScript compilation itself is the primary check;
// the runtime assertions validate that sample payloads conform.

describe('HealthResponse', () => {
  it('accepts a valid health payload', () => {
    const payload: HealthResponse = {
      status: 'ok',
      checks: {
        cache_open: { pass: true },
        cache_configured: { pass: true },
      },
      uptime_seconds: 3600,
      connections: { active: 5, max: 128 },
      inflight: 2,
      ready: true,
    };
    expect(payload.status).toBe('ok');
    expect(payload.ready).toBe(true);
    expect(payload.connections.active).toBe(5);
  });

  it('accepts a degraded health payload', () => {
    const degraded: HealthResponse = {
      status: 'degraded',
      checks: {
        cache_open: { pass: false, detail: 'cache failed to open' },
        cache_configured: { pass: true },
      },
      uptime_seconds: 3600,
      connections: { active: 5, max: 128 },
      inflight: 2,
      ready: true,
    };
    expect(degraded.status).toBe('degraded');
    expect(degraded.checks.cache_open.pass).toBe(false);
    expect(degraded.checks.cache_open.detail).toBe('cache failed to open');
  });
});

describe('StatsResponse', () => {
  it('accepts a valid stats payload', () => {
    const stats: StatsResponse = {
      notifications: { received: 1234, skipped_dedup: 45, skipped_inflight: 3, rejected_version: 0, rejected_malformed: 0, rejected_sentinel: 0 },
      variants: { written: 567, proactive: 89, gzip: 100, brotli: 50 },
      errors: { total: 12, origin_misconfiguration: 0 },
      cache: { entries: 450, size_bytes: 314572800 },
      by_type: {
        html: { count: 100, time_us: 15000000 },
        css: { count: 200, time_us: 8000000 },
        js: { count: 150, time_us: 5000000 },
        image: { count: 400, time_us: 120000000 },
      },
      by_format: { webp: 250, avif: 120, jpeg: 180, png: 50 },
      svg: {
        candidates_evaluated: 20, candidates_rejected: 5, vectorized: 10,
        fidelity_rejected: 3, timeout_exceeded: 1, size_rejected: 2,
        path_count_rejected: 0, written: 8, bytes_saved: 50000,
        vectorize_time_us: 120000, served: 5,
      },
      content_analysis: { photo: 150, screenshot: 100, illustration: 80, noisy: 70, denoised: 12 },
      ssimulacra2: { checks: 200, declines: 0, tombstone_hits: 0, reencodes: 8, avg_score_x100: 7250 },
      learned_quality: { predictions: 150, fallbacks: 12 },
      html_assembly: { complete: 90, skipped: 10, css_aborted: 5 },
      alternates: { writes: 500, write_failures: 3 },
      selector_invocations: 1000,
      cache_read_retries: 3,
      cache_read_failures: 0,
      cache_read_deferred_retries: 0,
      cache_read_deferred_successes: 0,
      cache_auto_heals: 0,
      cache_auto_heal_exhausted: 0,
      image_incomplete_matrices: 1,
      image_no_savings_skipped: 0,
      image_unconverted_fallthrough: 0,
      dedup: { writes_skipped: 0, content_hash_hits: 0, content_hash_stale: 0 },
      quality_baselining: { capped_jpeg: 5, skip_reencode: 2 },
      thread_pool: { inflight: 2, size: 8 },
      connections: { active: 5, max: 128 },
    };
    expect(stats.notifications.received).toBe(1234);
    expect(stats.by_type.image.time_us).toBe(120000000);
    expect(stats.ssimulacra2.avg_score_x100).toBe(7250);
    expect(stats.learned_quality.predictions).toBe(150);
    expect(stats.svg.served).toBe(5);
  });

  it('accepts stats payload with optional browser section', () => {
    const stats: StatsResponse = {
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
      by_format: { webp: 0, avif: 0, jpeg: 0, png: 0 },
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
      cache_read_failures: 0,
      cache_read_deferred_retries: 0,
      cache_read_deferred_successes: 0,
      cache_auto_heals: 0,
      cache_auto_heal_exhausted: 0,
      image_incomplete_matrices: 0,
      image_no_savings_skipped: 0,
      image_unconverted_fallthrough: 0,
      dedup: { writes_skipped: 0, content_hash_hits: 0, content_hash_stale: 0 },
      quality_baselining: { capped_jpeg: 0, skip_reencode: 0 },
      thread_pool: { inflight: 0, size: 0 },
      connections: { active: 0, max: 0 },
      browser: {
        enabled: true,
        chrome_running: true,
        profiles_generated: 50,
        profiles_used: 45,
        analysis_errors: 2,
        chrome_crashes: 0,
        queue_depth: 3,
        css_inlining_attempted: 10,
        css_inlining_stylesheets_found: 8,
        css_inlining_stylesheets_cached: 6,
        css_inlining_bytes_inlined: 15000,
        reanalyses_scheduled: 3,
        scripts_analyzed: 120,
        scripts_deferrable: 35,
      },
    };
    expect(stats.browser?.enabled).toBe(true);
    expect(stats.browser?.profiles_generated).toBe(50);
    expect(stats.browser?.css_inlining_attempted).toBe(10);
    expect(stats.browser?.reanalyses_scheduled).toBe(3);
    expect(stats.browser?.scripts_analyzed).toBe(120);
    expect(stats.browser?.scripts_deferrable).toBe(35);
  });

  it('accepts stats payload with optional policy section', () => {
    const stats: StatsResponse = {
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
      by_format: { webp: 0, avif: 0, jpeg: 0, png: 0 },
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
      cache_read_failures: 0,
      cache_read_deferred_retries: 0,
      cache_read_deferred_successes: 0,
      cache_auto_heals: 0,
      cache_auto_heal_exhausted: 0,
      image_incomplete_matrices: 0,
      image_no_savings_skipped: 0,
      image_unconverted_fallthrough: 0,
      dedup: { writes_skipped: 0, content_hash_hits: 0, content_hash_stale: 0 },
      quality_baselining: { capped_jpeg: 0, skip_reencode: 0 },
      thread_pool: { inflight: 0, size: 0 },
      connections: { active: 0, max: 0 },
      policy: {
        computed: 42,
        async_css_enabled: 30,
        script_deferral_enabled: 25,
      },
    };
    expect(stats.policy?.computed).toBe(42);
    expect(stats.policy?.async_css_enabled).toBe(30);
    expect(stats.policy?.script_deferral_enabled).toBe(25);
  });

  it('TypeStats has count and time_us', () => {
    const ts: TypeStats = { count: 42, time_us: 999 };
    expect(ts.count).toBe(42);
  });
});

describe('ConfigResponse', () => {
  it('accepts a valid config payload', () => {
    const config: ConfigResponse = {
      jpeg_quality: 85,
      webp_quality: 75,
      avif_quality: 25,
      savedata_jpeg_quality: 60,
      savedata_webp_quality: 50,
      savedata_avif_quality: 35,
      mobile_width: 480,
      tablet_width: 768,
      desktop_width: 0,
      proactive_image_variants: true,
      proactive_viewport_variants: true,
      proactive_savedata_variants: true,
      proactive_density_variants: true,
      content_analysis: true,
      quality_verify: true,
      target_ssimulacra2: 70.0,
      ssimulacra2_tolerance: 5.0,
      html_mode: 'full',
      disable_html: false,
      disable_css: false,
      disable_js: false,
      disable_script_deferral: false,
      disable_image: false,
      disable_lazy_load: false,
      disable_image_dimensions: false,
      disable_lcp_preload: false,
      disable_preconnect_injection: false,
      enable_speculation_rules: false,
      disable_async_css: false,
      disable_css_import_flattening: false,
      svg_preset: 'default',
      svg_fidelity_threshold: 70,
      svg_timeout_ms: 5000,
      svg_candidacy_threshold: 50,
      svg_color_precision: 0,
      svg_exclude_lcp: true,
      svg_filter_speckle: 4,
      svg_max_paths: 500,
      svg_max_pixels: 65536,
      svg_max_svg_bytes: 262144,
      svg_mode: 'detect',
      no_quality_cap: false,
      quality_cap_margin: 10,
      savedata_score_reduction: 15,
      gzip_level: 6,
      brotli_level: 4,
      max_url_length: 8192,
      max_html_size: 5242880,
      max_css_size: 2097152,
      max_js_size: 5242880,
      max_image_size: 33554432,
      enable_browser_analysis: false,
      chrome_binary: '/usr/bin/chrome-headless-shell',
      chrome_recycle_interval: 100,
      chrome_page_timeout: 60000,
      chrome_max_memory: 512,
      enable_browser_critical_css: true,
      enable_browser_lazy_loading: true,
      enable_browser_lcp_preload: true,
      enable_browser_image_sizing: true,
      browser_queue_size: 1000,
      browser_profile_ttl: 86400,
      learned_quality: true,
      learned_quality_jpeg: true,
      learned_quality_webp: true,
      learned_quality_avif: true,
      enable_warmup: false,
      cache_mode: 'safe',
    };
    expect(config.jpeg_quality).toBe(85);
    expect(config.proactive_image_variants).toBe(true);
    expect(config.svg_preset).toBe('default');
    expect(config.enable_browser_analysis).toBe(false);
    expect(config.learned_quality).toBe(true);
  });
});

describe('ConfigPatchResponse', () => {
  it('accepts applied, rejected, warnings, and full config', () => {
    const resp: ConfigPatchResponse = {
      applied: { jpeg_quality: 90 },
      rejected: {},
      warnings: [],
      config: {
        jpeg_quality: 90,
        webp_quality: 75,
        avif_quality: 25,
        savedata_jpeg_quality: 60,
        savedata_webp_quality: 50,
        savedata_avif_quality: 35,
        mobile_width: 480,
        tablet_width: 768,
        desktop_width: 0,
        proactive_image_variants: true,
        proactive_viewport_variants: true,
        proactive_savedata_variants: true,
        proactive_density_variants: true,
        content_analysis: true,
        quality_verify: true,
        target_ssimulacra2: 70.0,
        ssimulacra2_tolerance: 5.0,
        html_mode: 'full',
        disable_html: false,
        disable_css: false,
        disable_js: false,
        disable_script_deferral: false,
        disable_image: false,
        disable_lazy_load: false,
        disable_image_dimensions: false,
        disable_lcp_preload: false,
        disable_preconnect_injection: false,
        enable_speculation_rules: false,
        disable_async_css: false,
        disable_css_import_flattening: false,
        svg_preset: 'default',
        svg_fidelity_threshold: 70,
        svg_timeout_ms: 5000,
        svg_candidacy_threshold: 50,
        svg_color_precision: 0,
        svg_exclude_lcp: true,
        svg_filter_speckle: 4,
        svg_max_paths: 500,
        svg_max_pixels: 65536,
        svg_max_svg_bytes: 262144,
        svg_mode: 'detect',
        no_quality_cap: false,
        quality_cap_margin: 10,
        savedata_score_reduction: 15,
        gzip_level: 6,
        brotli_level: 4,
        max_url_length: 8192,
        max_html_size: 5242880,
        max_css_size: 2097152,
        max_js_size: 5242880,
        max_image_size: 33554432,
        enable_browser_analysis: false,
        chrome_binary: '/usr/bin/chrome-headless-shell',
        chrome_recycle_interval: 100,
        chrome_page_timeout: 60000,
        chrome_max_memory: 512,
        enable_browser_critical_css: true,
        enable_browser_lazy_loading: true,
        enable_browser_lcp_preload: true,
        enable_browser_image_sizing: true,
        browser_queue_size: 1000,
        browser_profile_ttl: 86400,
        learned_quality: true,
        learned_quality_jpeg: true,
        learned_quality_webp: true,
        learned_quality_avif: true,
        enable_warmup: false,
        cache_mode: 'safe',
      },
    };
    expect(resp.applied.jpeg_quality).toBe(90);
    expect(resp.config.jpeg_quality).toBe(90);
  });
});

describe('Cache types', () => {
  it('CacheAlternatesResponse matches backend shape', () => {
    const alt: AlternateInfo = {
      alternate_id: 8,
      size: 45678,
      hit_count: 123,
      is_sentinel: false,
      format: 'original',
      viewport: 'mobile',
      density: '1x',
      save_data: false,
      encoding: 'identity',
      content_type: 'html',
      origin_content_type: 'text/html',
      flags: 0,
      needs_revalidation: false,
      last_access: 1707123456789,
    };
    const resp: CacheAlternatesResponse = {
      url: '/index.html',
      hostname: 'example.com',
      alternates: [alt],
      count: 1,
      chain_length: 1,
    };
    expect(resp.alternates).toHaveLength(1);
    expect(resp.alternates[0].alternate_id).toBe(8);
    expect(resp.count).toBe(1);
    expect(resp.chain_length).toBe(1);
  });

  it('CacheUrlsResponse matches backend shape', () => {
    const entry: CacheUrlEntry = {
      url: '/style.css',
      hostname: 'example.com',
      alternate_count: 12,
    };
    const resp: CacheUrlsResponse = {
      total: 450,
      offset: 0,
      limit: 100,
      next_offset: 100,
      has_more: true,
      urls: [entry],
    };
    expect(resp.total).toBe(450);
    expect(resp.next_offset).toBe(100);
    expect(resp.has_more).toBe(true);
  });

  it('CacheSelectResponse matches backend shape', () => {
    const clientMask: MaskInfo = {
      raw: 10,
      format: 'avif',
      viewport: 'desktop',
      density: '1x',
      save_data: false,
      encoding: 'identity',
    };
    const altEntry: CacheSelectAlternateEntry = {
      alternate_id: 10,
      is_sentinel: false,
      size: 38000,
      score: 1200,
      mask: { raw: 10, format: 'avif', viewport: 'desktop', density: '1x', save_data: false, encoding: 'identity' },
      content_type: 'image',
    };
    const resp: CacheSelectResponse = {
      url: '/hero.jpg',
      hostname: 'example.com',
      client_mask: clientMask,
      alternates: [altEntry],
      best_index: 0,
      best_score: 1200,
    };
    expect(resp.best_score).toBe(1200);
    expect(resp.best_index).toBe(0);
    expect(resp.alternates[0].score).toBe(1200);
  });

  it('CachePurgeResponse', () => {
    const resp: CachePurgeResponse = { url: '/test', hostname: 'example.com', deleted: 42 };
    expect(resp.deleted).toBe(42);
    expect(resp.url).toBe('/test');
  });

  it('CacheReprocessResponse', () => {
    const resp: CacheReprocessResponse = { url: '/test', hostname: 'example.com', reprocess_enqueued: true };
    expect(resp.reprocess_enqueued).toBe(true);
  });
});

describe('Error types', () => {
  it('ApiErrorResponse matches error envelope', () => {
    const detail: ApiErrorDetail = {
      code: 'BAD_REQUEST',
      message: 'Missing required parameter',
      details: { param: 'url' },
    };
    const resp: ApiErrorResponse = { error: detail };
    expect(resp.error.code).toBe('BAD_REQUEST');
  });

  it('ApiErrorDetail.details is optional', () => {
    const detail: ApiErrorDetail = {
      code: 'NOT_FOUND',
      message: 'Resource not found',
    };
    expect(detail.details).toBeUndefined();
  });
});

describe('WebSocket stats types', () => {
  it('WsStatsSnapshot', () => {
    const snap: WsStatsSnapshot = {
      type: 'snapshot',
      sequence: 0,
      data: {
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
        by_format: { webp: 0, avif: 0, jpeg: 0, png: 0 },
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
        cache_read_failures: 0,
        cache_read_deferred_retries: 0,
        cache_read_deferred_successes: 0,
        cache_auto_heals: 0,
        cache_auto_heal_exhausted: 0,
        image_incomplete_matrices: 0,
        image_no_savings_skipped: 0,
        image_unconverted_fallthrough: 0,
        dedup: { writes_skipped: 0, content_hash_hits: 0, content_hash_stale: 0 },
        quality_baselining: { capped_jpeg: 0, skip_reencode: 0 },
        thread_pool: { inflight: 0, size: 0 },
        connections: { active: 0, max: 0 },
      },
    };
    expect(snap.type).toBe('snapshot');
    expect(snap.sequence).toBe(0);
  });

  it('WsStatsDelta', () => {
    const delta: WsStatsDelta = {
      type: 'delta',
      sequence: 1,
      data: { errors: { total: 1, origin_misconfiguration: 0 } },
    };
    expect(delta.type).toBe('delta');
    expect(delta.data.errors?.total).toBe(1);
  });

  it('WsStatsMessage union discriminates on type', () => {
    const msg: WsStatsMessage = {
      type: 'delta',
      sequence: 2,
      data: { errors: { total: 3, origin_misconfiguration: 0 } },
    };
    if (msg.type === 'delta') {
      expect(msg.data.errors?.total).toBe(3);
    }
  });

  it('WsStatsClientMessage union', () => {
    const auth: WsStatsAuth = { auth: 'mytoken' };
    const interval: WsStatsSetInterval = { set_interval_ms: 5000 };
    const msgs: WsStatsClientMessage[] = [auth, interval];
    expect(msgs).toHaveLength(2);
  });
});

describe('WebSocket event types', () => {
  it('WsEventItem', () => {
    const item: WsEventItem = {
      type: 'variant_written',
      data: { url: '/img.jpg', format: 'avif' },
    };
    expect(item.type).toBe('variant_written');
  });

  it('WsEventBatch', () => {
    const batch: WsEventBatch = {
      type: 'events',
      sequence: 1234,
      data: [{ type: 'variant_written', data: { url: '/img.jpg' } }],
    };
    expect(batch.data[0].type).toBe('variant_written');
  });

  it('WsEventOverflow', () => {
    const overflow: WsEventOverflow = { type: 'overflow', dropped_count: 42 };
    expect(overflow.dropped_count).toBe(42);
  });

  it('WsEventMessage union discriminates on type', () => {
    const msg: WsEventMessage = { type: 'overflow', dropped_count: 5 };
    if (msg.type === 'overflow') {
      expect(msg.dropped_count).toBe(5);
    }
  });
});

describe('WebSocket log types', () => {
  it('LogEntry with all fields', () => {
    const entry: LogEntry = {
      type: 'log',
      timestamp: 1707546896123,
      source: 'worker',
      level: 'info',
      module: 'image',
      message: 'Encoded WebP variant',
      details: { url: '/hero.jpg', size: 45000 },
    };
    expect(entry.type).toBe('log');
    expect(entry.source).toBe('worker');
    expect(entry.level).toBe('info');
    expect(entry.details?.url).toBe('/hero.jpg');
  });

  it('LogEntry without optional details', () => {
    const entry: LogEntry = {
      type: 'log',
      timestamp: 1707546896123,
      source: 'cache',
      level: 'debug',
      module: 'cache',
      message: 'Cache miss',
    };
    expect(entry.details).toBeUndefined();
  });

  it('LogLevel covers all severity levels', () => {
    const levels: LogLevel[] = ['debug', 'info', 'warning', 'error'];
    expect(levels).toHaveLength(4);
  });

  it('LogSource covers all sources', () => {
    const sources: LogSource[] = ['worker', 'cache', 'chrome'];
    expect(sources).toHaveLength(3);
  });

  it('WsLogSnapshot', () => {
    const snap: WsLogSnapshot = {
      type: 'snapshot',
      entries: [
        {
          type: 'log',
          timestamp: 1707546896123,
          source: 'worker',
          level: 'info',
          module: 'image',
          message: 'Test',
        },
      ],
      total: 1,
    };
    expect(snap.type).toBe('snapshot');
    expect(snap.entries).toHaveLength(1);
    expect(snap.total).toBe(1);
  });

  it('WsLogOverflow', () => {
    const overflow: WsLogOverflow = {
      type: 'overflow',
      dropped_count: 100,
    };
    expect(overflow.dropped_count).toBe(100);
  });

  it('WsLogMessage union discriminates on type', () => {
    const msg: WsLogMessage = {
      type: 'log',
      timestamp: 1707546896123,
      source: 'chrome',
      level: 'error',
      module: 'chrome',
      message: 'CDP connection lost',
    };
    if (msg.type === 'log') {
      expect(msg.source).toBe('chrome');
    }
  });
});

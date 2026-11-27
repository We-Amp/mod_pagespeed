<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  import { getContext, onDestroy } from 'svelte';
  import type { Readable } from 'svelte/store';
  import { formatBytes, formatUptime } from '@pagespeed/api-client';
  import type {
    ApiTransport,
    ConnectionManager,
    ConnectionState,
    StatsResponse,
    HealthResponse,
    WsStatsMessage,
  } from '@pagespeed/api-client';
  import TimeSeriesChart from '#lib/components/TimeSeriesChart.svelte';
  import HelpIcon from '#lib/components/HelpIcon.svelte';
  import AlertBanner from '#lib/components/AlertBanner.svelte';
  import { AlertManager } from '#lib/alerts.ts';
  import type { ActiveAlert } from '#lib/alerts.ts';
  import { RingBuffer } from '#lib/ring-buffer.ts';
  import { exportJson, exportCsv, timestampedFilename } from '#lib/export.ts';
  import { summarizeServeSavings } from '#lib/savings.ts';

  // -- Context ---------------------------------------------------------------

  const transport = getContext<ApiTransport>('api');
  const connectionManager = getContext<ConnectionManager>('connection');

  const connState: Readable<ConnectionState> = connectionManager.state;
  const healthData: Readable<HealthResponse | null> = connectionManager.healthData;

  // -- Reactive state --------------------------------------------------------

  let stats: StatsResponse | null = $state(null);
  let connected = $state(false);
  let uptimeSeconds = $state(0);

  // -- Alert system ----------------------------------------------------------

  const alertManager = new AlertManager();
  let activeAlerts: ActiveAlert[] = $state([]);

  function handleDismissAlert(id: string) {
    alertManager.dismiss(id);
    // Re-evaluate immediately to update the UI.
    if (stats) {
      activeAlerts = alertManager.evaluate(stats);
    }
  }

  // -- Time-series chart state -----------------------------------------------
  // Series indices:
  //   Throughput: 0 = notifications/s, 1 = variants/s
  //   Cache:      2 = cache entries,   3 = cache size MB
  //   Health:     4 = errors/s,        5 = thread pool inflight
  const SERIES_COUNT = 6;
  const ringBuffer = new RingBuffer(SERIES_COUNT);

  // Previous absolute counter values for rate-of-change calculation.
  let prevNotifications: number | null = null;
  let prevVariantsWritten: number | null = null;
  let prevErrors: number | null = null;

  // Reactive snapshots of chart data (re-assigned each push to trigger updates).
  let chartTimestamps: number[] = $state([]);
  let chartSeries: number[][] = $state([[], [], [], [], [], []]);

  // Subscribe to connection state store.
  const unsubConn = connState.subscribe((s) => {
    connected = s === 'connected';
  });

  // Subscribe to health data for uptime.
  const unsubHealth = healthData.subscribe((h) => {
    if (h) {
      uptimeSeconds = h.uptime_seconds;
    }
  });

  // -- WebSocket subscription ------------------------------------------------

  const unsubWs = transport.subscribe('/v1/ws/stats', (raw: unknown) => {
    const msg = raw as WsStatsMessage;
    if (msg.type === 'snapshot') {
      // Only use WS snapshot if it has the expected nested structure.
      if (msg.data && 'notifications' in (msg.data as Record<string, unknown>)) {
        stats = msg.data;
      }
    } else if (msg.type === 'delta') {
      if (stats) {
        stats = deepMerge(stats, msg.data);
      }
    }
  });

  // -- Bandwidth savings (serve-time, from shared mmap counters) -------------

  // Net savings can dip slightly negative when an optimization trades payload
  // bytes for render latency (e.g. critical-CSS inlining). summarizeServeSavings
  // clamps the headline to >= 0 (matching the Savings page) and exposes the
  // honest signal via hasRegression.
  let serveSavings = $derived(summarizeServeSavings(stats?.serve_savings));
  let savedBytesIn = $derived(serveSavings.originalBytes);
  let savedBytesOut = $derived(serveSavings.optimizedBytes);
  let savedBytesDelta = $derived(serveSavings.savedBytes);
  let savedPercent = $derived(serveSavings.savedPercent);
  let totalServeHits = $derived(serveSavings.hits);
  let savingsHasRegression = $derived(serveSavings.hasRegression);

  // -- Health summary --------------------------------------------------------

  let healthSummary = $derived.by(() => {
    const count = activeAlerts.length;
    if (count === 0) return { text: 'All systems healthy', level: 'healthy' as const };
    return {
      text: `${count} issue${count === 1 ? '' : 's'} detected`,
      level: count >= 2 ? ('error' as const) : ('warning' as const),
    };
  });

  // -- Cleanup ---------------------------------------------------------------

  onDestroy(() => {
    unsubWs();
    unsubConn();
    unsubHealth();
  });

  // -- Chart data pump -------------------------------------------------------
  // Push a data point each time stats changes.  For monotonic counters
  // (notifications, variants, errors) we compute rate-of-change (delta/s).

  $effect(() => {
    if (!stats) return;

    const now = Math.floor(Date.now() / 1000);

    // Compute rates (delta per second).  On the first sample the rate is 0.
    const notif = stats.notifications.received;
    const variants = stats.variants.written;
    const errors = stats.errors.total;

    const notifRate =
      prevNotifications !== null ? Math.max(0, notif - prevNotifications) : 0;
    const variantsRate =
      prevVariantsWritten !== null ? Math.max(0, variants - prevVariantsWritten) : 0;
    const errorsRate = prevErrors !== null ? Math.max(0, errors - prevErrors) : 0;

    prevNotifications = notif;
    prevVariantsWritten = variants;
    prevErrors = errors;
    errorsIncreasing = errorsRate > 0;

    // Gauge values (not rate-of-change).
    const entries = stats.cache.entries;
    const sizeMb = stats.cache.size_bytes / (1024 * 1024);
    const inflight = stats.thread_pool.inflight;

    ringBuffer.push(now, [
      notifRate,
      variantsRate,
      entries,
      sizeMb,
      errorsRate,
      inflight,
    ]);

    // Re-assign reactive snapshots so Svelte sees the update.
    chartTimestamps = ringBuffer.timestamps;
    chartSeries = ringBuffer.series;
  });

  // -- Alert evaluation (runs each time stats changes) ----------------------

  $effect(() => {
    if (stats) {
      activeAlerts = alertManager.evaluate(stats);
    }
  });

  // -- Chart theme colors (read from CSS custom properties) -----------------

  function getCssColor(name: string, fallback: string): string {
    if (typeof getComputedStyle === 'undefined') return fallback;
    const v = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
    return v || fallback;
  }

  // Lazily resolved on first render; safe since these are only used in the
  // template which runs client-side after mount.
  let chartColors = $derived({
    accent: getCssColor('--ps-accent', '#0078d4'),
    success: getCssColor('--ps-success', '#388a34'),
    warning: getCssColor('--ps-warning', '#bf8803'),
    error: getCssColor('--ps-error', '#f14c4c'),
    info: getCssColor('--ps-info', '#1a85ff'),
    muted: getCssColor('--ps-fg-muted', '#6e6e6e'),
  });

  // -- Derived values --------------------------------------------------------

  let formattedUptime = $derived(formatUptime(uptimeSeconds));

  let totalNotifications = $derived(stats?.notifications.received ?? 0);
  let variantsWritten = $derived(stats?.variants.written ?? 0);
  let proactiveVariants = $derived(stats?.variants.proactive ?? 0);
  let totalErrors = $derived(stats?.errors.total ?? 0);
  // Set from the stats-update pump above (errorsRate > 0), so it means "errors
  // rose on the latest sample" and stays stable between samples. The old
  // $derived compared totalErrors against a value a reactive $effect equalized
  // on the same tick, so the warning was only ever true for a single frame.
  let errorsIncreasing = $state(false);

  let cacheEntries = $derived(stats?.cache.entries ?? 0);
  let cacheSizeBytes = $derived(stats?.cache.size_bytes ?? 0);

  let activeConnections = $derived(stats?.connections.active ?? 0);
  let maxConnections = $derived(stats?.connections.max ?? 0);

  let threadPoolInflight = $derived(stats?.thread_pool.inflight ?? 0);
  let threadPoolSize = $derived(stats?.thread_pool.size ?? 0);

  let htmlAssemblyComplete = $derived(stats?.html_assembly.complete ?? 0);
  let htmlAssemblySkipped = $derived(stats?.html_assembly.skipped ?? 0);
  let htmlAssemblyCssAborted = $derived(stats?.html_assembly.css_aborted ?? 0);

  let alternateWrites = $derived(stats?.alternates.writes ?? 0);
  let alternateWriteFailures = $derived(stats?.alternates.write_failures ?? 0);

  let svgCandidatesEvaluated = $derived(stats?.svg?.candidates_evaluated ?? 0);
  let svgCandidatesRejected = $derived(stats?.svg?.candidates_rejected ?? 0);
  let svgVectorized = $derived(stats?.svg?.vectorized ?? 0);
  let svgFidelityRejected = $derived(stats?.svg?.fidelity_rejected ?? 0);
  let svgTimeoutExceeded = $derived(stats?.svg?.timeout_exceeded ?? 0);
  let svgSizeRejected = $derived(stats?.svg?.size_rejected ?? 0);
  let svgPathCountRejected = $derived(stats?.svg?.path_count_rejected ?? 0);
  let svgWritten = $derived(stats?.svg?.written ?? 0);
  let svgBytesSaved = $derived(stats?.svg?.bytes_saved ?? 0);
  let svgServed = $derived(stats?.svg?.served ?? 0);

  let browserEnabled = $derived(stats?.browser?.enabled ?? false);
  let browserChromeRunning = $derived(stats?.browser?.chrome_running ?? false);
  let browserProfilesGenerated = $derived(stats?.browser?.profiles_generated ?? 0);
  let browserProfilesUsed = $derived(stats?.browser?.profiles_used ?? 0);
  let browserAnalysisErrors = $derived(stats?.browser?.analysis_errors ?? 0);
  let browserChromeCrashes = $derived(stats?.browser?.chrome_crashes ?? 0);
  let browserQueueDepth = $derived(stats?.browser?.queue_depth ?? 0);

  let selectorInvocations = $derived(stats?.selector_invocations ?? 0);

  let learnedQualityPredictions = $derived(stats?.learned_quality?.predictions ?? 0);
  let learnedQualityFallbacks = $derived(stats?.learned_quality?.fallbacks ?? 0);

  let cacheReadRetries = $derived(stats?.cache_read_retries ?? 0);
  let cacheReadFailures = $derived(stats?.cache_read_failures ?? 0);
  let cacheAutoHeals = $derived(stats?.cache_auto_heals ?? 0);
  let cacheAutoHealExhausted = $derived(stats?.cache_auto_heal_exhausted ?? 0);
  let cacheDeferredRetries = $derived(stats?.cache_read_deferred_retries ?? 0);
  let cacheDeferredSuccesses = $derived(stats?.cache_read_deferred_successes ?? 0);
  let imageIncompleteMatrices = $derived(stats?.image_incomplete_matrices ?? 0);

  let dedupWritesSkipped = $derived(stats?.dedup?.writes_skipped ?? 0);
  let dedupContentHashHits = $derived(stats?.dedup?.content_hash_hits ?? 0);
  let dedupContentHashStale = $derived(stats?.dedup?.content_hash_stale ?? 0);

  let policyComputed = $derived(stats?.policy?.computed ?? 0);
  let policyAsyncCssEnabled = $derived(stats?.policy?.async_css_enabled ?? 0);
  let policyScriptDeferralEnabled = $derived(stats?.policy?.script_deferral_enabled ?? 0);

  let ssimChecks = $derived(stats?.ssimulacra2.checks ?? 0);
  let ssimReencodes = $derived(stats?.ssimulacra2.reencodes ?? 0);
  let ssimAvgScore = $derived(
    stats?.ssimulacra2.avg_score_x100
      ? (stats.ssimulacra2.avg_score_x100 / 100).toFixed(2)
      : '--',
  );

  // By-type breakdown data for the table.
  let byTypeEntries = $derived(
    stats?.by_type
      ? ([
          { label: 'HTML', ...stats.by_type.html },
          { label: 'CSS', ...stats.by_type.css },
          { label: 'JavaScript', ...stats.by_type.js },
          { label: 'Image', ...stats.by_type.image },
        ] as Array<{ label: string; count: number; time_us: number }>)
      : [],
  );

  // By-format breakdown for the format table.
  let byFormatEntries = $derived(
    stats?.by_format
      ? ([
          { label: 'WebP', count: stats.by_format.webp },
          { label: 'AVIF', count: stats.by_format.avif },
          { label: 'JPEG', count: stats.by_format.jpeg },
          { label: 'PNG', count: stats.by_format.png },
          { label: 'SVG', count: stats.by_format.svg ?? 0 },
        ] as Array<{ label: string; count: number }>)
      : [],
  );

  // Content analysis breakdown.
  let contentAnalysisEntries = $derived(
    stats?.content_analysis
      ? ([
          { label: 'Photo', count: stats.content_analysis.photo },
          { label: 'Screenshot', count: stats.content_analysis.screenshot },
          { label: 'Illustration', count: stats.content_analysis.illustration },
          { label: 'Noisy', count: stats.content_analysis.noisy },
        ] as Array<{ label: string; count: number }>)
      : [],
  );

  // -- Helpers ---------------------------------------------------------------

  // formatBytes and formatUptime imported from @pagespeed/api-client

  function formatDuration(microseconds: number): string {
    if (microseconds === 0) return '0 \u00B5s';
    if (microseconds < 1000) return `${microseconds} \u00B5s`;
    if (microseconds < 1_000_000) return `${(microseconds / 1000).toFixed(1)} ms`;
    return `${(microseconds / 1_000_000).toFixed(2)} s`;
  }

  function formatNumber(n: number): string {
    return n.toLocaleString();
  }

  /**
   * Deep-merge a Partial<StatsResponse> delta into the current stats.
   * For numeric fields, we replace (the backend sends absolute values in deltas).
   * For nested objects, we merge shallowly.
   */
  function deepMerge(base: StatsResponse, delta: Partial<StatsResponse>): StatsResponse {
    const result = { ...base };
    for (const key of Object.keys(delta) as Array<keyof StatsResponse>) {
      const val = delta[key];
      if (val === undefined) continue;
      if (typeof val === 'object' && val !== null && !Array.isArray(val)) {
        // Merge nested object shallowly.
        (result as Record<string, unknown>)[key] = {
          ...(base[key] as Record<string, unknown>),
          ...(val as Record<string, unknown>),
        };
      } else {
        (result as Record<string, unknown>)[key] = val;
      }
    }
    return result;
  }

  // -- Export handlers -------------------------------------------------------

  function handleExportJson() {
    if (!stats) return;
    exportJson(stats, timestampedFilename('pagespeed-stats', 'json'));
  }

  function handleExportCsv() {
    if (!stats) return;
    const headers = ['Category', 'Metric', 'Value'];
    const rows: (string | number)[][] = [
      ['Notifications', 'Received', stats.notifications.received],
      ['Notifications', 'Deduped', stats.notifications.skipped_dedup],
      ['Notifications', 'In-flight skipped', stats.notifications.skipped_inflight],
      ['Variants', 'Written', stats.variants.written],
      ['Variants', 'Proactive', stats.variants.proactive],
      ['Variants', 'Gzip', stats.variants.gzip],
      ['Variants', 'Brotli', stats.variants.brotli],
      ['Errors', 'Total', stats.errors.total],
      ['Errors', 'Origin Misconfiguration', stats.errors.origin_misconfiguration],
      ['Cache', 'Entries', stats.cache.entries],
      ['Cache', 'Size (bytes)', stats.cache.size_bytes],
      ['By Type', 'HTML Count', stats.by_type.html.count],
      ['By Type', 'HTML Time (us)', stats.by_type.html.time_us],
      ['By Type', 'CSS Count', stats.by_type.css.count],
      ['By Type', 'CSS Time (us)', stats.by_type.css.time_us],
      ['By Type', 'JS Count', stats.by_type.js.count],
      ['By Type', 'JS Time (us)', stats.by_type.js.time_us],
      ['By Type', 'Image Count', stats.by_type.image.count],
      ['By Type', 'Image Time (us)', stats.by_type.image.time_us],
      ['By Format', 'WebP', stats.by_format.webp],
      ['By Format', 'AVIF', stats.by_format.avif],
      ['By Format', 'JPEG', stats.by_format.jpeg],
      ['By Format', 'PNG', stats.by_format.png],
      ['By Format', 'SVG', stats.by_format.svg ?? 0],
      ['SVG', 'Candidates Evaluated', stats.svg?.candidates_evaluated ?? 0],
      ['SVG', 'Candidates Rejected', stats.svg?.candidates_rejected ?? 0],
      ['SVG', 'Vectorized', stats.svg?.vectorized ?? 0],
      ['SVG', 'Fidelity Rejected', stats.svg?.fidelity_rejected ?? 0],
      ['SVG', 'Timeout Exceeded', stats.svg?.timeout_exceeded ?? 0],
      ['SVG', 'Size Rejected', stats.svg?.size_rejected ?? 0],
      ['SVG', 'Path Count Rejected', stats.svg?.path_count_rejected ?? 0],
      ['SVG', 'Written', stats.svg?.written ?? 0],
      ['SVG', 'Bytes Saved', stats.svg?.bytes_saved ?? 0],
      ['SVG', 'Served', stats.svg?.served ?? 0],
      ['Content Analysis', 'Photo', stats.content_analysis.photo],
      ['Content Analysis', 'Screenshot', stats.content_analysis.screenshot],
      ['Content Analysis', 'Illustration', stats.content_analysis.illustration],
      ['Content Analysis', 'Noisy', stats.content_analysis.noisy],
      ['SSIMULACRA2', 'Checks', stats.ssimulacra2.checks],
      ['SSIMULACRA2', 'Re-encodes', stats.ssimulacra2.reencodes],
      ['SSIMULACRA2', 'Avg Score (x100)', stats.ssimulacra2.avg_score_x100],
      ['HTML Assembly', 'Complete', stats.html_assembly.complete],
      ['HTML Assembly', 'Skipped', stats.html_assembly.skipped],
      ['HTML Assembly', 'CSS Aborted', stats.html_assembly.css_aborted],
      ['Variants', 'Writes', stats.alternates.writes],
      ['Variants', 'Write Failures', stats.alternates.write_failures],
      ['Selector', 'Invocations', stats.selector_invocations],
      ['Thread Pool', 'Inflight', stats.thread_pool.inflight],
      ['Thread Pool', 'Size', stats.thread_pool.size],
      ['Connections', 'Active', stats.connections.active],
      ['Connections', 'Max', stats.connections.max],
      ['Browser', 'Profiles Generated', stats.browser?.profiles_generated ?? 0],
      ['Browser', 'Profiles Used', stats.browser?.profiles_used ?? 0],
      ['Browser', 'Analysis Errors', stats.browser?.analysis_errors ?? 0],
      ['Browser', 'Chrome Crashes', stats.browser?.chrome_crashes ?? 0],
      ['Browser', 'Queue Depth', stats.browser?.queue_depth ?? 0],
      ['Learned Quality', 'Predictions', stats.learned_quality?.predictions ?? 0],
      ['Learned Quality', 'Fallbacks', stats.learned_quality?.fallbacks ?? 0],
      ['Cache Reliability', 'Read Retries', stats.cache_read_retries],
      ['Cache Reliability', 'Read Failures', stats.cache_read_failures],
      ['Cache Reliability', 'Auto Heals', stats.cache_auto_heals],
      ['Cache Reliability', 'Auto Heal Exhausted', stats.cache_auto_heal_exhausted],
      ['Cache Reliability', 'Deferred Retries', stats.cache_read_deferred_retries],
      ['Cache Reliability', 'Deferred Successes', stats.cache_read_deferred_successes],
      ['Cache Reliability', 'Incomplete Matrices', stats.image_incomplete_matrices],
      ['Dedup', 'Writes Skipped', stats.dedup?.writes_skipped ?? 0],
      ['Dedup', 'Content Hash Hits', stats.dedup?.content_hash_hits ?? 0],
      ['Dedup', 'Content Hash Stale', stats.dedup?.content_hash_stale ?? 0],
      ['Policy', 'Computed', stats.policy?.computed ?? 0],
      ['Policy', 'Async CSS Enabled', stats.policy?.async_css_enabled ?? 0],
      ['Policy', 'Script Deferral Enabled', stats.policy?.script_deferral_enabled ?? 0],
    ];
    exportCsv(headers, rows, timestampedFilename('pagespeed-stats', 'csv'));
  }
</script>

{#if !connected}
  <div class="dashboard-disconnected">
    <div class="ps-card disconnected-card">
      <div class="ps-spinner" aria-label="Connecting to dashboard"></div>
      <p class="disconnected-text">Waiting for worker connection...</p>
      <span class="ps-badge ps-badge-warning">Disconnected</span>
    </div>
  </div>
{:else if !stats}
  <div class="dashboard-disconnected">
    <div class="ps-card disconnected-card">
      <div class="ps-spinner" aria-label="Loading dashboard statistics"></div>
      <p class="disconnected-text">Loading statistics...</p>
    </div>
  </div>
{:else if stats.notifications.received === 0}
  <!-- Onboarding: fresh installation with no traffic yet -->
  <div class="dashboard-onboarding">
    <div class="ps-card onboarding-card">
      <span class="onboarding-check" aria-hidden="true">&#x2713;</span>
      <h2 class="onboarding-title">ModPageSpeed is connected and ready.</h2>
      <p class="onboarding-text">
        Visit your site through the ModPageSpeed proxy to start optimizing.
      </p>
      <div class="onboarding-actions">
        <a class="ps-btn ps-btn-primary" href="/console/urls">Test a URL</a>
        <a class="ps-btn ps-btn-secondary" href="/console/diff">Compare Before/After</a>
        <a class="ps-btn ps-btn-secondary" href="/console/config">Review Settings</a>
      </div>
    </div>
  </div>
{:else}
  <AlertBanner alerts={activeAlerts} onDismiss={handleDismissAlert} />
  <div class="dashboard">
    <!-- Header -->
    <div class="dashboard-header">
      <h1 class="dashboard-title">Dashboard</h1>
      <div class="dashboard-header-right">
        <div class="dashboard-header-meta">
          <!-- Health Summary -->
          <span class="health-summary health-{healthSummary.level}">
            <span
              class="ps-status-dot ps-status-dot-{healthSummary.level === 'healthy'
                ? 'green'
                : healthSummary.level === 'warning'
                  ? 'yellow'
                  : 'red'}"
              aria-hidden="true"
            ></span>
            {healthSummary.text}
          </span>
          <!-- Uptime -->
          <span class="uptime-text">Uptime: {formattedUptime}</span>
        </div>
        <div class="export-buttons">
          <button
            class="ps-btn ps-btn-secondary"
            onclick={handleExportJson}
            disabled={!stats}
            title="Export stats as JSON"
          >
            Export JSON
          </button>
          <button
            class="ps-btn ps-btn-secondary"
            onclick={handleExportCsv}
            disabled={!stats}
            title="Export stats as CSV"
          >
            Export CSV
          </button>
        </div>
      </div>
    </div>

    <!-- Hero: Total Bandwidth Saved -->
    {#if savedBytesIn > 0}
      <div class="ps-card hero-card">
        <div class="hero-label">Total Bandwidth Saved</div>
        {#if savingsHasRegression}
          <!-- Served bytes currently exceed origin (e.g. critical-CSS inlining
               trades payload for faster first paint). Don't paint a green
               "saved" headline or claim a "0% reduction" next to growing
               numbers — say it plainly. -->
          <div class="hero-value hero-value-regression">No net savings</div>
          <div class="hero-detail">
            Served bytes exceed origin by {formatBytes(savedBytesOut - savedBytesIn)}
            ({formatBytes(savedBytesIn)} origin, {formatBytes(savedBytesOut)} served) —
            critical-CSS inlining trades payload for faster first paint.
          </div>
        {:else}
          <div class="hero-value">{formatBytes(savedBytesDelta)}</div>
          <div class="hero-detail">
            {savedPercent}% reduction ({formatBytes(savedBytesIn)} &rarr; {formatBytes(
              savedBytesOut,
            )})
          </div>
        {/if}
      </div>
    {/if}

    <!-- Key Metrics Cards -->
    <h2 class="section-title">Counters <span class="since-restart">(since restart{formattedUptime ? ` \u2014 ${formattedUptime} ago` : ''})</span></h2>
    <div class="stat-cards">
      <div class="ps-card stat-card">
        <div class="stat-label">
          Notifications <HelpIcon
            tooltip="Cache miss notifications received from nginx. Each notification triggers an optimization task in the worker."
          />
        </div>
        <div class="stat-value">{formatNumber(totalNotifications)}</div>
        <div class="stat-detail">
          {formatNumber(stats.notifications.skipped_dedup)} deduped, {formatNumber(
            stats.notifications.skipped_inflight,
          )} in-flight <HelpIcon
            tooltip="Deduped: variant already cached. In-flight: same URL already being processed in the thread pool."
          />
        </div>
      </div>

      <div class="ps-card stat-card">
        <div class="stat-label">
          Variants Written <HelpIcon
            tooltip="Optimized variants written to cache (WebP, AVIF, resized, compressed). Includes both on-demand and proactive variants."
          />
        </div>
        <div class="stat-value">{formatNumber(variantsWritten)}</div>
        <div class="stat-detail">
          {formatNumber(proactiveVariants)} proactive <HelpIcon
            tooltip="Variants pre-generated for other viewports/formats before they are requested."
          />
        </div>
      </div>

      <div class="ps-card stat-card">
        <div class="stat-label">
          Compression <HelpIcon
            tooltip="Pre-compressed variants generated (gzip + brotli). These are served directly by nginx for zero-copy delivery."
          />
        </div>
        <div class="stat-value">
          {formatNumber(stats.variants.gzip + stats.variants.brotli)}
        </div>
        <div class="stat-detail">
          {formatNumber(stats.variants.gzip)} gzip,
          {formatNumber(stats.variants.brotli)} brotli
        </div>
      </div>

      <div class="ps-card stat-card">
        <div class="stat-label">
          Errors <HelpIcon
            tooltip="Total processing errors. Includes image decode failures, transcoding errors, and cache write failures."
          />
        </div>
        <div class="stat-value" class:stat-value-warning={errorsIncreasing}>
          <a href="/console/logs" class="stat-link">{formatNumber(totalErrors)}</a>
        </div>
        <div class="stat-detail">
          {#if alternateWriteFailures > 0}
            <a href="/console/metrics" class="stat-link"
              >{formatNumber(alternateWriteFailures)} write failures</a
            >
            <HelpIcon
              tooltip="Failed cache write operations, typically due to disk pressure or permissions."
            />
          {:else}
            No write failures
          {/if}
        </div>
      </div>

      <div class="ps-card stat-card">
        <div class="stat-label">
          Connections <HelpIcon
            tooltip="Active management connections (nginx ↔ worker IPC) vs maximum allowed. High ratio may indicate connection saturation."
          />
        </div>
        <div class="stat-value">
          {formatNumber(activeConnections)}<span class="stat-max"
            >/{formatNumber(maxConnections)}</span
          >
        </div>
        <div class="stat-detail">active / max</div>
      </div>

      <div class="ps-card stat-card">
        <div class="stat-label">
          Thread Pool <HelpIcon
            tooltip="Tasks currently in-flight / thread pool size. When in-flight equals pool size, new tasks queue."
          />
        </div>
        <div class="stat-value">
          {formatNumber(threadPoolInflight)}<span class="stat-max"
            >/{formatNumber(threadPoolSize)}</span
          >
        </div>
        <div class="stat-detail">in-flight / pool size</div>
      </div>

      <div class="ps-card stat-card">
        <div class="stat-label">
          Cache <HelpIcon
            tooltip="Total number of cache entries (alternates) and their combined disk space usage."
          />
        </div>
        <div class="stat-value">{formatNumber(cacheEntries)}</div>
        <div class="stat-detail">{formatBytes(cacheSizeBytes)}</div>
      </div>

      <div class="ps-card stat-card">
        <div class="stat-label">
          Cache Hits <HelpIcon
            tooltip="Optimized responses served from cache by the front-end (nginx or the in-process ASP.NET middleware). Includes HTML, CSS, JS, and image hits. Counters are maintained via shared memory."
          />
        </div>
        <div class="stat-value">{formatNumber(totalServeHits)}</div>
        <div class="stat-detail">{formatNumber(selectorInvocations)} selector invocations</div>
      </div>
    </div>

    <!-- Charts Section -->
    <div class="charts-section">
      <h2 class="section-title">Activity</h2>
      <div class="charts-grid">
        <div class="ps-card chart-card">
          <TimeSeriesChart
            title="Throughput (per update)"
            timestamps={chartTimestamps}
            series={[
              {
                label: 'Notifications',
                color: chartColors.accent,
                data: chartSeries[0] ?? [],
              },
              {
                label: 'Variants Written',
                color: chartColors.success,
                data: chartSeries[1] ?? [],
              },
            ]}
          />
        </div>
        <div class="ps-card chart-card">
          <TimeSeriesChart
            title="Cache (entries and size)"
            timestamps={chartTimestamps}
            series={[
              {
                label: 'Entries',
                color: chartColors.info,
                data: chartSeries[2] ?? [],
              },
              {
                label: 'Size (MB)',
                color: chartColors.warning,
                data: chartSeries[3] ?? [],
              },
            ]}
          />
        </div>
        <div class="ps-card chart-card">
          <TimeSeriesChart
            title="Health (errors and thread pool)"
            timestamps={chartTimestamps}
            series={[
              {
                label: 'Errors (/s)',
                color: chartColors.error,
                data: chartSeries[4] ?? [],
              },
              {
                label: 'Inflight',
                color: chartColors.muted,
                data: chartSeries[5] ?? [],
              },
            ]}
          />
        </div>
      </div>
    </div>

    <!-- Tables Section -->
    <div class="tables-row">
      <!-- Content Type Breakdown -->
      <div class="ps-card table-card">
        <h2 class="section-title">Processing by Type</h2>
        <table class="stats-table">
          <thead>
            <tr>
              <th scope="col">Type</th>
              <th scope="col" class="num-col">Processed</th>
              <th scope="col" class="num-col">Avg Time</th>
            </tr>
          </thead>
          <tbody>
            {#each byTypeEntries as entry}
              <tr>
                <td>
                  <span class="type-label">{entry.label}</span>
                </td>
                <td class="num-col">{formatNumber(entry.count)}</td>
                <td class="num-col">
                  {entry.count > 0
                    ? formatDuration(Math.round(entry.time_us / entry.count))
                    : '--'}
                </td>
              </tr>
            {/each}
          </tbody>
        </table>
      </div>

      <!-- Image Format Breakdown -->
      <div class="ps-card table-card">
        <h2 class="section-title">Image Formats</h2>
        <table class="stats-table">
          <thead>
            <tr>
              <th scope="col">Format</th>
              <th scope="col" class="num-col">Count</th>
            </tr>
          </thead>
          <tbody>
            {#each byFormatEntries as entry}
              <tr>
                <td>{entry.label}</td>
                <td class="num-col">{formatNumber(entry.count)}</td>
              </tr>
            {/each}
          </tbody>
        </table>
      </div>
    </div>

    <!-- Second row of tables -->
    <div class="tables-row">
      <!-- HTML Assembly -->
      <div class="ps-card table-card">
        <h2 class="section-title">HTML Assembly</h2>
        <div class="kv-grid">
          <span class="kv-label"
            >Complete <HelpIcon
              tooltip="HTML pages fully processed with all CSS inlined and optimizations applied."
            /></span
          >
          <span class="kv-value">{formatNumber(htmlAssemblyComplete)}</span>
          <span class="kv-label"
            >Skipped <HelpIcon
              tooltip="HTML pages skipped because the content type or size did not qualify for processing."
            /></span
          >
          <span class="kv-value">{formatNumber(htmlAssemblySkipped)}</span>
          <span class="kv-label"
            >CSS Aborted <HelpIcon
              tooltip="HTML processing aborted because referenced CSS was not yet cached. The page will be reprocessed once the CSS is available."
            /></span
          >
          <span class="kv-value" class:stat-value-error={htmlAssemblyCssAborted > 0}>
            <a href="/console/urls" class="stat-link"
              >{formatNumber(htmlAssemblyCssAborted)}</a
            >
          </span>
        </div>
      </div>

      <!-- SSIMULACRA2 Quality -->
      <div class="ps-card table-card">
        <h2 class="section-title">
          Quality (SSIMULACRA2) <HelpIcon
            tooltip="SSIMULACRA2 is a perceptual image quality metric. Scores above 70 are visually indistinguishable from the original. Re-encodes happen when the first attempt scores below the configured target."
          />
        </h2>
        <div class="kv-grid">
          <span class="kv-label">Checks</span>
          <span class="kv-value">{formatNumber(ssimChecks)}</span>
          <span class="kv-label">Re-encodes</span>
          <span class="kv-value">{formatNumber(ssimReencodes)}</span>
          <span class="kv-label">Avg Score</span>
          <span class="kv-value">{ssimAvgScore}</span>
        </div>
      </div>

      <!-- Learned Quality -->
      <div class="ps-card table-card">
        <h2 class="section-title">
          Learned Quality <HelpIcon
            tooltip="ML models predict optimal encoder quality (~5\u00B5s). Fallbacks use iterative binary search when prediction confidence is low."
          />
        </h2>
        <div class="kv-grid">
          <span class="kv-label">Predictions</span>
          <span class="kv-value">{formatNumber(learnedQualityPredictions)}</span>
          <span class="kv-label">Fallbacks</span>
          <span class="kv-value">{formatNumber(learnedQualityFallbacks)}</span>
        </div>
      </div>

      <!-- Content Analysis -->
      <div class="ps-card table-card">
        <h2 class="section-title">Content Analysis</h2>
        <table class="stats-table">
          <thead>
            <tr>
              <th scope="col">Classification</th>
              <th scope="col" class="num-col">Count</th>
            </tr>
          </thead>
          <tbody>
            {#each contentAnalysisEntries as entry}
              <tr>
                <td>{entry.label}</td>
                <td class="num-col">{formatNumber(entry.count)}</td>
              </tr>
            {/each}
          </tbody>
        </table>
      </div>
    </div>

    <!-- Alternates + SVG -->
    <div class="tables-row">
      <div class="ps-card table-card">
        <h2 class="section-title">Variants</h2>
        <div class="kv-grid">
          <span class="kv-label">Writes</span>
          <span class="kv-value">{formatNumber(alternateWrites)}</span>
          <span class="kv-label">Failures</span>
          <span class="kv-value" class:stat-value-error={alternateWriteFailures > 0}>
            {formatNumber(alternateWriteFailures)}
          </span>
        </div>
      </div>

      <div class="ps-card table-card">
        <h2 class="section-title">
          SVG Auto-Vectorization <HelpIcon
            tooltip="SVG vectorization converts simple raster images (logos, icons, illustrations) into resolution-independent SVG. Candidates are evaluated based on content analysis; photos and noisy images are rejected."
          />
        </h2>
        <div class="kv-grid">
          <span class="kv-label">Candidates Evaluated</span>
          <span class="kv-value">{formatNumber(svgCandidatesEvaluated)}</span>
          <span class="kv-label">Candidates Rejected</span>
          <span class="kv-value">{formatNumber(svgCandidatesRejected)}</span>
          <span class="kv-label">Vectorized</span>
          <span class="kv-value">{formatNumber(svgVectorized)}</span>
          <span class="kv-label">Fidelity Rejected</span>
          <span class="kv-value">{formatNumber(svgFidelityRejected)}</span>
          <span class="kv-label">Timeout Exceeded</span>
          <span class="kv-value">{formatNumber(svgTimeoutExceeded)}</span>
          <span class="kv-label">Size Rejected</span>
          <span class="kv-value">{formatNumber(svgSizeRejected)}</span>
          <span class="kv-label">Path Count Rejected</span>
          <span class="kv-value">{formatNumber(svgPathCountRejected)}</span>
          <span class="kv-label">Written</span>
          <span class="kv-value">{formatNumber(svgWritten)}</span>
          <span class="kv-label">Bytes Saved</span>
          <span class="kv-value">{formatBytes(svgBytesSaved)}</span>
          <span class="kv-label">Served</span>
          <span class="kv-value">{formatNumber(svgServed)}</span>
        </div>
      </div>

      <!-- Dedup -->
      <div class="ps-card table-card">
        <h2 class="section-title">
          Deduplication <HelpIcon
            tooltip="Content deduplication avoids redundant cache writes when identical bytes would be stored under different alternate IDs."
          />
        </h2>
        <div class="kv-grid">
          <span class="kv-label">Writes Skipped</span>
          <span class="kv-value">{formatNumber(dedupWritesSkipped)}</span>
          <span class="kv-label">Content Hash Hits</span>
          <span class="kv-value">{formatNumber(dedupContentHashHits)}</span>
          <span class="kv-label">Content Hash Stale</span>
          <span class="kv-value">{formatNumber(dedupContentHashStale)}</span>
        </div>
      </div>
    </div>

    <!-- Cache Reliability -->
    <div class="tables-row">
      <div class="ps-card table-card">
        <h2 class="section-title">
          Cache Reliability <HelpIcon
            tooltip="Cache read retries and auto-heals indicate transient corruption recovery. Persistent failures may indicate disk issues."
          />
        </h2>
        <div class="kv-grid">
          <span class="kv-label">Read Retries</span>
          <span class="kv-value">{formatNumber(cacheReadRetries)}</span>
          <span class="kv-label">Read Failures</span>
          <span class="kv-value" class:stat-value-error={cacheReadFailures > 0}>
            {formatNumber(cacheReadFailures)}
          </span>
          <span class="kv-label">Auto Heals</span>
          <span class="kv-value">{formatNumber(cacheAutoHeals)}</span>
          <span class="kv-label">Auto Heal Exhausted</span>
          <span class="kv-value" class:stat-value-warning={cacheAutoHealExhausted > 0}>
            {formatNumber(cacheAutoHealExhausted)}
          </span>
          <span class="kv-label">Deferred Retries</span>
          <span class="kv-value">{formatNumber(cacheDeferredRetries)}</span>
          <span class="kv-label">Deferred Successes</span>
          <span class="kv-value">{formatNumber(cacheDeferredSuccesses)}</span>
          <span class="kv-label">Incomplete Matrices</span>
          <span class="kv-value">{formatNumber(imageIncompleteMatrices)}</span>
        </div>
      </div>
    </div>

    <!-- Browser Analysis -->
    {#if browserEnabled}
      <div class="tables-row">
        <div class="ps-card table-card">
          <h2 class="section-title">
            Browser Analysis <HelpIcon
              tooltip="Browser analysis uses headless Chrome to extract critical CSS, detect LCP candidates, and validate lazy loading. Stats update when browser analysis is enabled via --enable-browser-analysis."
            />
          </h2>
          <div class="kv-grid">
            <span class="kv-label">Chrome</span>
            <span class="kv-value">
              {#if browserChromeRunning}
                <span class="ps-badge ps-badge-success">Running</span>
              {:else}
                <span class="ps-badge ps-badge-warning">Stopped</span>
              {/if}
            </span>
            <span class="kv-label">Profiles Generated</span>
            <span class="kv-value">{formatNumber(browserProfilesGenerated)}</span>
            <span class="kv-label">Profiles Used</span>
            <span class="kv-value">{formatNumber(browserProfilesUsed)}</span>
            <span class="kv-label">Queue Depth</span>
            <span class="kv-value">{formatNumber(browserQueueDepth)}</span>
            <span class="kv-label">Analysis Errors</span>
            <span class="kv-value">
              {formatNumber(browserAnalysisErrors)}
            </span>
            <span class="kv-label">Chrome Crashes</span>
            <span class="kv-value" class:stat-value-warning={browserChromeCrashes > 3}>
              {formatNumber(browserChromeCrashes)}
            </span>
          </div>
        </div>
      </div>
    {/if}

    <!-- Policy -->
    {#if stats.policy}
      <div class="tables-row">
        <div class="ps-card table-card">
          <h2 class="section-title">
            Policy <HelpIcon
              tooltip="Optimization policies computed from browser analysis. Controls which transforms are applied per-URL template."
            />
          </h2>
          <div class="kv-grid">
            <span class="kv-label">Policies Computed</span>
            <span class="kv-value">{formatNumber(policyComputed)}</span>
            <span class="kv-label">Async CSS Enabled</span>
            <span class="kv-value">{formatNumber(policyAsyncCssEnabled)}</span>
            <span class="kv-label">Script Deferral Enabled</span>
            <span class="kv-value">{formatNumber(policyScriptDeferralEnabled)}</span>
          </div>
        </div>
      </div>
    {/if}
  </div>
{/if}

<style>
  /* Layout */
  .dashboard {
    padding: var(--ps-space-lg) var(--ps-space-xl);
    max-width: 1200px;
  }

  .dashboard-disconnected {
    display: flex;
    align-items: center;
    justify-content: center;
    min-height: 60vh;
    padding: var(--ps-space-xl);
  }

  .disconnected-card {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: var(--ps-space-md);
    max-width: 320px;
    text-align: center;
  }

  .disconnected-text {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    color: var(--ps-fg-secondary);
    margin: 0;
  }

  /* Header */
  .dashboard-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-bottom: var(--ps-space-lg);
    flex-wrap: wrap;
    gap: var(--ps-space-sm);
  }

  .dashboard-title {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-lg);
    font-weight: 600;
    color: var(--ps-fg-primary);
    margin: 0;
  }

  .dashboard-header-right {
    display: flex;
    align-items: center;
    gap: var(--ps-space-md);
    flex-wrap: wrap;
  }

  .export-buttons {
    display: flex;
    gap: var(--ps-space-xs);
  }

  .uptime-text {
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-secondary);
  }

  /* Stat Cards Grid */
  .stat-cards {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(180px, 1fr));
    gap: var(--ps-space-md);
    margin-bottom: var(--ps-space-xl);
  }

  .stat-card {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-xs);
    padding: var(--ps-space-md) var(--ps-space-lg);
  }

  .stat-label {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    font-weight: 500;
    color: var(--ps-fg-secondary);
    text-transform: uppercase;
    letter-spacing: 0.03em;
  }

  .stat-value {
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-lg);
    font-weight: 600;
    color: var(--ps-fg-primary);
    line-height: 1.2;
  }

  .stat-value-error {
    color: var(--ps-error);
  }

  .stat-value-warning {
    color: var(--ps-warning);
  }

  .stat-max {
    font-weight: 400;
    color: var(--ps-fg-muted);
    font-size: var(--ps-font-size);
  }

  .stat-detail {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-muted);
  }

  /* Charts */
  .charts-section {
    margin-bottom: var(--ps-space-xl);
  }

  .charts-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(320px, 1fr));
    gap: var(--ps-space-md);
    margin-top: var(--ps-space-md);
  }

  .chart-card {
    padding: var(--ps-space-md) var(--ps-space-lg);
    overflow: hidden;
  }

  /* Tables */
  .tables-row {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(280px, 1fr));
    gap: var(--ps-space-md);
    margin-bottom: var(--ps-space-lg);
  }

  .table-card {
    overflow: hidden;
  }

  .section-title {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    font-weight: 600;
    color: var(--ps-fg-primary);
    margin: 0 0 var(--ps-space-md);
  }

  .since-restart {
    font-weight: 400;
    font-size: calc(var(--ps-font-size) * 0.85);
    color: var(--ps-fg-muted);
  }

  .stats-table {
    width: 100%;
    border-collapse: collapse;
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
  }

  .stats-table th {
    text-align: left;
    font-weight: 500;
    color: var(--ps-fg-secondary);
    font-size: var(--ps-font-size-sm);
    padding: var(--ps-space-xs) var(--ps-space-sm);
    border-bottom: 1px solid var(--ps-border);
  }

  .stats-table td {
    padding: var(--ps-space-xs) var(--ps-space-sm);
    color: var(--ps-fg-primary);
    border-bottom: 1px solid var(--ps-border);
  }

  .stats-table tbody tr:last-child td {
    border-bottom: none;
  }

  .num-col {
    text-align: right;
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-sm);
  }

  .type-label {
    font-weight: 500;
  }

  /* Key-Value Grid */
  .kv-grid {
    display: grid;
    grid-template-columns: auto 1fr;
    gap: var(--ps-space-xs) var(--ps-space-lg);
    align-items: baseline;
  }

  .kv-label {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-secondary);
  }

  .kv-value {
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size);
    color: var(--ps-fg-primary);
    text-align: right;
  }

  /* Responsive */
  @media (max-width: 767px) {
    .dashboard {
      padding: var(--ps-space-md);
    }

    .stat-cards {
      grid-template-columns: repeat(2, 1fr);
    }

    .charts-grid {
      grid-template-columns: 1fr;
    }

    .tables-row {
      grid-template-columns: 1fr;
    }
  }

  /* -- Onboarding card ---------------------------------------------------- */

  .dashboard-onboarding {
    display: flex;
    align-items: center;
    justify-content: center;
    min-height: 50vh;
    padding: var(--ps-space-xl);
  }

  .onboarding-card {
    text-align: center;
    padding: var(--ps-space-xl) var(--ps-space-xl);
    max-width: 480px;
  }

  .onboarding-check {
    font-size: 48px;
    line-height: 1;
    display: block;
    margin-bottom: var(--ps-space-md);
  }

  .onboarding-title {
    margin: 0 0 var(--ps-space-sm);
    font-size: 20px;
    font-weight: 600;
  }

  .onboarding-text {
    margin: 0 0 var(--ps-space-lg);
    color: var(--ps-fg-secondary);
  }

  .onboarding-actions {
    display: flex;
    gap: var(--ps-space-sm);
    justify-content: center;
    flex-wrap: wrap;
  }

  /* -- Hero metric card --------------------------------------------------- */

  .hero-card {
    text-align: center;
    padding: var(--ps-space-lg) var(--ps-space-xl);
    margin-bottom: var(--ps-space-lg);
    background: var(--ps-bg-tertiary);
  }

  .hero-label {
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-secondary);
    text-transform: uppercase;
    letter-spacing: 0.05em;
    margin-bottom: var(--ps-space-xs);
  }

  .hero-value {
    font-size: 32px;
    font-weight: 700;
    color: var(--ps-success);
  }

  .hero-value-regression {
    color: var(--ps-warning);
  }

  .hero-detail {
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-muted);
    margin-top: var(--ps-space-xs);
  }

  /* -- Health summary ----------------------------------------------------- */

  .dashboard-header-meta {
    display: flex;
    align-items: center;
    gap: var(--ps-space-md);
  }

  .health-summary {
    display: inline-flex;
    align-items: center;
    gap: var(--ps-space-xs);
    font-size: var(--ps-font-size-sm);
  }

  .health-healthy {
    color: var(--ps-success);
  }

  .health-warning {
    color: var(--ps-warning);
  }

  .health-error {
    color: var(--ps-error);
  }

  /* -- Stat links --------------------------------------------------------- */

  .stat-link {
    color: inherit;
    text-decoration: none;
  }

  .stat-link:hover {
    text-decoration: underline;
    color: var(--ps-fg-link);
  }
</style>

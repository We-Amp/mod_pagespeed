<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  import { getContext, onDestroy } from 'svelte';
  import type { Readable } from 'svelte/store';
  import { formatBytes } from '@pagespeed/api-client';
  import type {
    ApiTransport,
    ConnectionManager,
    ConnectionState,
    StatsResponse,
  } from '@pagespeed/api-client';
  import {
    exportJson,
    exportCsv,
    timestampedFilename,
  } from '#lib/export.ts';
  import HelpIcon from '#lib/components/HelpIcon.svelte';

  // -- Context ---------------------------------------------------------------

  const transport = getContext<ApiTransport>('api');
  const connectionManager = getContext<ConnectionManager>('connection');

  const connState: Readable<ConnectionState> = connectionManager.state;

  // -- Reactive state --------------------------------------------------------

  let stats: StatsResponse | null = $state(null);
  let connected = $state(false);

  // Subscribe to connection state store.
  const unsubConn = connState.subscribe((s) => {
    connected = s === 'connected';
  });

  // -- HTTP polling fallback --------------------------------------------------

  let pollTimer: ReturnType<typeof setInterval> | undefined;

  function startPolling() {
    stopPolling();
    fetchStats();
    pollTimer = setInterval(fetchStats, 2000);
  }

  function stopPolling() {
    if (pollTimer !== undefined) {
      clearInterval(pollTimer);
      pollTimer = undefined;
    }
  }

  async function fetchStats() {
    try {
      const data = await transport.get<StatsResponse>('/v1/stats');
      stats = data;
    } catch {
      // Ignore -- health polling in the layout handles reconnection.
    }
  }

  $effect(() => {
    if (connected) {
      startPolling();
    } else {
      stopPolling();
    }
  });

  // -- Cleanup ---------------------------------------------------------------

  onDestroy(() => {
    stopPolling();
    unsubConn();
  });

  // -- Derived values --------------------------------------------------------

  // Stats-based metrics (always available when connected).
  let variantsWritten = $derived(stats?.variants.written ?? 0);
  let proactiveVariants = $derived(stats?.variants.proactive ?? 0);
  let gzipCount = $derived(stats?.variants.gzip ?? 0);
  let brotliCount = $derived(stats?.variants.brotli ?? 0);
  let compressionCount = $derived(gzipCount + brotliCount);
  let cacheEntries = $derived(stats?.cache.entries ?? 0);
  let cacheSizeBytes = $derived(stats?.cache.size_bytes ?? 0);

  // By-type breakdown with real serve-time savings.
  type TypeEntry = {
    label: string;
    key: 'html' | 'css' | 'js' | 'image';
    count: number;
    timeUs: number;
    originalBytes: number;
    optimizedBytes: number;
    hits: number;
  };

  let byTypeEntries = $derived.by((): TypeEntry[] => {
    if (!stats?.by_type) return [];
    const ss = stats.serve_savings;
    return [
      {
        label: 'HTML',
        key: 'html',
        count: stats.by_type.html.count,
        timeUs: stats.by_type.html.time_us,
        originalBytes: ss?.html?.original_bytes ?? 0,
        optimizedBytes: ss?.html?.optimized_bytes ?? 0,
        hits: ss?.html?.hits ?? 0,
      },
      {
        label: 'CSS',
        key: 'css',
        count: stats.by_type.css.count,
        timeUs: stats.by_type.css.time_us,
        originalBytes: ss?.css?.original_bytes ?? 0,
        optimizedBytes: ss?.css?.optimized_bytes ?? 0,
        hits: ss?.css?.hits ?? 0,
      },
      {
        label: 'JavaScript',
        key: 'js',
        count: stats.by_type.js.count,
        timeUs: stats.by_type.js.time_us,
        originalBytes: ss?.js?.original_bytes ?? 0,
        optimizedBytes: ss?.js?.optimized_bytes ?? 0,
        hits: ss?.js?.hits ?? 0,
      },
      {
        label: 'Image',
        key: 'image',
        count: stats.by_type.image.count,
        timeUs: stats.by_type.image.time_us,
        originalBytes: ss?.image?.original_bytes ?? 0,
        optimizedBytes: ss?.image?.optimized_bytes ?? 0,
        hits: ss?.image?.hits ?? 0,
      },
    ];
  });

  let totalTypeCount = $derived(
    byTypeEntries.reduce((sum, e) => sum + e.count, 0),
  );

  // By-format breakdown.
  let byFormatEntries = $derived(
    stats?.by_format
      ? ([
          { label: 'WebP', count: stats.by_format.webp },
          { label: 'AVIF', count: stats.by_format.avif },
          { label: 'JPEG', count: stats.by_format.jpeg },
          { label: 'PNG', count: stats.by_format.png },
        ] as Array<{ label: string; count: number }>)
      : [],
  );

  let totalFormatCount = $derived(
    byFormatEntries.reduce((sum, e) => sum + e.count, 0),
  );

  // Serve-time savings from real nginx counters.
  let totalOriginalBytes = $derived(
    byTypeEntries.reduce((sum, e) => sum + e.originalBytes, 0),
  );
  let totalOptimizedBytes = $derived(
    byTypeEntries.reduce((sum, e) => sum + e.optimizedBytes, 0),
  );
  let totalHits = $derived(
    byTypeEntries.reduce((sum, e) => sum + e.hits, 0),
  );
  let totalBytesSaved = $derived(
    totalOriginalBytes > totalOptimizedBytes
      ? totalOriginalBytes - totalOptimizedBytes
      : 0,
  );
  let totalSavingsPercent = $derived(
    totalOriginalBytes > 0
      ? (totalBytesSaved / totalOriginalBytes) * 100
      : 0,
  );

  let hasData = $derived(variantsWritten > 0 || totalTypeCount > 0);

  // -- Helpers ---------------------------------------------------------------

  // formatBytes imported from @pagespeed/api-client

  function formatNumber(n: number): string {
    return n.toLocaleString();
  }

  function formatPercent(n: number): string {
    return n.toFixed(1) + '%';
  }

  function formatDuration(microseconds: number): string {
    if (microseconds === 0) return '0 \u00B5s';
    if (microseconds < 1000) return `${microseconds} \u00B5s`;
    if (microseconds < 1_000_000)
      return `${(microseconds / 1000).toFixed(1)} ms`;
    return `${(microseconds / 1_000_000).toFixed(2)} s`;
  }

  // -- Export handlers -------------------------------------------------------

  function handleExportJson() {
    if (!stats) return;
    const payload = {
      exported_at: new Date().toISOString(),
      by_type: byTypeEntries.map((e) => ({
        type: e.label,
        count: e.count,
        time_us: e.timeUs,
        original_bytes: e.originalBytes,
        optimized_bytes: e.optimizedBytes,
        bytes_saved: e.originalBytes > e.optimizedBytes ? e.originalBytes - e.optimizedBytes : 0,
        savings_percent: e.originalBytes > 0
          ? Number((((e.originalBytes - e.optimizedBytes) / e.originalBytes) * 100).toFixed(1))
          : 0,
        hits: e.hits,
      })),
      by_format: byFormatEntries.map((e) => ({
        format: e.label,
        count: e.count,
        share:
          totalFormatCount > 0
            ? Number(((e.count / totalFormatCount) * 100).toFixed(1))
            : 0,
      })),
      totals: {
        type_count: totalTypeCount,
        format_count: totalFormatCount,
        original_bytes: totalOriginalBytes,
        optimized_bytes: totalOptimizedBytes,
        bytes_saved: totalBytesSaved,
        savings_percent: Number(totalSavingsPercent.toFixed(1)),
        hits: totalHits,
        variants_written: variantsWritten,
        proactive_variants: proactiveVariants,
        compression: {
          gzip: gzipCount,
          brotli: brotliCount,
          total: compressionCount,
        },
        cache: {
          entries: cacheEntries,
          size_bytes: cacheSizeBytes,
        },
      },
    };
    exportJson(payload, timestampedFilename('pagespeed-savings', 'json'));
  }

  function handleExportCsv() {
    if (!stats) return;
    const headers = [
      'Category',
      'Item',
      'Original Bytes',
      'Optimized Bytes',
      'Bytes Saved',
      'Savings %',
      'Hits',
    ];
    const rows: (string | number)[][] = [];

    // By-type rows.
    for (const entry of byTypeEntries) {
      const saved = entry.originalBytes > entry.optimizedBytes
        ? entry.originalBytes - entry.optimizedBytes : 0;
      const pct = entry.originalBytes > 0
        ? Number((((entry.originalBytes - entry.optimizedBytes) / entry.originalBytes) * 100).toFixed(1))
        : 0;
      rows.push([
        'By Type',
        entry.label,
        entry.originalBytes,
        entry.optimizedBytes,
        saved,
        pct,
        entry.hits,
      ]);
    }

    // By-format rows.
    for (const entry of byFormatEntries) {
      rows.push(['By Format', entry.label, '', '', '', '', entry.count]);
    }

    // Summary row.
    rows.push([
      'Summary',
      'Total',
      totalOriginalBytes,
      totalOptimizedBytes,
      totalBytesSaved,
      Number(totalSavingsPercent.toFixed(1)),
      totalHits,
    ]);

    exportCsv(headers, rows, timestampedFilename('pagespeed-savings', 'csv'));
  }
</script>

{#if !connected}
  <div class="savings-disconnected">
    <div class="ps-card disconnected-card">
      <div class="ps-spinner" aria-label="Connecting to savings report"></div>
      <p class="disconnected-text">Waiting for worker connection...</p>
      <span class="ps-badge ps-badge-warning">Disconnected</span>
    </div>
  </div>
{:else if !stats}
  <div class="savings-disconnected">
    <div class="ps-card disconnected-card">
      <div class="ps-spinner" aria-label="Loading savings data"></div>
      <p class="disconnected-text">Loading statistics...</p>
    </div>
  </div>
{:else if !hasData}
  <div class="savings-disconnected">
    <div class="ps-card disconnected-card">
      <p class="disconnected-text">No optimization data yet.</p>
      <span class="ps-badge ps-badge-info">
        Waiting for traffic to generate savings data
      </span>
    </div>
  </div>
{:else}
  <div class="savings">
    <!-- Header -->
    <div class="savings-header">
      <div class="savings-header-left">
        <h1 class="savings-title">Bandwidth Savings <HelpIcon tooltip="Real serve-time savings measured by nginx. Only counts traffic served from cache — uncached requests are not included. Counters accumulate since worker start." /></h1>
        <span class="ps-badge ps-badge-success">Connected</span>
      </div>
      <div class="savings-header-actions">
        <div class="export-buttons">
          <button
            class="ps-btn ps-btn-secondary"
            onclick={handleExportJson}
            disabled={!stats}
            title="Export savings as JSON"
          >
            Export JSON
          </button>
          <button
            class="ps-btn ps-btn-secondary"
            onclick={handleExportCsv}
            disabled={!stats}
            title="Export savings as CSV"
          >
            Export CSV
          </button>
        </div>
      </div>
    </div>

    <!-- Summary Cards -->
    <div class="stat-cards">
      <div class="ps-card stat-card">
        <div class="stat-label">Bytes Saved</div>
        <div class="stat-value" class:stat-value-positive={totalBytesSaved > 0}>
          {totalBytesSaved > 0 ? formatBytes(totalBytesSaved) : '--'}
        </div>
        <div class="stat-detail">
          {#if totalOriginalBytes > 0}
            {formatBytes(totalOriginalBytes)} original, {formatBytes(totalOptimizedBytes)} optimized
          {:else}
            No serve-time data yet
          {/if}
        </div>
      </div>

      <div class="ps-card stat-card">
        <div class="stat-label">Savings Rate <HelpIcon tooltip="Percentage of bandwidth saved. Typical savings: images 30-60%, CSS/JS 15-40%, HTML 5-20%. Depends on content type and optimization settings." /></div>
        <div
          class="stat-value"
          class:stat-value-positive={totalSavingsPercent > 0}
        >
          {totalSavingsPercent > 0 ? formatPercent(totalSavingsPercent) : '--'}
        </div>
        <div class="stat-detail">
          {#if totalHits > 0}
            {formatNumber(totalHits)} optimized responses served
          {:else}
            No data
          {/if}
        </div>
      </div>

      <div class="ps-card stat-card">
        <div class="stat-label">Variants Written <HelpIcon tooltip="Total optimized variants written to cache. Includes format conversions (WebP, AVIF), viewport resizes, and quality adjustments." /></div>
        <div class="stat-value">{formatNumber(variantsWritten)}</div>
        <div class="stat-detail">
          {formatNumber(proactiveVariants)} proactive <HelpIcon tooltip="Variants pre-generated for other viewports and formats before being explicitly requested. Reduces first-visit latency." /> (since worker start)
        </div>
      </div>

      <div class="ps-card stat-card">
        <div class="stat-label">Compression <HelpIcon tooltip="Pre-compressed variants stored in cache for zero-copy delivery. Eliminates on-the-fly compression overhead." /></div>
        <div class="stat-value">{formatNumber(compressionCount)}</div>
        <div class="stat-detail">
          {formatNumber(gzipCount)} gzip, {formatNumber(brotliCount)} brotli
        </div>
      </div>

      <div class="ps-card stat-card">
        <div class="stat-label">Cache</div>
        <div class="stat-value">{formatNumber(cacheEntries)}</div>
        <div class="stat-detail">{formatBytes(cacheSizeBytes)} on disk</div>
      </div>
    </div>

    <!-- Tables Section -->
    <div class="tables-row">
      <!-- Content Type Breakdown -->
      <div class="ps-card table-card">
        <h2 class="section-title">Savings by Type</h2>
        <table class="stats-table">
          <thead>
            <tr>
              <th scope="col">Type</th>
              <th scope="col" class="num-col">Original</th>
              <th scope="col" class="num-col">Optimized</th>
              <th scope="col" class="num-col">Saved</th>
              <th scope="col" class="num-col">Savings</th>
              <th scope="col" class="num-col">Hits</th>
            </tr>
          </thead>
          <tbody>
            {#each byTypeEntries as entry}
              {@const saved = entry.originalBytes > entry.optimizedBytes ? entry.originalBytes - entry.optimizedBytes : 0}
              {@const pct = entry.originalBytes > 0 ? ((entry.originalBytes - entry.optimizedBytes) / entry.originalBytes) * 100 : 0}
              <tr>
                <td>
                  <span class="type-label">{entry.label}</span>
                </td>
                <td class="num-col">
                  {entry.originalBytes > 0 ? formatBytes(entry.originalBytes) : '--'}
                </td>
                <td class="num-col">
                  {entry.optimizedBytes > 0 ? formatBytes(entry.optimizedBytes) : '--'}
                </td>
                <td class="num-col">
                  {saved > 0 ? formatBytes(saved) : '--'}
                </td>
                <td class="num-col">
                  {entry.originalBytes > 0 ? formatPercent(pct) : '--'}
                </td>
                <td class="num-col">{formatNumber(entry.hits)}</td>
              </tr>
            {/each}
          </tbody>
          {#if totalOriginalBytes > 0}
            <tfoot>
              <tr class="totals-row">
                <td><span class="type-label">Total</span></td>
                <td class="num-col">{formatBytes(totalOriginalBytes)}</td>
                <td class="num-col">{formatBytes(totalOptimizedBytes)}</td>
                <td class="num-col">{formatBytes(totalBytesSaved)}</td>
                <td class="num-col">{formatPercent(totalSavingsPercent)}</td>
                <td class="num-col">{formatNumber(totalHits)}</td>
              </tr>
            </tfoot>
          {/if}
        </table>
        <p class="table-note">
          Real serve-time savings measured by nginx. Counters accumulate since worker start.
        </p>
      </div>

      <!-- Image Format Breakdown -->
      <div class="ps-card table-card">
        <h2 class="section-title">Image Format Distribution</h2>
        <table class="stats-table">
          <thead>
            <tr>
              <th scope="col">Format</th>
              <th scope="col" class="num-col">Count</th>
              <th scope="col" class="num-col">Share</th>
            </tr>
          </thead>
          <tbody>
            {#each byFormatEntries as entry}
              <tr>
                <td>{entry.label}</td>
                <td class="num-col">{formatNumber(entry.count)}</td>
                <td class="num-col">
                  {totalFormatCount > 0
                    ? formatPercent((entry.count / totalFormatCount) * 100)
                    : '--'}
                </td>
              </tr>
            {/each}
          </tbody>
          {#if totalFormatCount > 0}
            <tfoot>
              <tr class="totals-row">
                <td><span class="type-label">Total</span></td>
                <td class="num-col">{formatNumber(totalFormatCount)}</td>
                <td class="num-col">100.0%</td>
              </tr>
            </tfoot>
          {/if}
        </table>
      </div>
    </div>

    <!-- Serve-time savings detail (shown when savings data is available) -->
    {#if totalOriginalBytes > 0}
      <div class="tables-row">
        <div class="ps-card table-card">
          <h2 class="section-title">Serve-Time Savings Summary</h2>
          <div class="kv-grid">
            <span class="kv-label">Original Size</span>
            <span class="kv-value">{formatBytes(totalOriginalBytes)}</span>
            <span class="kv-label">Optimized Size</span>
            <span class="kv-value">{formatBytes(totalOptimizedBytes)}</span>
            <span class="kv-label">Bytes Saved</span>
            <span class="kv-value kv-value-positive">
              {formatBytes(totalBytesSaved)}
            </span>
            <span class="kv-label">Savings Rate</span>
            <span class="kv-value kv-value-positive">
              {formatPercent(totalSavingsPercent)}
            </span>
            <span class="kv-label">Optimized Hits</span>
            <span class="kv-value">{formatNumber(totalHits)}</span>
          </div>
        </div>
      </div>
    {/if}
  </div>
{/if}

<style>
  /* Layout */
  .savings {
    padding: var(--ps-space-lg) var(--ps-space-xl);
    max-width: 1200px;
  }

  .savings-disconnected {
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
  .savings-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-bottom: var(--ps-space-lg);
    flex-wrap: wrap;
    gap: var(--ps-space-sm);
  }

  .savings-header-left {
    display: flex;
    align-items: center;
    gap: var(--ps-space-sm);
    flex-wrap: wrap;
  }

  .savings-header-actions {
    display: flex;
    align-items: center;
    gap: var(--ps-space-sm);
    flex-wrap: wrap;
  }

  .export-buttons {
    display: flex;
    gap: var(--ps-space-xs);
  }

  .savings-title {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-lg);
    font-weight: 600;
    color: var(--ps-fg-primary);
    margin: 0;
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

  .stat-value-positive {
    color: var(--ps-success);
  }

  .stat-detail {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-muted);
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

  .stats-table tfoot td {
    border-top: 1px solid var(--ps-border);
    border-bottom: none;
    font-weight: 500;
  }

  .totals-row td {
    padding-top: var(--ps-space-sm);
  }

  .num-col {
    text-align: right;
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-sm);
  }

  .type-label {
    font-weight: 500;
  }

  .table-note {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-muted);
    margin: var(--ps-space-sm) 0 0;
    line-height: 1.4;
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

  .kv-value-positive {
    color: var(--ps-success);
  }

  /* Responsive */
  @media (max-width: 767px) {
    .savings {
      padding: var(--ps-space-md);
    }

    .stat-cards {
      grid-template-columns: repeat(2, 1fr);
    }

    .tables-row {
      grid-template-columns: 1fr;
    }
  }
</style>

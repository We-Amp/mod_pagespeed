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
  import { exportJson, timestampedFilename } from '#lib/export.ts';
  import {
    METRIC_DESCRIPTIONS,
    METRIC_CATEGORIES,
    metricCategory,
  } from '#lib/metric-descriptions.ts';

  const transport = getContext<ApiTransport>('api');
  const connectionManager = getContext<ConnectionManager>('connection');
  const connState: Readable<ConnectionState> = connectionManager.state;

  let stats: StatsResponse | null = $state(null);
  let connected = $state(false);
  let filter = $state('');

  const unsubConn = connState.subscribe((s) => {
    connected = s === 'connected';
  });

  // Poll /v1/stats every 2s.
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
      stats = await transport.get<StatsResponse>('/v1/stats');
    } catch {
      // Layout handles reconnection.
    }
  }

  $effect(() => {
    if (connected) {
      startPolling();
    } else {
      stopPolling();
    }
  });

  onDestroy(() => {
    stopPolling();
    unsubConn();
  });

  // Flatten nested stats into a sorted list of {key, value} entries.
  interface MetricEntry {
    key: string;
    value: string;
    description: string;
    raw: number | boolean | string;
    warn: boolean;
    category: string;
  }

  function flatten(obj: Record<string, unknown>, prefix = ''): MetricEntry[] {
    const entries: MetricEntry[] = [];
    for (const [k, v] of Object.entries(obj)) {
      const key = prefix ? `${prefix}.${k}` : k;
      if (v !== null && typeof v === 'object' && !Array.isArray(v)) {
        entries.push(...flatten(v as Record<string, unknown>, key));
      } else {
        const warn =
          (key === 'errors.total' && typeof v === 'number' && v > 0) ||
          (key === 'errors.origin_misconfiguration' && typeof v === 'number' && v > 0) ||
          (key.includes('write_failures') && typeof v === 'number' && v > 0) ||
          (key.includes('analysis_errors') && typeof v === 'number' && v > 0) ||
          (key.includes('chrome_crashes') && typeof v === 'number' && v > 0);
        entries.push({
          key,
          value: formatValue(key, v),
          description: METRIC_DESCRIPTIONS[key] ?? '',
          raw: v as number | boolean | string,
          warn,
          category: metricCategory(key),
        });
      }
    }
    return entries;
  }

  function formatValue(key: string, v: unknown): string {
    if (typeof v === 'boolean') return v ? 'true' : 'false';
    if (typeof v === 'string') return v;
    if (typeof v !== 'number') return String(v);
    if (key.includes('size_bytes')) return `${formatBytes(v)} (${v.toLocaleString()})`;
    if (key.includes('time_us')) return formatDuration(v);
    if (key === 'ssimulacra2.avg_score_x100') return `${(v / 100).toFixed(2)} (${v})`;
    return v.toLocaleString();
  }

  function formatDuration(us: number): string {
    if (us === 0) return '0';
    if (us < 1000) return `${us} \u00B5s`;
    if (us < 1_000_000) return `${(us / 1000).toFixed(1)} ms`;
    return `${(us / 1_000_000).toFixed(2)} s`;
  }

  let allEntries = $derived(
    stats ? flatten(stats as unknown as Record<string, unknown>) : [],
  );
  let filteredEntries = $derived(
    filter
      ? allEntries.filter(
          (e) =>
            e.key.toLowerCase().includes(filter.toLowerCase()) ||
            e.value.toLowerCase().includes(filter.toLowerCase()) ||
            e.description.toLowerCase().includes(filter.toLowerCase()),
        )
      : allEntries,
  );

  /** Group filtered entries by category, preserving original order. */
  interface MetricGroup {
    category: string;
    label: string;
    entries: MetricEntry[];
  }

  let groupedEntries = $derived.by(() => {
    const groups: MetricGroup[] = [];
    const labelToGroup = new Map<string, MetricGroup>();
    for (const entry of filteredEntries) {
      const label = METRIC_CATEGORIES[entry.category] ?? entry.category;
      let group = labelToGroup.get(label);
      if (!group) {
        group = { category: entry.category, label, entries: [] };
        labelToGroup.set(label, group);
        groups.push(group);
      }
      group.entries.push(entry);
    }
    return groups;
  });

  let warnCount = $derived(allEntries.filter((e) => e.warn).length);

  function handleExport() {
    if (!stats) return;
    exportJson(stats, timestampedFilename('pagespeed-metrics', 'json'));
  }
</script>

{#if !connected}
  <div class="metrics-empty">
    <div class="ps-card empty-card">
      <div class="ps-spinner" aria-label="Connecting"></div>
      <p class="empty-text">Waiting for worker connection...</p>
      <span class="ps-badge ps-badge-warning">Disconnected</span>
    </div>
  </div>
{:else if !stats}
  <div class="metrics-empty">
    <div class="ps-card empty-card">
      <div class="ps-spinner" aria-label="Loading"></div>
      <p class="empty-text">Loading metrics...</p>
    </div>
  </div>
{:else}
  <div class="metrics">
    <div class="metrics-header">
      <div class="metrics-header-left">
        <h1 class="metrics-title">All Metrics</h1>
        <span class="ps-badge">{filteredEntries.length} counters</span>
        {#if warnCount > 0}
          <span class="ps-badge ps-badge-warning">{warnCount} warning(s)</span>
        {/if}
      </div>
      <div class="metrics-header-actions">
        <input
          type="text"
          class="ps-input filter-input"
          placeholder="Filter metrics..."
          bind:value={filter}
        />
        <button
          class="ps-btn ps-btn-secondary"
          onclick={handleExport}
          title="Export raw JSON"
        >
          Export JSON
        </button>
      </div>
    </div>

    <div class="ps-card table-card">
      <table class="metrics-table">
        <thead>
          <tr>
            <th scope="col">Metric</th>
            <th scope="col" class="desc-col">Description</th>
            <th scope="col" class="val-col">Value</th>
          </tr>
        </thead>
        <tbody>
          {#each groupedEntries as group (group.category)}
            <tr class="category-row">
              <td colspan="3" class="category-cell">{group.label}</td>
            </tr>
            {#each group.entries as entry (entry.key)}
              <tr class:warn-row={entry.warn}>
                <td class="key-cell">
                  <code class="metric-key">{entry.key}</code>
                </td>
                <td class="desc-cell">
                  <span class="metric-desc">{entry.description}</span>
                </td>
                <td class="val-cell">
                  <span class="metric-value" class:warn-value={entry.warn}>
                    {entry.value}
                  </span>
                </td>
              </tr>
            {/each}
          {/each}
        </tbody>
      </table>
      {#if filteredEntries.length === 0}
        <p class="no-results">No metrics match "{filter}"</p>
      {/if}
    </div>
  </div>
{/if}

<style>
  .metrics {
    padding: var(--ps-space-lg) var(--ps-space-xl);
    max-width: 1100px;
  }

  .metrics-empty {
    display: flex;
    align-items: center;
    justify-content: center;
    min-height: 60vh;
    padding: var(--ps-space-xl);
  }

  .empty-card {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: var(--ps-space-md);
    max-width: 320px;
    text-align: center;
  }

  .empty-text {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    color: var(--ps-fg-secondary);
    margin: 0;
  }

  .metrics-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-bottom: var(--ps-space-lg);
    flex-wrap: wrap;
    gap: var(--ps-space-sm);
  }

  .metrics-header-left {
    display: flex;
    align-items: center;
    gap: var(--ps-space-sm);
  }

  .metrics-header-actions {
    display: flex;
    align-items: center;
    gap: var(--ps-space-sm);
  }

  .metrics-title {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-lg);
    font-weight: 600;
    color: var(--ps-fg-primary);
    margin: 0;
  }

  .filter-input {
    width: 220px;
  }

  .table-card {
    overflow: hidden;
  }

  .metrics-table {
    width: 100%;
    border-collapse: collapse;
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
  }

  .metrics-table th {
    text-align: left;
    font-weight: 500;
    color: var(--ps-fg-secondary);
    font-size: var(--ps-font-size-sm);
    padding: var(--ps-space-xs) var(--ps-space-md);
    border-bottom: 1px solid var(--ps-border);
    position: sticky;
    top: 0;
    background: var(--ps-bg-primary);
  }

  .metrics-table td {
    padding: var(--ps-space-xs) var(--ps-space-md);
    border-bottom: 1px solid var(--ps-border);
  }

  .metrics-table tbody tr:last-child td {
    border-bottom: none;
  }

  .metrics-table tbody tr:not(.category-row):hover {
    background: var(--ps-bg-hover);
  }

  .val-col {
    text-align: right;
    width: 200px;
  }

  .desc-col {
    width: 280px;
  }

  .key-cell {
    white-space: nowrap;
  }

  .val-cell {
    text-align: right;
  }

  .desc-cell {
    color: var(--ps-fg-secondary);
  }

  .metric-key {
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-primary);
  }

  .metric-desc {
    font-size: var(--ps-font-size-sm);
  }

  .metric-value {
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-primary);
  }

  .category-row {
    background: var(--ps-bg-secondary, var(--ps-bg-hover));
  }

  .category-cell {
    font-weight: 600;
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-secondary);
    text-transform: uppercase;
    letter-spacing: 0.05em;
    padding: var(--ps-space-xs) var(--ps-space-md) !important;
  }

  .warn-row {
    background: color-mix(in srgb, var(--ps-warning) 8%, transparent);
  }

  .warn-value {
    color: var(--ps-warning);
    font-weight: 600;
  }

  .no-results {
    padding: var(--ps-space-lg);
    text-align: center;
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    color: var(--ps-fg-muted);
  }

  @media (max-width: 767px) {
    .metrics {
      padding: var(--ps-space-md);
    }
    .filter-input {
      width: 160px;
    }
    .desc-col,
    .desc-cell {
      display: none;
    }
  }
</style>

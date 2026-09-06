<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  import { AdminApiClient } from "$lib/api/client";
  import type { StatsResponse } from "$lib/api/types";
  import { onDestroy } from "svelte";

  const { basePath = "" }: { basePath?: string; isGlobal?: boolean } = $props();
  const api = new AdminApiClient(basePath);

  // ── State ────────────────────────────────────────────────────
  let error = $state<string | null>(null);
  let loading = $state(true);
  let autoRefresh = $state(true);
  let search = $state("");
  let timer: ReturnType<typeof setInterval> | null = null;

  /** Maximum number of snapshots to keep in the ring buffer. */
  const MAX_SNAPSHOTS = 60;

  /** Each snapshot records the timestamp and all variable values. */
  interface Snapshot {
    time: number;
    variables: Record<string, number>;
  }

  let snapshots = $state<Snapshot[]>([]);

  // ── Derived data ─────────────────────────────────────────────

  /** All variable names seen across every snapshot. */
  let allNames = $derived.by(() => {
    const names = new Set<string>();
    for (const snap of snapshots) {
      for (const key of Object.keys(snap.variables)) {
        names.add(key);
      }
    }
    return [...names].sort();
  });

  /** Filtered list of variable names based on search. */
  let filteredNames = $derived.by(() => {
    if (!search) return allNames;
    const q = search.toLowerCase();
    return allNames.filter((n) => n.toLowerCase().includes(q));
  });

  /** For each variable, build the time-series array (one value per snapshot). */
  function seriesFor(name: string): number[] {
    return snapshots.map((s) => s.variables[name] ?? 0);
  }

  /** Latest value for a variable. */
  function latestValue(name: string): number {
    if (snapshots.length === 0) return 0;
    return snapshots[snapshots.length - 1].variables[name] ?? 0;
  }

  /** Change from first to last snapshot. */
  function deltaValue(name: string): number {
    if (snapshots.length < 2) return 0;
    const first = snapshots[0].variables[name] ?? 0;
    const last = snapshots[snapshots.length - 1].variables[name] ?? 0;
    return last - first;
  }

  let lastUpdated = $derived(
    snapshots.length > 0
      ? new Date(snapshots[snapshots.length - 1].time).toLocaleTimeString()
      : null,
  );

  // ── Sorting ──────────────────────────────────────────────────
  let sortKey = $state<"name" | "value" | "delta">("name");
  let sortAsc = $state(true);

  let sortedNames = $derived.by(() => {
    const items = filteredNames.slice();
    items.sort((a, b) => {
      let cmp: number;
      if (sortKey === "name") {
        cmp = a.localeCompare(b);
      } else if (sortKey === "value") {
        cmp = latestValue(a) - latestValue(b);
      } else {
        cmp = deltaValue(a) - deltaValue(b);
      }
      return sortAsc ? cmp : -cmp;
    });
    return items;
  });

  function toggleSort(key: "name" | "value" | "delta") {
    if (sortKey === key) {
      sortAsc = !sortAsc;
    } else {
      sortKey = key;
      sortAsc = true;
    }
  }

  function sortIndicator(key: "name" | "value" | "delta"): string {
    if (sortKey !== key) return "";
    return sortAsc ? " \u25B2" : " \u25BC";
  }

  // ── Sparkline SVG builder ────────────────────────────────────
  function sparklinePath(values: number[]): string {
    if (values.length === 0) return "";
    const max = Math.max(...values, 1);
    const w = 200;
    const h = 24;
    const barW = w / values.length;
    return values
      .map((v, i) => {
        const barH = (v / max) * h;
        const x = i * barW;
        const y = h - barH;
        return `<rect x="${x}" y="${y}" width="${Math.max(barW - 0.5, 0.5)}" height="${barH}" fill="var(--ps-primary)" opacity="0.7"/>`;
      })
      .join("");
  }

  // ── Fetching / polling ───────────────────────────────────────
  async function fetchData() {
    try {
      const stats: StatsResponse = await api.getStats();
      if (stats.variables) {
        const snap: Snapshot = {
          time: stats.timestamp ? stats.timestamp * 1000 : Date.now(),
          variables: stats.variables,
        };
        snapshots = [...snapshots.slice(-(MAX_SNAPSHOTS - 1)), snap];
      }
      error = null;
    } catch (err) {
      error = err instanceof Error ? err.message : String(err);
    } finally {
      loading = false;
    }
  }

  function startPolling() {
    stopPolling();
    fetchData();
    timer = setInterval(fetchData, 5000);
  }

  function stopPolling() {
    if (timer !== null) {
      clearInterval(timer);
      timer = null;
    }
  }

  function toggleAutoRefresh() {
    autoRefresh = !autoRefresh;
    if (autoRefresh) {
      startPolling();
    } else {
      stopPolling();
    }
  }

  function clearHistory() {
    snapshots = [];
  }

  // Start on mount.
  startPolling();
  onDestroy(() => stopPolling());
</script>

<div class="page">
  <div class="header">
    <h1>Console</h1>
    <div class="controls">
      <button class="btn btn-secondary" onclick={toggleAutoRefresh}>
        {autoRefresh ? "Pause" : "Resume"}
      </button>
      <button class="btn btn-primary" onclick={fetchData}>
        Refresh
      </button>
      <button class="btn btn-secondary" onclick={clearHistory}>
        Clear History
      </button>
    </div>
  </div>

  {#if loading && snapshots.length === 0}
    <p class="loading">Loading statistics...</p>
  {:else if error && snapshots.length === 0}
    <p class="error">{error}</p>
  {:else}
    <div class="summary-bar">
      <span class="summary-item">
        <strong>{allNames.length}</strong> variables
      </span>
      <span class="summary-item">
        <strong>{snapshots.length}</strong> snapshots collected
      </span>
      {#if lastUpdated}
        <span class="summary-item">
          Last updated: {lastUpdated}
        </span>
      {/if}
      {#if error}
        <span class="summary-item summary-error">Fetch error: {error}</span>
      {/if}
    </div>

    <div class="toolbar">
      <input
        type="text"
        class="search-input"
        placeholder="Filter variables..."
        bind:value={search}
      />
      <span class="count">
        {filteredNames.length} of {allNames.length} variables
      </span>
    </div>

    <div class="table-wrapper">
      <table>
        <thead>
          <tr>
            <th class="sortable" onclick={() => toggleSort("name")}>
              Variable{sortIndicator("name")}
            </th>
            <th class="sortable th-value" onclick={() => toggleSort("value")}>
              Latest{sortIndicator("value")}
            </th>
            <th class="sortable th-delta" onclick={() => toggleSort("delta")}>
              Delta{sortIndicator("delta")}
            </th>
            <th class="th-sparkline">Trend</th>
          </tr>
        </thead>
        <tbody>
          {#each sortedNames as name (name)}
            {@const series = seriesFor(name)}
            {@const latest = latestValue(name)}
            {@const delta = deltaValue(name)}
            <tr>
              <td class="name-cell">{name}</td>
              <td class="value-cell">{latest.toLocaleString()}</td>
              <td class="delta-cell" class:positive={delta > 0} class:negative={delta < 0}>
                {#if delta > 0}+{/if}{delta.toLocaleString()}
              </td>
              <td class="sparkline-cell">
                {#if series.length > 1}
                  <svg viewBox="0 0 200 24" preserveAspectRatio="none" width="120" height="24">
                    {@html sparklinePath(series)}
                  </svg>
                {:else}
                  <span class="no-trend">--</span>
                {/if}
              </td>
            </tr>
          {:else}
            <tr>
              <td colspan="4" class="empty">No matching variables found.</td>
            </tr>
          {/each}
        </tbody>
      </table>
    </div>
  {/if}
</div>

<style>
  .page {
    max-width: 1100px;
  }

  .header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-bottom: var(--ps-space-md);
    flex-wrap: wrap;
    gap: var(--ps-space-sm);
  }

  h1 {
    margin: 0;
  }

  .controls {
    display: flex;
    gap: var(--ps-space-sm);
    align-items: center;
  }

  .summary-bar {
    display: flex;
    gap: var(--ps-space-lg);
    margin-bottom: var(--ps-space-md);
    padding: var(--ps-space-sm) var(--ps-space-md);
    background: var(--ps-bg-tertiary);
    border-radius: var(--ps-border-radius);
    font-size: var(--ps-font-size-sm);
    flex-wrap: wrap;
  }

  .summary-item {
    color: var(--ps-text-secondary);
  }

  .summary-error {
    color: var(--ps-error);
  }

  .toolbar {
    display: flex;
    align-items: center;
    gap: var(--ps-space-md);
    margin-bottom: var(--ps-space-md);
    flex-wrap: wrap;
  }

  .search-input {
    flex: 1;
    min-width: 200px;
    padding: var(--ps-space-sm) var(--ps-space-md);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
    font-size: var(--ps-font-size-sm);
    background: var(--ps-bg);
    color: var(--ps-text);
  }

  .search-input:focus {
    outline: none;
    border-color: var(--ps-primary);
    box-shadow: 0 0 0 2px var(--ps-primary-light);
  }

  .count {
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-secondary);
    white-space: nowrap;
  }

  .table-wrapper {
    overflow-x: auto;
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
    max-height: 70vh;
    overflow-y: auto;
  }

  table {
    width: 100%;
    border-collapse: collapse;
    font-size: var(--ps-font-size-sm);
  }

  thead {
    background: var(--ps-bg-tertiary);
    position: sticky;
    top: 0;
    z-index: 1;
  }

  th {
    text-align: left;
    padding: var(--ps-space-sm) var(--ps-space-md);
    font-weight: 600;
    border-bottom: 2px solid var(--ps-border);
    white-space: nowrap;
  }

  th.sortable {
    cursor: pointer;
    user-select: none;
  }

  th.sortable:hover {
    background: var(--ps-surface-hover);
  }

  .th-value,
  .th-delta {
    text-align: right;
  }

  .th-sparkline {
    text-align: center;
    min-width: 130px;
  }

  td {
    padding: var(--ps-space-sm) var(--ps-space-md);
    border-bottom: 1px solid var(--ps-border-light);
  }

  tbody tr:nth-child(even) {
    background: var(--ps-bg-secondary);
  }

  tbody tr:hover {
    background: var(--ps-surface-hover);
  }

  .name-cell {
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-xs);
    word-break: break-all;
  }

  .value-cell {
    font-family: var(--ps-font-mono);
    text-align: right;
    white-space: nowrap;
  }

  .delta-cell {
    font-family: var(--ps-font-mono);
    text-align: right;
    white-space: nowrap;
    color: var(--ps-text-tertiary);
  }

  .delta-cell.positive {
    color: #16a34a;
  }

  .delta-cell.negative {
    color: #dc2626;
  }

  .sparkline-cell {
    text-align: center;
    vertical-align: middle;
  }

  .sparkline-cell svg {
    display: inline-block;
    vertical-align: middle;
  }

  .no-trend {
    color: var(--ps-text-tertiary);
    font-size: var(--ps-font-size-xs);
  }

  .btn {
    padding: var(--ps-space-sm) var(--ps-space-md);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
    font-size: var(--ps-font-size-sm);
    cursor: pointer;
    white-space: nowrap;
  }

  .btn-primary {
    background: var(--ps-primary);
    color: var(--ps-text-inverse);
    border-color: var(--ps-primary);
  }

  .btn-primary:hover {
    background: var(--ps-primary-hover);
  }

  .btn-secondary {
    background: var(--ps-bg);
    color: var(--ps-text);
  }

  .btn-secondary:hover {
    background: var(--ps-surface-hover);
  }

  .loading {
    color: var(--ps-text-secondary);
  }

  .error {
    color: var(--ps-error);
  }

  .empty {
    text-align: center;
    color: var(--ps-text-tertiary);
    padding: var(--ps-space-xl);
  }

  @media (max-width: 600px) {
    .header {
      flex-direction: column;
      align-items: flex-start;
    }

    .toolbar {
      flex-direction: column;
      align-items: stretch;
    }

    .search-input {
      min-width: unset;
    }
  }
</style>

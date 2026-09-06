<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  import { AdminApiClient } from "$lib/api/client";
  import type { ConsoleResponse, TimeRangeParams } from "$lib/api/types";
  import RefreshNotice from "$lib/RefreshNotice.svelte";
  import {
    graphTitle,
    latestForDisplay,
    loadDeltaView,
    saveDeltaView,
    seriesForDisplay,
    type Sample,
  } from "$lib/utils/graph-series";

  const { basePath = "" }: { basePath?: string; isGlobal?: boolean } = $props();
  const api = new AdminApiClient(basePath);

  let data = $state<ConsoleResponse | null>(null);
  let error = $state<string | null>(null);
  let loading = $state(false);

  let timeRange = $state("15");
  let autoRefresh = $state(true);
  // Cumulative counters stay the default view; the per-interval view is opt-in
  // and remembered across page loads.
  let deltaView = $state(loadDeltaView());
  let search = $state("");
  let timer: ReturnType<typeof setInterval> | null = null;

  const timeRangeOptions = [
    { label: "Last 5 minutes", value: "5" },
    { label: "Last 15 minutes", value: "15" },
    { label: "Last 30 minutes", value: "30" },
    { label: "Last 1 hour", value: "60" },
    { label: "Last 6 hours", value: "360" },
    { label: "Last 24 hours", value: "1440" },
  ];

  function buildParams(): TimeRangeParams {
    const minutes = parseInt(timeRange, 10);
    const now = Date.now();
    return {
      start: now - minutes * 60 * 1000,
      end: now,
    };
  }

  /**
   * Normalise the backend response into the ConsoleResponse shape the UI expects.
   *
   * The backend returns:  { timestamps: number[], variables: { name: number[] } }
   * The UI expects:       { timestamps: number[], graphs: { name: string, data: number[] }[] }
   */
  function normaliseResponse(result: any): ConsoleResponse {
    // If the response was HTML (SPA shell fallback) the client returns {raw: ...}.
    if ("raw" in result && typeof result.raw === "string") {
      return { graphs: [], timestamps: [] };
    }
    // If the backend returned {error: ...}, surface it.
    if (result.error && typeof result.error === "string") {
      return { graphs: [], timestamps: result.timestamps ?? [] };
    }
    // Already in the expected shape?
    if (Array.isArray(result.graphs)) {
      return result as ConsoleResponse;
    }
    // Transform {variables: {name: number[]}} -> {graphs: [{name, data}]}
    if (result.variables && typeof result.variables === "object") {
      const graphs = Object.entries(result.variables as Record<string, number[]>).map(
        ([name, values]) => ({ name, data: values }),
      );
      return { graphs, timestamps: result.timestamps ?? [] };
    }
    return { graphs: [], timestamps: [] };
  }

  async function fetchData() {
    loading = data === null;
    error = null;
    try {
      const result = await api.getGraphs(buildParams());
      data = normaliseResponse(result);
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

  function toggleDeltaView() {
    deltaView = !deltaView;
    saveDeltaView(deltaView);
  }

  function toggleAutoRefresh() {
    autoRefresh = !autoRefresh;
    if (autoRefresh) {
      startPolling();
    } else {
      stopPolling();
    }
  }

  $effect(() => {
    void timeRange;
    if (autoRefresh) {
      startPolling();
    } else {
      fetchData();
    }
    return () => stopPolling();
  });

  let filteredGraphs = $derived.by(() => {
    if (!data?.graphs) return [];
    const matching = !search
      ? data.graphs
      : data.graphs.filter((g) => g.name.toLowerCase().includes(search.toLowerCase()));
    // Plot per-interval deltas when the view is on; a counter reset (restart)
    // becomes a gap rather than a negative spike.
    return matching.map((g) => ({
      name: g.name,
      title: graphTitle(g.name, deltaView),
      series: seriesForDisplay(g.data, deltaView),
    }));
  });

  /** The plottable values of a series, with gaps dropped. */
  function plotted(series: Sample[]): number[] {
    return series.filter((v): v is number => v !== null && Number.isFinite(v));
  }

  function formatTimestamp(ts: number): string {
    return new Date(ts).toLocaleTimeString();
  }

  // Simple sparkline as an inline SVG bar chart. A `null` sample is a gap (no
  // bar) -- that is how a counter reset from a server restart is drawn.
  function sparklinePath(values: Sample[]): string {
    if (values.length === 0) return "";
    const max = Math.max(...plotted(values), 1);
    const w = 300;
    const h = 40;
    const barW = w / values.length;
    return values
      .map((v, i) => {
        if (v === null || !Number.isFinite(v)) return "";
        const barH = (v / max) * h;
        const x = i * barW;
        const y = h - barH;
        return `<rect x="${x}" y="${y}" width="${Math.max(barW - 0.5, 0.5)}" height="${barH}" fill="var(--ps-primary)" opacity="0.7"/>`;
      })
      .join("");
  }
</script>

<div class="page">
  <div class="header">
    <h1>Graphs</h1>
    <div class="controls">
      <select class="select" bind:value={timeRange}>
        {#each timeRangeOptions as opt}
          <option value={opt.value}>{opt.label}</option>
        {/each}
      </select>
      <button class="btn btn-secondary" onclick={toggleAutoRefresh}>
        {autoRefresh ? "Pause" : "Resume"}
      </button>
      <label class="toggle" title="Plot the change per sample interval instead of the cumulative counter. A counter reset (server restart) is drawn as a gap.">
        <input
          type="checkbox"
          data-testid="graphs-delta-toggle"
          checked={deltaView}
          onchange={toggleDeltaView}
        />
        Show per-interval deltas
      </label>
      <button class="btn btn-primary" onclick={fetchData}>
        Refresh
      </button>
    </div>
  </div>

  {#if loading && !data}
    <p class="loading">Loading graph data...</p>
  {:else if error && !data}
    <p class="error">{error}</p>
  {:else if data}
    <RefreshNotice error={error} />
    {#if (data.graphs?.length ?? 0) > 0}
      <div class="toolbar">
        <input
          type="text"
          class="search-input"
          placeholder="Filter graphs by name..."
          bind:value={search}
        />
        <span class="count">
          {filteredGraphs.length} of {data.graphs?.length ?? 0} graphs
        </span>
        {#if data.timestamps && data.timestamps.length > 0}
          <span class="timestamp">
            {formatTimestamp(data.timestamps[0])} -- {formatTimestamp(data.timestamps[data.timestamps.length - 1])}
          </span>
        {/if}
      </div>

      {#if filteredGraphs.length > 0}
        <div class="graphs-grid">
          {#each filteredGraphs as graph (graph.name)}
            {@const points = plotted(graph.series)}
            {@const latest = latestForDisplay(graph.series)}
            <div class="graph-card">
              <div class="graph-header">
                <h3 class="graph-name">{graph.title}</h3>
                <span class="graph-stats">
                  {#if points.length > 0}
                    <!-- A gap on the final sample has no value to report, so
                         Latest shows a dash rather than an earlier interval's
                         number. Min/Max still summarise the samples present. -->
                    Latest: {latest === null ? "\u2014" : latest.toLocaleString()}
                    | Min: {Math.min(...points).toLocaleString()}
                    | Max: {Math.max(...points).toLocaleString()}
                  {/if}
                </span>
              </div>
              <div class="sparkline">
                <svg viewBox="0 0 300 40" preserveAspectRatio="none" width="100%" height="40">
                  {@html sparklinePath(graph.series)}
                </svg>
              </div>
              <details class="data-details">
                <summary>Raw data ({graph.series.length} point{graph.series.length === 1 ? "" : "s"})</summary>
                <pre class="data-block">{JSON.stringify(graph.series, null, 2)}</pre>
              </details>
            </div>
          {/each}
        </div>
      {:else}
        <p class="empty">No graphs match your filter.</p>
      {/if}
    {:else}
      <div class="empty-state" data-testid="graphs-empty-state">
        <p class="empty-title">No Graph Data Available</p>
        <p class="empty-description">
          This server does not expose the time-series graphs endpoint.
          View statistics on the <a href="#/statistics">Statistics</a> page instead.
        </p>
      </div>
    {/if}
  {:else}
    <div class="empty-state" data-testid="graphs-empty-state">
      <p class="empty-title">No Graph Data Available</p>
      <p class="empty-description">
        This server does not expose the time-series graphs endpoint.
        View statistics on the <a href="#/statistics">Statistics</a> page instead.
      </p>
    </div>
  {/if}
</div>

<style>
  .page {
    max-width: 960px;
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

  .timestamp {
    font-size: var(--ps-font-size-xs);
    color: var(--ps-text-tertiary);
    white-space: nowrap;
  }

  .select {
    padding: var(--ps-space-sm) var(--ps-space-md);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
    font-size: var(--ps-font-size-sm);
    background: var(--ps-bg);
    color: var(--ps-text);
  }

  .toggle {
    display: flex;
    align-items: center;
    gap: var(--ps-space-xs);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-secondary);
    white-space: nowrap;
    cursor: pointer;
  }

  .toggle input {
    cursor: pointer;
    margin: 0;
  }

  .graphs-grid {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-md);
  }

  .graph-card {
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
    overflow: hidden;
  }

  .graph-header {
    padding: var(--ps-space-sm) var(--ps-space-md);
    background: var(--ps-bg-tertiary);
    border-bottom: 1px solid var(--ps-border-light);
    display: flex;
    align-items: center;
    justify-content: space-between;
    flex-wrap: wrap;
    gap: var(--ps-space-sm);
  }

  .graph-name {
    font-size: var(--ps-font-size-sm);
    font-family: var(--ps-font-mono);
    margin: 0;
  }

  .graph-stats {
    font-size: var(--ps-font-size-xs);
    color: var(--ps-text-tertiary);
  }

  .sparkline {
    padding: var(--ps-space-sm) var(--ps-space-md);
    background: var(--ps-bg-secondary);
    border-bottom: 1px solid var(--ps-border-light);
  }

  .sparkline svg {
    display: block;
  }

  .data-details {
    font-size: var(--ps-font-size-sm);
  }

  .data-details summary {
    padding: var(--ps-space-sm) var(--ps-space-md);
    cursor: pointer;
    color: var(--ps-text-secondary);
    font-size: var(--ps-font-size-xs);
  }

  .data-details summary:hover {
    background: var(--ps-surface-hover);
  }

  .data-block {
    margin: 0;
    padding: var(--ps-space-sm) var(--ps-space-md);
    max-height: 200px;
    overflow: auto;
    font-size: var(--ps-font-size-xs);
    background: var(--ps-bg-secondary);
    border-top: 1px solid var(--ps-border-light);
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
    color: var(--ps-text-tertiary);
  }

  .empty-state {
    text-align: center;
    padding: var(--ps-space-xl) var(--ps-space-lg);
    border: 1px dashed var(--ps-border);
    border-radius: var(--ps-border-radius-lg);
    background: var(--ps-bg-secondary);
  }

  .empty-title {
    font-size: var(--ps-font-size-lg);
    font-weight: 600;
    color: var(--ps-text-secondary);
    margin-bottom: var(--ps-space-sm);
  }

  .empty-description {
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-tertiary);
    line-height: 1.5;
  }

  .empty-description a {
    color: var(--ps-primary);
    text-decoration: underline;
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

    .graph-header {
      flex-direction: column;
      align-items: flex-start;
    }
  }
</style>

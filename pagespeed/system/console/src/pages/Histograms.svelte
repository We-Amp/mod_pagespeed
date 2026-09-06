<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  import { AdminApiClient } from "$lib/api/client";
  import { usePolling } from "$lib/api/polling.svelte";
  import RefreshNotice from "$lib/RefreshNotice.svelte";

  const { basePath = "" }: { basePath?: string; isGlobal?: boolean } = $props();
  const api = new AdminApiClient(basePath);
  const histograms = usePolling(() => api.getHistograms(), 10000);

  let search = $state("");
  let selectedIndex = $state(0);

  // The backend emits a -5000 sentinel for percentile stats when a histogram
  // has too few samples to compute them. These are latency/size values that are
  // never legitimately negative, so render an en-dash instead of "-5000".
  function fmtStat(v: string): string {
    const n = Number(v.replace(/,/g, ""));
    return Number.isFinite(n) && n < 0 ? "\u2013" : v;
  }

  interface HistogramRow {
    name: string;
    count: string;
    avg: string;
    stddev: string;
    min: string;
    median: string;
    max: string;
    p90: string;
    p95: string;
    p99: string;
  }

  interface HistogramDetail {
    buckets: Array<{
      lower: string;
      upper: string;
      count: string;
      pct: string;
      cumPct: string;
      barWidth: number;
    }>;
  }

  /**
   * Parse the backend HTML into structured histogram data.
   * The backend returns an HTML string with:
   * - A summary <table> with rows for each histogram
   * - Hidden <div id="hist_N"> elements with bucket detail tables
   * - A <script> block (which we discard)
   */
  function parseHistograms(html: string): {
    rows: HistogramRow[];
    details: HistogramDetail[];
  } {
    if (!html) return { rows: [], details: [] };

    const parser = new DOMParser();
    const doc = parser.parseFromString(html, "text/html");

    // Parse summary table rows
    const rows: HistogramRow[] = [];
    const tableRows = doc.querySelectorAll("tbody tr");
    for (const tr of tableRows) {
      const cells = tr.querySelectorAll("td");
      if (cells.length < 10) continue;
      // First cell contains a label with a radio + the name
      const nameEl = cells[0]?.querySelector("label");
      const name = nameEl?.textContent?.trim() ?? cells[0]?.textContent?.trim() ?? "";
      rows.push({
        name,
        count: cells[1]?.textContent?.trim() ?? "",
        avg: cells[2]?.textContent?.trim() ?? "",
        stddev: cells[3]?.textContent?.trim() ?? "",
        min: cells[4]?.textContent?.trim() ?? "",
        median: cells[5]?.textContent?.trim() ?? "",
        max: cells[6]?.textContent?.trim() ?? "",
        p90: cells[7]?.textContent?.trim() ?? "",
        p95: cells[8]?.textContent?.trim() ?? "",
        p99: cells[9]?.textContent?.trim() ?? "",
      });
    }

    // Parse detail divs (hist_0, hist_1, ...)
    const details: HistogramDetail[] = [];
    let i = 0;
    while (true) {
      const div = doc.getElementById(`hist_${i}`);
      if (!div) break;
      const buckets: HistogramDetail["buckets"] = [];
      const detailRows = div.querySelectorAll("table tr");
      for (const tr of detailRows) {
        const cells = tr.querySelectorAll("td");
        if (cells.length < 6) continue;
        // cells: [lower-bracket, lower-val, upper-val, count, pct, cumPct, bar-div]
        const lower = (cells[0]?.textContent ?? "") + (cells[1]?.textContent ?? "");
        const upper = cells[2]?.textContent?.trim() ?? "";
        const count = cells[3]?.textContent?.trim() ?? "";
        const pct = cells[4]?.textContent?.trim() ?? "";
        const cumPct = cells[5]?.textContent?.trim() ?? "";
        // Extract bar width from inline style
        const barDiv = cells[6]?.querySelector("div");
        const widthMatch = barDiv?.getAttribute("style")?.match(/width:\s*(\d+)/);
        const barWidth = widthMatch ? parseInt(widthMatch[1], 10) : 0;
        buckets.push({ lower, upper, count, pct, cumPct, barWidth });
      }
      details.push({ buckets });
      i++;
    }

    return { rows, details };
  }

  let parsed = $derived.by(() => {
    const html =
      histograms.data?.histograms ??
      (histograms.data as Record<string, unknown>)?.raw ??
      "";
    return parseHistograms(String(html));
  });

  let filteredIndices = $derived.by(() => {
    if (!parsed.rows.length) return [];
    const indices = parsed.rows.map((_, i) => i);
    if (!search) return indices;
    const q = search.toLowerCase();
    return indices.filter((i) =>
      parsed.rows[i].name.toLowerCase().includes(q),
    );
  });

  // Ensure selectedIndex is valid after filtering.
  let effectiveSelected = $derived(
    filteredIndices.includes(selectedIndex)
      ? selectedIndex
      : (filteredIndices[0] ?? 0),
  );

  function selectHistogram(index: number) {
    selectedIndex = index;
  }

  function toggleAutoRefresh() {
    if (histograms.autoRefresh) {
      histograms.stop();
    } else {
      histograms.start();
    }
  }
</script>

<div class="page">
  <div class="header">
    <h1>Histograms</h1>
    <div class="controls">
      <button class="btn btn-secondary" onclick={toggleAutoRefresh}>
        {histograms.autoRefresh ? "Pause" : "Resume"} Auto-Refresh
      </button>
      <button class="btn btn-primary" onclick={() => histograms.refresh()}>
        Refresh Now
      </button>
    </div>
  </div>

  {#if histograms.loading}
    <p class="loading">Loading histograms...</p>
  {:else if histograms.error && !histograms.data}
    <p class="error">{histograms.error.message}</p>
  {:else if parsed.rows.length === 0}
    <p class="empty">No histogram data available.</p>
  {:else}
    <RefreshNotice error={histograms.error} />
    <div class="toolbar">
      <input
        type="text"
        class="search-input"
        placeholder="Filter histograms by name..."
        bind:value={search}
      />
    </div>

    {#if filteredIndices.length === 0}
      <p class="empty">No histograms match your filter.</p>
    {:else}
      <div class="histogram-wrapper">
        <table class="histogram-table">
          <thead>
            <tr>
              <th>Histogram Name</th>
              <th>Count</th>
              <th>Avg</th>
              <th>StdDev</th>
              <th>Min</th>
              <th>Median</th>
              <th>Max</th>
              <th>90%</th>
              <th>95%</th>
              <th>99%</th>
            </tr>
          </thead>
          <tbody>
            {#each filteredIndices as idx}
              {@const row = parsed.rows[idx]}
              <tr
                class="summary-row"
                class:selected={effectiveSelected === idx}
                onclick={() => selectHistogram(idx)}
              >
                <td class="name-cell">{row.name}</td>
                <td class="num-cell">{row.count}</td>
                <td class="num-cell">{fmtStat(row.avg)}</td>
                <td class="num-cell">{fmtStat(row.stddev)}</td>
                <td class="num-cell">{fmtStat(row.min)}</td>
                <td class="num-cell">{fmtStat(row.median)}</td>
                <td class="num-cell">{fmtStat(row.max)}</td>
                <td class="num-cell">{fmtStat(row.p90)}</td>
                <td class="num-cell">{fmtStat(row.p95)}</td>
                <td class="num-cell">{fmtStat(row.p99)}</td>
              </tr>
            {/each}
          </tbody>
        </table>
      </div>

      {#if parsed.details[effectiveSelected]?.buckets.length}
        <div class="detail-wrapper">
          <h3 class="detail-title">{parsed.rows[effectiveSelected]?.name}</h3>
          <table class="detail-table">
            <thead>
              <tr>
                <th>Bucket</th>
                <th>Count</th>
                <th>%</th>
                <th>Cumulative %</th>
                <th>Distribution</th>
              </tr>
            </thead>
            <tbody>
              {#each parsed.details[effectiveSelected].buckets as bucket}
                <tr>
                  <td class="bucket-range">{bucket.lower}{bucket.upper}</td>
                  <td class="num-cell">{bucket.count}</td>
                  <td class="num-cell">{bucket.pct}</td>
                  <td class="num-cell">{bucket.cumPct}</td>
                  <td class="bar-cell">
                    <div
                      class="bar"
                      style="width: {Math.max(
                        2,
                        Math.round((bucket.barWidth / 400) * 100),
                      )}%"
                    ></div>
                  </td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
      {/if}
    {/if}
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
  }

  .toolbar {
    margin-bottom: var(--ps-space-md);
  }

  .search-input {
    width: 100%;
    max-width: 400px;
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

  .histogram-wrapper {
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
    overflow: auto;
    max-height: 50vh;
    margin-bottom: var(--ps-space-md);
  }

  .histogram-table {
    width: 100%;
    border-collapse: collapse;
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-sm);
  }

  .histogram-table thead {
    position: sticky;
    top: 0;
    background: var(--ps-bg-secondary);
    z-index: 1;
  }

  .histogram-table th {
    padding: var(--ps-space-sm) var(--ps-space-md);
    text-align: left;
    font-weight: 600;
    border-bottom: 2px solid var(--ps-border);
    white-space: nowrap;
  }

  .histogram-table td {
    padding: var(--ps-space-xs) var(--ps-space-md);
    border-bottom: 1px solid var(--ps-border);
  }

  .summary-row {
    cursor: pointer;
    transition: background-color 0.15s;
  }

  .summary-row:hover {
    background-color: var(--ps-surface-hover);
  }

  .summary-row.selected {
    background-color: var(--ps-primary-light, #e3f2fd);
  }

  .name-cell {
    font-weight: 500;
    word-break: break-word;
  }

  .num-cell {
    text-align: right;
    white-space: nowrap;
  }

  .detail-wrapper {
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
    padding: var(--ps-space-md);
    background: var(--ps-bg-secondary);
    overflow: auto;
    max-height: 40vh;
  }

  .detail-title {
    margin: 0 0 var(--ps-space-sm) 0;
    font-size: var(--ps-font-size-sm);
    font-weight: 600;
  }

  .detail-table {
    width: 100%;
    border-collapse: collapse;
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-sm);
  }

  .detail-table th {
    padding: var(--ps-space-xs) var(--ps-space-md);
    text-align: left;
    font-weight: 600;
    border-bottom: 2px solid var(--ps-border);
    white-space: nowrap;
  }

  .detail-table td {
    padding: var(--ps-space-xs) var(--ps-space-md);
    border-bottom: 1px solid var(--ps-border);
  }

  .bucket-range {
    white-space: nowrap;
    font-family: var(--ps-font-mono);
  }

  .bar-cell {
    width: 40%;
    padding-right: var(--ps-space-md);
  }

  .bar {
    height: 16px;
    background-color: var(--ps-primary, #4285f4);
    border-radius: 2px;
    min-width: 2px;
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

  @media (max-width: 600px) {
    .header {
      flex-direction: column;
      align-items: flex-start;
    }

    .search-input {
      max-width: unset;
    }
  }
</style>

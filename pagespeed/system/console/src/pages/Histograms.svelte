<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  import { usePolling } from "$lib/api/polling.svelte";
  import LoadError from "$lib/LoadError.svelte";
  import PageHeader from "$lib/PageHeader.svelte";
  import RefreshNotice from "$lib/RefreshNotice.svelte";
  import { bucketWidths, bucketShares, unitForHistogram } from "$lib/utils/histograms";
  import { formatCount, formatPercent, formatSig } from "$lib/utils/format";
  import type { HistogramJson } from "$lib/api/types";
  import { useConsole } from "$lib/api/context";

  const { api } = useConsole();

  let updatedAt = $state<number | null>(null);
  const histograms = usePolling(
    () =>
      api.getHistograms().then((h) => {
        updatedAt = Date.now();
        return h;
      }),
    10000,
  );

  let search = $state("");
  let selectedIndex = $state(0);

  let rows = $derived(histograms.data?.histograms ?? []);

  let filteredIndices = $derived.by(() => {
    if (!rows.length) return [];
    const indices = rows.map((_, i) => i);
    if (!search) return indices;
    const q = search.toLowerCase();
    return indices.filter((i) => rows[i].name.toLowerCase().includes(q));
  });

  // Ensure selectedIndex is valid after filtering.
  let effectiveSelected = $derived(
    filteredIndices.includes(selectedIndex)
      ? selectedIndex
      : (filteredIndices[0] ?? 0),
  );

  let selected = $derived.by(
    (): HistogramJson | undefined => rows[effectiveSelected],
  );
  let widths = $derived.by(() => bucketWidths(selected?.buckets ?? []));
  let shares = $derived.by(() =>
    bucketShares(selected?.buckets ?? [], selected?.count ?? 0),
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
  <PageHeader
    title="Histograms"
    updatedAt={updatedAt}
    refresh={{
      autoRefresh: histograms.autoRefresh,
      intervalMs: 10000,
      onToggle: toggleAutoRefresh,
      onRefresh: () => histograms.refresh(),
    }}
  >
    {#snippet toolbar()}
      <input
        type="text"
        class="search-input field field-search"
        placeholder="Filter histograms by name..."
        bind:value={search}
      />
    {/snippet}
  </PageHeader>

  {#if histograms.loading}
    <p class="loading">Loading histograms...</p>
  {:else if histograms.error && !histograms.data}
    <LoadError message={histograms.error.message} />
  {:else if rows.length === 0}
    <p class="empty">No histogram data available.</p>
  {:else}
    <RefreshNotice error={histograms.error} />

    {#if filteredIndices.length === 0}
      <p class="empty">No histograms match your filter.</p>
    {:else}
      <div class="histogram-wrapper">
        <table class="histogram-table">
          <thead>
            <tr>
              <th>Histogram Name</th>
              <th scope="col" class="num-head">Unit</th>
              <th class="num-head">Count</th>
              <th class="num-head">Avg</th>
              <th class="num-head">StdDev</th>
              <th class="num-head">Min</th>
              <th class="num-head">Median</th>
              <th class="num-head">Max</th>
              <th class="num-head">90%</th>
              <th class="num-head">95%</th>
              <th class="num-head">99%</th>
            </tr>
          </thead>
          <tbody>
            {#each filteredIndices as idx}
              {@const row = rows[idx]}
              <tr
                class="summary-row"
                class:selected={effectiveSelected === idx}
                onclick={() => selectHistogram(idx)}
              >
                <td class="name-cell">
                  <button
                    type="button"
                    class="row-select"
                    aria-pressed={effectiveSelected === idx}
                    onclick={(e) => {
                      e.stopPropagation();
                      selectHistogram(idx);
                    }}
                  >{row.name}</button>
                </td>
                <td class="num-cell">{unitForHistogram(row.name)}</td>
                <td class="num-cell num">{formatCount(row.count)}</td>
                <td class="num-cell num">{row.avg === null ? "—" : formatSig(row.avg)}</td>
                <td class="num-cell num">{row.stddev === null ? "—" : formatSig(row.stddev)}</td>
                <td class="num-cell num">{row.min === null ? "—" : formatSig(row.min)}</td>
                <td class="num-cell num">{row.median === null ? "—" : formatSig(row.median)}</td>
                <td class="num-cell num">{row.max === null ? "—" : formatSig(row.max)}</td>
                <td class="num-cell num">{row.p90 === null ? "—" : formatSig(row.p90)}</td>
                <td class="num-cell num">{row.p95 === null ? "—" : formatSig(row.p95)}</td>
                <td class="num-cell num">{row.p99 === null ? "—" : formatSig(row.p99)}</td>
              </tr>
            {/each}
          </tbody>
        </table>
      </div>

      {#if selected?.buckets.length}
        <div class="detail-wrapper">
          <h2 class="detail-title">{selected.name}</h2>
          <table class="detail-table">
            <thead>
              <tr>
                <th>Bucket</th>
                <th class="num-head">Count</th>
                <th class="num-head">%</th>
                <th class="num-head">Cumulative %</th>
                <th>Distribution</th>
              </tr>
            </thead>
            <tbody>
              {#each selected.buckets as bucket, i}
                <tr>
                  <td class="bucket-range">[{bucket.start}, {bucket.limit})</td>
                  <td class="num-cell">{bucket.count}</td>
                  <td class="num-cell">{formatPercent(shares[i].percent)}</td>
                  <td class="num-cell">{formatPercent(shares[i].cumulative)}</td>
                  <td class="bar-cell">
                    <div class="bar" style="width: {Math.max(2, widths[i])}%"
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
    word-break: keep-all;
  }

  .row-select {
    padding: 0;
    border: 0;
    background: none;
    color: inherit;
    font: inherit;
    font-weight: 500;
    text-align: left;
    cursor: pointer;
  }

  /* A numeric column's header sits over its right-aligned values. */
  .histogram-table th.num-head,
  .detail-table th.num-head {
    text-align: right;
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

  .loading {
    color: var(--ps-text-secondary);
  }

  .empty {
    color: var(--ps-text-secondary);
  }
</style>

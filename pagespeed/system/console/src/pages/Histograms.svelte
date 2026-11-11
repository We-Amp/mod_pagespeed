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
    <LoadError error={histograms.error} noun="the histograms" onretry={() => histograms.refresh()} />
  {:else if rows.length === 0}
    <p class="muted">No histogram data available.</p>
  {:else}
    <RefreshNotice error={histograms.error} />

    {#if filteredIndices.length === 0}
      <p class="muted">No histograms match your filter.</p>
    {:else}
      <!-- A table that may scroll sideways is a named region that takes keyboard focus (WCAG 2.1.1). -->
      <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
      <div class="table-frame table-frame-tall histogram-wrapper" role="region" aria-label="Histogram summary" tabindex="0">
        <table class="data data-dense histogram-table">
          <thead>
            <tr>
              <th>Histogram Name</th>
              <th scope="col" class="num">Unit</th>
              <th class="num">Count</th>
              <th class="num">Avg</th>
              <th class="num">StdDev</th>
              <th class="num">Min</th>
              <th class="num">Median</th>
              <th class="num">Max</th>
              <th class="num">90%</th>
              <th class="num">95%</th>
              <th class="num">99%</th>
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
                <td class="num">{unitForHistogram(row.name)}</td>
                <td class="num">{formatCount(row.count)}</td>
                <td class="num">{row.avg === null ? "—" : formatSig(row.avg)}</td>
                <td class="num">{row.stddev === null ? "—" : formatSig(row.stddev)}</td>
                <td class="num">{row.min === null ? "—" : formatSig(row.min)}</td>
                <td class="num">{row.median === null ? "—" : formatSig(row.median)}</td>
                <td class="num">{row.max === null ? "—" : formatSig(row.max)}</td>
                <td class="num">{row.p90 === null ? "—" : formatSig(row.p90)}</td>
                <td class="num">{row.p95 === null ? "—" : formatSig(row.p95)}</td>
                <td class="num">{row.p99 === null ? "—" : formatSig(row.p99)}</td>
              </tr>
            {/each}
          </tbody>
        </table>
      </div>

      {#if selected?.buckets.length}
        <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
        <div class="card detail-wrapper" role="region" aria-label="Buckets of {selected.name}" tabindex="0">
          <h2 class="minor-title detail-title">{selected.name}</h2>
          <table class="data data-dense detail-table">
            <thead>
              <tr>
                <th>Bucket</th>
                <th class="num">Count</th>
                <th class="num">%</th>
                <th class="num">Cumulative %</th>
                <th>Distribution</th>
              </tr>
            </thead>
            <tbody>
              {#each selected.buckets as bucket, i}
                <tr>
                  <td class="bucket-range">[{bucket.start}, {bucket.limit})</td>
                  <td class="num">{bucket.count}</td>
                  <td class="num">{formatPercent(shares[i].percent)}</td>
                  <td class="num">{formatPercent(shares[i].cumulative)}</td>
                  <td class="bar-cell">
                    <div class="bar bar-fill" style="width: {Math.max(2, widths[i])}%"
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
  .histogram-wrapper {
    margin-bottom: var(--ps-space-md);
  }

  .histogram-table,
  .detail-table {
    font-family: var(--ps-font-mono);
  }

  .summary-row {
    cursor: pointer;
    transition: background-color 0.15s;
  }

  .summary-row.selected {
    background-color: var(--ps-primary-light);
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

  .detail-wrapper {
    overflow: auto;
    max-height: 40vh;
  }

  .bucket-range {
    white-space: nowrap;
  }

  .bar-cell {
    width: 40%;
  }

  /* A bucket's share: the shared bar fill, at this table's height. */
  .bar {
    height: 16px;
    border-radius: 2px;
  }
</style>

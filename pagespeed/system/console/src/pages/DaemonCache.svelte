<script lang="ts">
  // Daemon cache: the optimizer daemon's cache occupancy and serve-savings
  // counters via /v1/daemon/stats. The serve-savings table reuses the
  // Statistics page's sortable-table machinery; daemon counters are not in
  // the module's description table, so they carry the neutral description.
  import { AdminApiClient } from "$lib/api/client";
  import { usePolling } from "$lib/api/polling.svelte";
  import RefreshNotice from "$lib/RefreshNotice.svelte";
  import {
    counterRows,
    fieldValue,
    formatBytes,
    isDaemonUnavailable,
  } from "$lib/utils/daemon";
  import {
    DEFAULT_SORT,
    buildRows,
    nextSortState,
    type SortKey,
    type SortState,
  } from "$lib/utils/stat-table";

  const { basePath = "" }: { basePath?: string; isGlobal?: boolean } = $props();
  const api = new AdminApiClient(basePath);
  const stats = usePolling(() => api.daemonStats(), 5000);

  let search = $state("");
  let sort = $state<SortState>({ ...DEFAULT_SORT });

  let unreachable = $derived(isDaemonUnavailable(stats.error));

  let savingsRows = $derived.by(() => {
    const counters = Object.fromEntries(
      counterRows(stats.data?.serve_savings).map((row) => [row.name, row.value]),
    );
    return buildRows(counters, search, sort);
  });

  function toggleSort(key: SortKey) {
    sort = nextSortState(sort, key);
  }

  function sortIndicator(key: SortKey): string {
    if (sort.key !== key) return "";
    return sort.asc ? " ▲" : " ▼";
  }

  function toggleAutoRefresh() {
    if (stats.autoRefresh) {
      stats.stop();
    } else {
      stats.start();
    }
  }
</script>

<div class="page">
  <div class="header">
    <h1>Daemon Cache</h1>
    <div class="controls">
      <button class="btn btn-secondary" onclick={toggleAutoRefresh}>
        {stats.autoRefresh ? "Pause" : "Resume"} Auto-Refresh
      </button>
      <button class="btn btn-primary" onclick={() => stats.refresh()}>
        Refresh Now
      </button>
    </div>
  </div>

  {#if stats.loading}
    <p class="loading">Loading daemon cache statistics...</p>
  {:else if stats.error && !stats.data}
    {#if unreachable}
      <div class="empty-state" data-testid="daemon-unreachable">
        <p class="empty-title">Daemon Unreachable</p>
        <p class="empty-description">
          The module could not reach the optimizer daemon ({stats.error.message}).
          The daemon may be stopped or not configured, or this build does not
          serve the daemon endpoints; the module keeps serving without it.
          This panel populates once the daemon is reachable.
        </p>
      </div>
    {:else}
      <p class="error">{stats.error.message}</p>
    {/if}
  {:else if stats.data}
    <RefreshNotice error={stats.error} />

    <div class="info-grid">
      <div class="info-item">
        <span class="info-label">Cache entries</span>
        <span class="info-value">{fieldValue(stats.data.cache?.entries)}</span>
      </div>
      <div class="info-item">
        <span class="info-label">Cache size</span>
        <span class="info-value">{formatBytes(stats.data.cache?.size_bytes)}</span>
      </div>
    </div>

    <section class="daemon-section">
      <h2>Serve Savings</h2>
      {#if savingsRows.length > 0 || search}
        <div class="toolbar">
          <input
            type="text"
            class="search-input"
            placeholder="Search counters..."
            bind:value={search}
          />
          <span class="count">{savingsRows.length} counters</span>
        </div>
        <div class="table-wrapper">
          <table>
            <thead>
              <tr>
                <th class="sortable" onclick={() => toggleSort("name")}>
                  Name{sortIndicator("name")}
                </th>
                <th class="sortable" onclick={() => toggleSort("value")}>
                  Value{sortIndicator("value")}
                </th>
              </tr>
            </thead>
            <tbody>
              {#each savingsRows as row (row.name)}
                <tr>
                  <td class="name-cell" title={row.description}>{row.name}</td>
                  <td class="value-cell">{row.value.toLocaleString()}</td>
                </tr>
              {:else}
                <tr>
                  <td colspan="2" class="empty">No matching counters found.</td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
      {:else}
        <p class="empty">The daemon reports no serve-savings counters.</p>
      {/if}
    </section>
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
  }

  .daemon-section {
    margin-top: var(--ps-space-xl);
  }

  h2 {
    font-size: var(--ps-font-size-lg);
    font-weight: 600;
    margin: 0 0 var(--ps-space-md) 0;
    padding-bottom: var(--ps-space-xs);
    border-bottom: 1px solid var(--ps-border);
  }

  .info-grid {
    display: flex;
    flex-wrap: wrap;
    gap: var(--ps-space-lg);
    padding: var(--ps-space-md);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius-lg);
    background: var(--ps-bg-secondary);
  }

  .info-item {
    display: flex;
    flex-direction: column;
    gap: 2px;
  }

  .info-label {
    font-size: var(--ps-font-size-xs);
    font-weight: 600;
    color: var(--ps-text-tertiary);
    text-transform: uppercase;
    letter-spacing: 0.05em;
  }

  .info-value {
    font-size: var(--ps-font-size-sm);
    font-family: var(--ps-font-mono);
    color: var(--ps-text);
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
    word-break: break-all;
    cursor: help;
  }

  .value-cell {
    font-family: var(--ps-font-mono);
    text-align: right;
    white-space: nowrap;
  }

  .empty {
    text-align: center;
    color: var(--ps-text-tertiary);
    padding: var(--ps-space-md);
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

<script lang="ts">
  import { AdminApiClient } from "$lib/api/client";
  import { usePolling } from "$lib/api/polling.svelte";

  const { basePath = "" }: { basePath?: string; isGlobal?: boolean } = $props();
  const api = new AdminApiClient(basePath);
  const stats = usePolling(() => api.getStats(), 5000);

  let search = $state("");
  let sortKey = $state<"name" | "value">("name");
  let sortAsc = $state(true);

  let entries = $derived.by(() => {
    if (!stats.data?.variables) return [];
    let items = Object.entries(stats.data.variables).map(([name, value]) => ({ name, value }));
    if (search) {
      const q = search.toLowerCase();
      items = items.filter((e) => e.name.toLowerCase().includes(q));
    }
    items.sort((a, b) => {
      const cmp =
        sortKey === "name"
          ? a.name.localeCompare(b.name)
          : a.value - b.value;
      return sortAsc ? cmp : -cmp;
    });
    return items;
  });

  let totalCount = $derived(
    stats.data?.variables ? Object.keys(stats.data.variables).length : 0,
  );

  let lastUpdated = $derived(
    stats.data?.timestamp
      ? new Date(stats.data.timestamp * 1000).toLocaleTimeString()
      : null,
  );

  function toggleSort(key: "name" | "value") {
    if (sortKey === key) {
      sortAsc = !sortAsc;
    } else {
      sortKey = key;
      sortAsc = true;
    }
  }

  function sortIndicator(key: "name" | "value"): string {
    if (sortKey !== key) return "";
    return sortAsc ? " \u25B2" : " \u25BC";
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
    <h1>Statistics</h1>
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
    <p class="loading">Loading statistics...</p>
  {:else if stats.error}
    <p class="error">{stats.error.message}</p>
  {:else}
    <div class="toolbar">
      <input
        type="text"
        class="search-input"
        placeholder="Search variables..."
        bind:value={search}
      />
      <span class="count">
        {entries.length} of {totalCount} variables
      </span>
      {#if lastUpdated}
        <span class="timestamp">Last updated: {lastUpdated}</span>
      {/if}
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
          {#each entries as entry (entry.name)}
            <tr>
              <td class="name-cell">{entry.name}</td>
              <td class="value-cell">{entry.value.toLocaleString()}</td>
            </tr>
          {:else}
            <tr>
              <td colspan="2" class="empty">No matching variables found.</td>
            </tr>
          {/each}
        </tbody>
      </table>
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
  }

  .value-cell {
    font-family: var(--ps-font-mono);
    text-align: right;
    white-space: nowrap;
  }

  .empty {
    text-align: center;
    color: var(--ps-text-tertiary);
    padding: var(--ps-space-xl);
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

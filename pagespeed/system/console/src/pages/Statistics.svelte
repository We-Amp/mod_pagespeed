<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  import { usePolling } from "$lib/api/polling.svelte";
  import LoadError from "$lib/LoadError.svelte";
  import PageHeader from "$lib/PageHeader.svelte";
  import RefreshNotice from "$lib/RefreshNotice.svelte";
  import SortableHeader from "$lib/SortableHeader.svelte";
  import ViewSwitch from "$lib/ViewSwitch.svelte";
  import {
    DEFAULT_SORT,
    buildRows,
    groupStatRows,
    loadDeltaColumn,
    loadDescriptionColumn,
    nameSegments,
    nextSortState,
    saveDeltaColumn,
    saveDescriptionColumn,
    type SortKey,
    type SortState,
  } from "$lib/utils/stat-table";
  import { formatCount } from "$lib/utils/format";
  import { statisticsScopeLine } from "$lib/utils/config-scope";
  import { useConsole } from "$lib/api/context";
  import { router } from "$lib/router.svelte";

  const { api, scope } = useConsole();

  /** The first successful poll's variables; "since open" is measured from here. */
  let baseline = $state<Record<string, number> | null>(null);
  let updatedAt = $state<number | null>(null);
  const stats = usePolling(
    () =>
      api.getStats().then((s) => {
        updatedAt = Date.now();
        if (baseline === null && s.variables) baseline = { ...s.variables };
        return s;
      }),
    5000,
  );

  let search = $state("");
  let sort = $state<SortState>({ ...DEFAULT_SORT });
  let showDescriptions = $state(loadDescriptionColumn());
  // The retired Console page's address lands here with delta=1
  // (redirects.ts): its Δ column is turned on, and stays on as the viewer's
  // choice until they turn it off.
  const deltaRequested = router.params.get("delta") === "1";
  if (deltaRequested) saveDeltaColumn(true);
  let showDelta = $state(deltaRequested || loadDeltaColumn());

  let deltas = $derived.by(() => {
    if (baseline === null || !stats.data?.variables) return null;
    const map = new Map<string, number>();
    for (const [name, value] of Object.entries(stats.data.variables)) {
      const first = baseline[name];
      if (typeof first === "number" && Number.isFinite(first) && Number.isFinite(value)) {
        map.set(name, value - first);
      }
    }
    return map;
  });

  let entries = $derived(buildRows(stats.data?.variables, search, sort, deltas));
  let groups = $derived(groupStatRows(entries));

  let totalCount = $derived(
    stats.data?.variables ? Object.keys(stats.data.variables).length : 0,
  );

  function toggleSort(key: SortKey) {
    sort = nextSortState(sort, key);
  }

  function toggleDescriptions() {
    showDescriptions = !showDescriptions;
    saveDescriptionColumn(showDescriptions);
  }

  function toggleDeltaColumn() {
    showDelta = !showDelta;
    saveDeltaColumn(showDelta);
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
  <PageHeader
    title="Statistics"
    subtitle={statisticsScopeLine(scope.isGlobal, scope.host)}
    updatedAt={updatedAt}
    refresh={{
      autoRefresh: stats.autoRefresh,
      intervalMs: 5000,
      onToggle: toggleAutoRefresh,
      onRefresh: () => stats.refresh(),
    }}
  >
    {#snippet toolbar()}
      <div class="control-group">
      <input
        type="search"
        class="search-input field field-search"
        placeholder="Search name or description..."
        aria-label="Search statistics"
        bind:value={search}
      />
      <span class="count field-text">{entries.length} of {totalCount} variables</span>
      </div>
      <div class="control-group">
      <label class="toggle field-check">
        <input type="checkbox" checked={showDescriptions} onchange={toggleDescriptions} />
        Description column
      </label>
      <label class="toggle field-check">
        <input type="checkbox" checked={showDelta} onchange={toggleDeltaColumn} />
        Δ since open
      </label>
      </div>
    {/snippet}
  </PageHeader>

  <ViewSwitch
    label="Statistics views"
    views={[
      { label: "Table", href: "#/statistics", current: true },
      { label: "Graphs", href: "#/graphs", current: false },
    ]}
  />

  {#if stats.loading}
    <p class="loading">Loading statistics...</p>
  {:else if stats.error && !stats.data}
    <LoadError message={stats.error.message} />
  {:else}
    <RefreshNotice error={stats.error} />

    <!-- On a wide window the groups flow into columns; with descriptions
         shown each column needs more room to stay readable. -->
    <div class="stat-groups" class:stat-groups--described={showDescriptions}>
    {#each groups as group (group.key)}
      <details class="stat-group" open>
        <summary>{group.label} <span class="stat-group-count">{group.rows.length}</span></summary>
        <!-- A table that may scroll sideways is a named region that takes keyboard focus (WCAG 2.1.1). -->
        <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
        <div class="table-wrapper" role="region" aria-label="{group.label} statistics" tabindex="0">
          <table>
            <colgroup>
              <col />
              <col class="value-col" />
              {#if showDelta}<col class="value-col" />{/if}
            </colgroup>
            <thead>
              <tr>
                <SortableHeader label="Name" active={sort.key === "name"} ascending={sort.asc} onsort={() => toggleSort("name")} />
                <SortableHeader label="Value" align="right" active={sort.key === "value"} ascending={sort.asc} onsort={() => toggleSort("value")} />
                {#if showDelta}
                  <SortableHeader label="Δ since open" align="right" active={sort.key === "delta"} ascending={sort.asc} onsort={() => toggleSort("delta")} />
                {/if}
              </tr>
            </thead>
            <tbody>
              {#each group.rows as entry (entry.name)}
                <tr>
                  <td class="name-cell" title={entry.description}>
                    {#each nameSegments(entry.name) as part, i (i)}{part}<wbr />{/each}
                    {#if showDescriptions}<div class="stat-desc">{entry.description}</div>{/if}
                  </td>
                  <td class="value-cell num">{formatCount(entry.value)}</td>
                  {#if showDelta}
                    <td class="value-cell num delta-cell" class:delta-pos={entry.delta !== null && entry.delta > 0} class:delta-neg={entry.delta !== null && entry.delta < 0}>
                      {entry.delta === null ? "—" : `${entry.delta > 0 ? "+" : ""}${formatCount(entry.delta)}`}
                    </td>
                  {/if}
                </tr>
              {:else}
                <tr><td colspan={showDelta ? 3 : 2} class="empty">No matching variables found.</td></tr>
              {/each}
            </tbody>
          </table>
        </div>
      </details>
    {:else}
      <p class="empty">No matching variables found.</p>
    {/each}
    </div>
  {/if}
</div>

<style>
  .search-input:focus {
    outline: none;
    border-color: var(--ps-primary);
    box-shadow: 0 0 0 2px var(--ps-primary-light);
  }

  .toggle {
    color: var(--ps-text-secondary);
    user-select: none;
  }

  .count {
    font-size: var(--ps-font-size-sm);
  }

  .table-wrapper {
    overflow-x: auto;
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
  }

  /* Column minimums: 44rem keeps a name and its value together; with
     descriptions shown, 48rem keeps them to two or three lines. */
  .stat-groups {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(100%, 44rem), 1fr));
    align-items: start;
    column-gap: var(--ps-space-lg);
  }

  .stat-groups--described {
    grid-template-columns: repeat(auto-fit, minmax(min(100%, 48rem), 1fr));
  }

  /* The value columns have one width in every table, so the "Value"
     header sits at the same place in each table of a column. */
  table {
    width: 100%;
    table-layout: fixed;
    border-collapse: collapse;
    font-size: var(--ps-font-size-sm);
  }

  thead {
    background: var(--ps-bg-tertiary);
    position: sticky;
    top: 0;
  }

  td {
    padding: 3px var(--ps-space-md);
    line-height: 1.4;
    border-bottom: 1px solid var(--ps-border-light);
  }

  .value-col {
    width: var(--ps-value-column-width);
  }

  /* A phone keeps the room for the names; a value still fits in full. */
  @media (max-width: 640px) {
    .value-col {
      width: var(--ps-value-column-width-narrow);
    }
  }

  .stat-group {
    min-width: 0;
    margin-bottom: var(--ps-space-md);
  }

  .stat-group summary {
    cursor: pointer;
    padding: var(--ps-space-xs) var(--ps-space-sm);
    font-size: var(--ps-font-size-sm);
    font-weight: 600;
    color: var(--ps-text);
  }

  .stat-group-count {
    margin-left: var(--ps-space-xs);
    font-weight: 500;
    font-size: var(--ps-font-size-xs);
    color: var(--ps-text-secondary);
  }

  .stat-desc {
    margin-top: 2px;
    font-size: var(--ps-font-size-xs);
    color: var(--ps-text-secondary);
  }

  .delta-pos {
    color: var(--ps-success-text);
  }

  .delta-neg {
    color: var(--ps-error);
  }

  tbody tr:nth-child(even) {
    background: var(--ps-bg-secondary);
  }

  tbody tr:hover {
    background: var(--ps-surface-hover);
  }

  .name-cell {
    font-family: var(--ps-font-mono);
    word-break: normal;
    overflow-wrap: break-word;
    cursor: help;
  }

  .value-cell {
    font-family: var(--ps-font-mono);
    text-align: right;
    white-space: nowrap;
  }

  .empty {
    text-align: center;
    color: var(--ps-text-secondary);
    padding: var(--ps-space-xl);
  }

  .loading {
    color: var(--ps-text-secondary);
  }
</style>

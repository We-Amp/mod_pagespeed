<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  // A sortable column header: the sort state on the header cell (aria-sort)
  // and a real button inside it, so it works from the keyboard and screen
  // readers announce the order.
  let {
    label,
    active,
    ascending,
    onsort,
    align = "left",
  }: {
    label: string;
    active: boolean;
    ascending: boolean;
    onsort: () => void;
    align?: "left" | "right";
  } = $props();
</script>

<th
  class="sortable"
  class:right={align === "right"}
  aria-sort={active ? (ascending ? "ascending" : "descending") : "none"}
>
  <button type="button" class="sort-button" onclick={onsort}>
    {label}<span class="sort-indicator" aria-hidden="true">{active ? (ascending ? "▲" : "▼") : "↕"}</span>
  </button>
</th>

<style>
  .sortable {
    padding: var(--ps-space-sm) var(--ps-space-md);
    border-bottom: 2px solid var(--ps-border);
    font-weight: 600;
    text-align: left;
    white-space: nowrap;
  }

  .sortable.right {
    text-align: right;
  }

  .sortable:hover {
    background: var(--ps-surface-hover);
  }

  .sort-button {
    padding: 0;
    border: 0;
    background: none;
    color: inherit;
    font: inherit;
    text-align: inherit;
    cursor: pointer;
  }

  .sort-indicator {
    margin-left: var(--ps-space-xs);
    font-size: 0.65em;
    color: var(--ps-text-secondary);
  }

  .sortable[aria-sort="ascending"] .sort-indicator,
  .sortable[aria-sort="descending"] .sort-indicator {
    color: var(--ps-primary);
  }
</style>

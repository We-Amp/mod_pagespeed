<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  import { nextTabId } from "./utils/tabs";

  interface Tab {
    id: string;
    label: string;
  }

  interface Props {
    tabs: readonly Tab[];
    active: string;
    label: string;
    idPrefix: string;
    panelId: string;
    onselect: (id: string) => void;
  }

  let { tabs, active, label, idPrefix, panelId, onselect }: Props = $props();

  function onkeydown(event: KeyboardEvent) {
    const next = nextTabId(tabs.map((t) => t.id), active, event.key);
    if (next === null) return;
    event.preventDefault();
    onselect(next);
    // Selection follows focus, so move focus to the newly selected tab.
    document.getElementById(`${idPrefix}-tab-${next}`)?.focus();
  }
</script>

<div class="tabs" role="tablist" aria-label={label}>
  {#each tabs as tab (tab.id)}
    <button
      type="button"
      role="tab"
      id="{idPrefix}-tab-{tab.id}"
      class="tab"
      class:active={active === tab.id}
      aria-selected={active === tab.id}
      aria-controls={panelId}
      tabindex={active === tab.id ? 0 : -1}
      onclick={() => onselect(tab.id)}
      {onkeydown}
    >{tab.label}</button>
  {/each}
</div>

<style>
  /* Moved verbatim from the Caches page: the console's one tab style. */
  .tabs {
    display: flex;
    border-bottom: 2px solid var(--ps-border);
    margin-bottom: var(--ps-space-lg);
    gap: 0;
    overflow-x: auto;
  }
  .tab {
    padding: var(--ps-space-sm) var(--ps-space-lg);
    border: none;
    background: none;
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-secondary);
    cursor: pointer;
    border-bottom: 2px solid transparent;
    margin-bottom: -2px;
    white-space: nowrap;
  }
  .tab:hover {
    color: var(--ps-text);
    background: var(--ps-surface-hover);
  }
  .tab.active {
    color: var(--ps-primary);
    border-bottom-color: var(--ps-primary);
    font-weight: 600;
  }
</style>

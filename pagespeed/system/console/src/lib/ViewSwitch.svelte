<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  // Views of one sidebar entry that are separate routes (Statistics: the
  // table and the graphs). Plain links, so each view has its own address,
  // can be shared, and the Back button works.
  interface View {
    label: string;
    href: string;
    current: boolean;
  }

  let { label, views }: { label: string; views: readonly View[] } = $props();
</script>

<nav class="view-switch" aria-label={label}>
  {#each views as view (view.href)}
    <a
      class="view-link"
      class:current={view.current}
      href={view.href}
      aria-current={view.current ? "page" : undefined}
    >{view.label}</a>
  {/each}
</nav>

<style>
  /* The console's one tab look (Tabs.svelte), as links. */
  .view-switch {
    display: flex;
    border-bottom: 2px solid var(--ps-border);
    margin-bottom: var(--ps-space-lg);
    overflow-x: auto;
  }
  .view-link {
    padding: var(--ps-space-sm) var(--ps-space-lg);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-secondary);
    text-decoration: none;
    border-bottom: 2px solid transparent;
    margin-bottom: -2px;
    white-space: nowrap;
  }
  .view-link:hover {
    color: var(--ps-text);
    background: var(--ps-surface-hover);
  }
  .view-link.current {
    color: var(--ps-primary);
    border-bottom-color: var(--ps-primary);
    font-weight: 600;
  }
</style>

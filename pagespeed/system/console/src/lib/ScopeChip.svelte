<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  // A card's scope: whose traffic its numbers cover. The optimizer serves
  // every host from one cache, so its cards are always whole-server. The
  // chip shows only where the scope tells the reader something
  // (utils/scope-chip.ts); hidden, its text still names the card's scope
  // to assistive technology.
  let { label, shown = true }: { label: "this host" | "whole server"; shown?: boolean } = $props();
</script>

{#if shown}
  <span class="scope-chip" data-testid="scope-chip-{label === 'whole server' ? 'server' : 'host'}">{label}</span>
{:else}
  <span class="visually-hidden">{label}</span>
{/if}

<style>
  .scope-chip {
    display: inline-block;
    vertical-align: middle;
    margin-left: var(--ps-space-xs);
    font-size: var(--ps-font-size-xs);
    font-weight: 500;
    color: var(--ps-text-secondary);
    border: 1px solid var(--ps-border);
    border-radius: 999px;
    padding: 0 var(--ps-space-sm);
    line-height: 1.6;
    white-space: nowrap;
  }
</style>

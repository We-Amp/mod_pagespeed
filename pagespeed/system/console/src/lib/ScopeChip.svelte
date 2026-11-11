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
  <span class="badge badge-outline scope-chip" data-testid="scope-chip-{label === 'whole server' ? 'server' : 'host'}">{label}</span>
{:else}
  <span class="visually-hidden">{label}</span>
{/if}

<style>
  .scope-chip {
    margin-left: var(--ps-space-xs);
    vertical-align: middle;
    font-weight: 500;
  }
</style>

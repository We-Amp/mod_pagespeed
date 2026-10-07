<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  // One way to say "there is nothing to show here, and why": the optimizer is
  // not answering, it is warming up, a type has no data yet, a chart is still
  // collecting. A missing number is never shown as 0. `action` is a console
  // route or a fixed documentation link -- never a value from either endpoint.
  let {
    title,
    detail = "",
    action = null,
    testid = "empty-state",
  }: {
    title: string;
    detail?: string;
    action?: { label: string; href: string } | null;
    testid?: string;
  } = $props();
</script>

<div class="empty-state" data-testid={testid}>
  <p class="empty-title">{title}</p>
  {#if detail}
    <p class="empty-detail">{detail}</p>
  {/if}
  {#if action}
    <a class="empty-action" href={action.href}>{action.label}</a>
  {/if}
</div>

<style>
  /* As wide as its text needs, not the window. */
  .empty-state {
    max-width: var(--ps-card-max);
    padding: var(--ps-space-md);
    border: 1px dashed var(--ps-border);
    border-radius: var(--ps-border-radius);
    background: var(--ps-bg-secondary);
    color: var(--ps-text);
    overflow-wrap: anywhere;
  }

  .empty-title {
    margin: 0;
    font-weight: 600;
  }

  .empty-detail {
    margin: var(--ps-space-xs) 0 0;
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-secondary);
  }

  .empty-action {
    display: inline-block;
    margin-top: var(--ps-space-sm);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-primary);
  }
</style>

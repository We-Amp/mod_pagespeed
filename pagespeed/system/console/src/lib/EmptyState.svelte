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

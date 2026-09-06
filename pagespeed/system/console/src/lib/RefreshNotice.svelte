<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  // Inline "last refresh failed" banner. Shown above stale data when a poll
  // fails but we still have previously-fetched data to display, so a transient
  // backend blip no longer blanks the whole page (only the very first fetch,
  // with no data yet, falls back to a full-page error).
  let { error }: { error: Error | string | null } = $props();

  let message = $derived(
    error == null ? "" : typeof error === "string" ? error : error.message,
  );
</script>

{#if error}
  <div class="refresh-notice" role="status">
    <span class="refresh-notice-icon" aria-hidden="true">⚠</span>
    <span>Last refresh failed: {message}. Showing the most recent data.</span>
  </div>
{/if}

<style>
  .refresh-notice {
    display: flex;
    align-items: center;
    gap: var(--ps-space-sm);
    padding: var(--ps-space-sm) var(--ps-space-md);
    margin-bottom: var(--ps-space-md);
    border: 1px solid var(--ps-warning);
    border-radius: var(--ps-border-radius);
    background: var(--ps-bg-secondary);
    color: var(--ps-warning);
    font-size: var(--ps-font-size-sm);
  }

  .refresh-notice-icon {
    flex-shrink: 0;
  }
</style>

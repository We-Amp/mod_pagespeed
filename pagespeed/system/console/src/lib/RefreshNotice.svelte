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
  <div class="notice notice-warning refresh-notice" role="status">
    <span class="refresh-notice-icon" aria-hidden="true">⚠</span>
    <span>Last refresh failed: {message}. Showing the most recent data.</span>
  </div>
{/if}

<style>
  /* The frame and colours are the shared .notice; the icon sits beside the line. */
  .refresh-notice {
    display: flex;
    align-items: center;
    gap: var(--ps-space-sm);
    margin-bottom: var(--ps-space-md);
  }

  .refresh-notice > * + * {
    margin-top: 0;
  }

  .refresh-notice-icon {
    flex-shrink: 0;
    color: var(--ps-warning-text);
  }
</style>

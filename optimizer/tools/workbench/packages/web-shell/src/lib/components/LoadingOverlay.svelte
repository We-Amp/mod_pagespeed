<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  let {
    message = 'Loading...',
  }: {
    message?: string;
  } = $props();
</script>

<div class="loading-overlay" role="status" aria-live="polite">
  <div class="loading-overlay-content">
    <span class="ps-spinner" aria-label={message}></span>
    <span class="loading-overlay-message">{message}</span>
  </div>
</div>

<style>
  .loading-overlay {
    position: absolute;
    inset: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    background: color-mix(in srgb, var(--ps-bg-primary) 80%, transparent);
    z-index: 50;
  }

  .loading-overlay-content {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: var(--ps-space-md);
  }

  .loading-overlay-message {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    color: var(--ps-fg-secondary);
  }
</style>
